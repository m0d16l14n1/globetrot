#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end test of globetrotd against the real kernel input stack.

Needs root and /dev/uinput (run in a VM or container, not on a desktop you are
typing on: the daemon grabs the fake keyboard only, but be safe). It:
  1. creates a fake keyboard with KEY_A and KEY_FN through uinput,
  2. starts globetrotd with tap_mode = both,
  3. injects key sequences and reads the daemon's virtual keyboard and IPC socket.

Usage: sudo python3 tests/e2e_uinput.py build/globetrotd
"""
from __future__ import annotations

import fcntl
import glob
import json
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

EV_SYN, EV_KEY = 0x00, 0x01
SYN_REPORT = 0
KEY_A, KEY_C, KEY_DELETE, KEY_BACKSPACE = 30, 46, 111, 14
KEY_LEFTMETA, KEY_LEFTALT, KEY_F24, KEY_FN = 125, 56, 194, 0x1D0

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_SETUP = 0x405C5503
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
EVIOCGRAB = 0x40044590
EVENT_FMT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FMT)
VIRT_NAME = "Globetrot Virtual Keyboard"
FAKE_NAME = "globetrot-e2e fake keyboard"


_LOG_TAIL: list[str] = []


def die(msg: str) -> None:
    print(f"FAIL: {msg}")
    if _LOG_TAIL:
        print("daemon log (last lines):")
        print("".join(_LOG_TAIL[-15:]), end="")
    sys.exit(1)


class FakeKeyboard:
    def __init__(self) -> None:
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for k in (KEY_A, KEY_C, KEY_DELETE, KEY_BACKSPACE, KEY_LEFTMETA, KEY_LEFTALT, KEY_FN):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        setup = struct.pack("HHHH80sI", 0x06, 0x05AC, 0x0001, 1, FAKE_NAME.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def emit(self, code: int, value: int) -> None:
        os.write(self.fd, struct.pack(EVENT_FMT, 0, 0, EV_KEY, code, value))
        os.write(self.fd, struct.pack(EVENT_FMT, 0, 0, EV_SYN, SYN_REPORT, 0))

    def tap(self, code: int, hold_s: float = 0.03) -> None:
        self.emit(code, 1)
        time.sleep(hold_s)
        self.emit(code, 0)

    def close(self) -> None:
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


def find_event_node(name: str, timeout_s: float = 5.0) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for path in glob.glob("/sys/class/input/event*/device/name"):
            try:
                if open(path).read().strip() == name:
                    node = "/dev/input/" + path.split("/")[4]
                    if os.path.exists(node):
                        return node
            except OSError:
                pass
        time.sleep(0.05)
    die(f"no /dev/input node for {name!r}")
    return ""


def read_keys(fd: int, wait_s: float = 0.6) -> list[tuple[int, int]]:
    keys: list[tuple[int, int]] = []
    deadline = time.monotonic() + wait_s
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return keys
        r, _, _ = select.select([fd], [], [], left)
        if not r:
            return keys
        data = os.read(fd, EVENT_SIZE * 64)
        for off in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE):
            _, _, etype, code, value = struct.unpack_from(EVENT_FMT, data, off)
            if etype == EV_KEY:
                keys.append((code, value))


class DaemonLogKeys:
    """Keys the daemon wrote to uinput, from its --verbose log. Used when the
    virtual keyboard's evdev node cannot be opened (some container sandboxes
    only allow the first input node they see)."""

    def __init__(self, stream) -> None:
        self._lines: list[str] = []
        self._lock = threading.Lock()
        threading.Thread(target=self._pump, args=(stream,), daemon=True).start()

    def _pump(self, stream) -> None:
        for line in stream:
            _LOG_TAIL.append(line)
            with self._lock:
                self._lines.append(line)

    def read(self, wait_s: float = 0.6) -> list[tuple[int, int]]:
        time.sleep(wait_s)
        with self._lock:
            lines, self._lines = self._lines, []
        keys = []
        for line in lines:
            # "globetrotd: uinput key f24 (194) val=1"
            if "uinput key" in line:
                code = int(line.split("(", 1)[1].split(")", 1)[0])
                keys.append((code, int(line.rsplit("val=", 1)[1])))
        return keys


def read_ipc(sock: socket.socket, wait_s: float = 0.6) -> list[dict]:
    out: list[dict] = []
    buf = b""
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        r, _, _ = select.select([sock], [], [], max(0.0, deadline - time.monotonic()))
        if not r:
            break
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    for line in buf.decode().splitlines():
        if line.strip():
            out.append(json.loads(line))
    return out


def main() -> int:
    if os.geteuid() != 0 or not os.path.exists("/dev/uinput"):
        print("SKIP: needs root and /dev/uinput")
        return 0
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/globetrotd")
    work = tempfile.mkdtemp(prefix="globe-e2e-")
    conf = os.path.join(work, "globe.conf")
    sock_path = os.path.join(work, "events.sock")
    status_path = os.path.join(work, "status.json")
    with open(conf, "w") as f:
        f.write("[globe]\ntap_mode = both\ntap_key = f24\ncancel_on_pointer = false\n"
                "[bindings]\nglobe+c = key:leftmeta+leftalt+c\n")

    kbd = FakeKeyboard()
    fake_node = find_event_node(FAKE_NAME)
    proc = subprocess.Popen(
        [daemon, "-v", "-c", conf, "-s", sock_path, "--status-file", status_path, "-d", fake_node],
        stderr=subprocess.PIPE, text=True,
    )
    log_keys = DaemonLogKeys(proc.stderr)
    passed = 0
    try:
        virt_node = find_event_node(VIRT_NAME)
        try:
            vfd = os.open(virt_node, os.O_RDONLY | os.O_NONBLOCK)
            read_virtual = lambda wait_s=0.6: read_keys(vfd, wait_s)  # noqa: E731
            print(f"  reading virtual keyboard from {virt_node}")
        except PermissionError:
            vfd = -1
            read_virtual = log_keys.read
            print(f"  {virt_node} not openable here; reading the daemon's uinput writes from its log")
        for _ in range(100):
            if os.path.exists(sock_path):
                break
            time.sleep(0.05)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(sock_path)
        time.sleep(0.6)  # the daemon waits for keys to be idle, then grabs
        read_virtual(0.1)

        # 1. The fake keyboard is grabbed: a second reader sees nothing.
        status = json.load(open(status_path))
        if not any(k["path"] == fake_node for k in status["keyboards"]):
            die(f"keyboard not grabbed: {status}")
        other = os.open(fake_node, os.O_RDONLY | os.O_NONBLOCK)
        kbd.tap(KEY_A)
        if read_keys(other, 0.3):
            die("grab is not exclusive")
        os.close(other)
        if read_virtual(0.2) != [(KEY_A, 1), (KEY_A, 0)]:
            die("grabbed key was not re-emitted")
        passed += 1
        print("  [PASS] fake keyboard grabbed exclusively")

        # 2. Plain typing passes through the virtual keyboard.
        kbd.tap(KEY_A)
        got = read_virtual()
        if got != [(KEY_A, 1), (KEY_A, 0)]:
            die(f"plain key passthrough: {got}")
        passed += 1
        print("  [PASS] plain key passes through")

        # 3. Solo Globe tap: F24 tap on the virtual keyboard + globe_tap IPC event.
        kbd.tap(KEY_FN, 0.05)
        got = read_virtual()
        ev = read_ipc(sock)
        if got != [(KEY_F24, 1), (KEY_F24, 0)]:
            die(f"solo tap output: {got}")
        if not any(e["type"] == "globe_tap" and e["action"] == "switch_layout" for e in ev):
            die(f"solo tap IPC: {ev}")
        if any(c == KEY_FN for c, _ in got):
            die("raw KEY_FN leaked")
        passed += 1
        print("  [PASS] solo Globe tap -> F24 + switch_layout")

        # 4. Globe held past tap_timeout_ms (400 ms): debounced, nothing emitted.
        kbd.tap(KEY_FN, 0.55)
        got = read_virtual()
        ev = read_ipc(sock)
        if got or not any(e["type"] == "globe_debounced" for e in ev):
            die(f"long hold: keys={got} ipc={ev}")
        passed += 1
        print("  [PASS] long hold is debounced")

        # 5. Globe + Delete (kernel Fn layer): pure Delete, tap cancelled.
        kbd.emit(KEY_FN, 1)
        kbd.tap(KEY_DELETE)
        kbd.emit(KEY_FN, 0)
        got = read_virtual()
        ev = read_ipc(sock)
        if got != [(KEY_DELETE, 1), (KEY_DELETE, 0)]:
            die(f"fn layer passthrough: {got}")
        if any(e["type"] == "globe_tap" for e in ev):
            die("Fn-layer combo also fired a tap")
        passed += 1
        print("  [PASS] Globe+Delete stays a pure Delete, no layout switch")

        # 6. Bound chord: Globe+C -> Super+Alt+C, all released.
        kbd.emit(KEY_FN, 1)
        kbd.tap(KEY_C)
        kbd.emit(KEY_FN, 0)
        got = read_virtual()
        downs = [c for c, v in got if v == 1]
        ups = [c for c, v in got if v == 0]
        if sorted(downs) != sorted([KEY_LEFTMETA, KEY_LEFTALT, KEY_C]) or sorted(ups) != sorted(downs):
            die(f"chord binding: {got}")
        passed += 1
        print("  [PASS] Globe+C binding -> Super+Alt+C, nothing stuck")

        # 7. SIGHUP reload keeps working; SIGTERM exits cleanly and drops the device.
        proc.send_signal(signal.SIGHUP)
        time.sleep(0.3)
        kbd.tap(KEY_A)
        if read_virtual() != [(KEY_A, 1), (KEY_A, 0)]:
            die("typing after SIGHUP reload")
        proc.send_signal(signal.SIGTERM)
        rc = proc.wait(timeout=5)
        if rc != 0:
            die(f"daemon exit code {rc}")
        if os.path.exists(sock_path) or os.path.exists(status_path):
            die("socket/status file left behind")
        passed += 1
        print("  [PASS] SIGHUP reload, clean SIGTERM shutdown")
        if vfd >= 0:
            os.close(vfd)
        sock.close()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        kbd.close()
    print(f"All {passed} end-to-end checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
