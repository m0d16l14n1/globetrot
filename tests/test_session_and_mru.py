# SPDX-License-Identifier: MIT
"""Unit & integration tests for globetrot MRU switcher, compositor backends, IPC, and CLI."""
from __future__ import annotations

import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

from globetrot.backends import (  # noqa: E402
    Fcitx5Backend,
    GnomeBackend,
    HyprlandBackend,
    KWinBackend,
    LayoutBackend,
    LayoutSnapshot,
    NiriBackend,
    SwayBackend,
    default_runner,
    launch_user_command,
)
from globetrot import ctl  # noqa: E402
from globetrot.mru import MruLayoutEngine  # noqa: E402
from globetrot.session import GlobeSessionDispatcher, SessionConfig  # noqa: E402


class FakeBackend(LayoutBackend):
    name = "fake"

    def __init__(self, layouts: list[str], initial: int = 0) -> None:
        super().__init__()
        self.layouts = list(layouts)
        self.current = initial
        self.history: list[int] = []

    def is_available(self) -> bool:
        return True

    def get_snapshot(self) -> LayoutSnapshot | None:
        return LayoutSnapshot(backend=self.name, layouts=list(self.layouts), current_index=self.current)

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        self.current = index
        self.history.append(index)
        return True


class TestMruLayoutEngine(unittest.TestCase):
    def test_two_layouts_toggle_immediately(self) -> None:
        mru = MruLayoutEngine(commit_window_s=1.0)
        # EN(0) <-> RU(1)
        self.assertEqual(mru.next_layout(2, 0, now_s=1.0), 1)
        self.assertEqual(mru.next_layout(2, 1, now_s=1.1), 0)
        self.assertEqual(mru.next_layout(2, 0, now_s=1.2), 1)
        self.assertEqual(mru.next_layout(2, 1, now_s=5.0), 0)

    def test_three_layouts_mru_cycling_and_commit(self) -> None:
        # Layouts: 0=EN, 1=RU, 2=DE
        mru = MruLayoutEngine(commit_window_s=1.0)

        # Rapid double tap within 1.0s cycles EN(0) -> RU(1) -> DE(2)
        t1 = mru.next_layout(3, 0, now_s=10.0)
        self.assertEqual(t1, 1)
        t2 = mru.next_layout(3, 1, now_s=10.3)
        self.assertEqual(t2, 2)

        # Wait > 1.0s -> DE(2) commits as MRU head ([2, 0, 1]).
        # Next single tap switches back to previous head EN(0).
        t3 = mru.next_layout(3, 2, now_s=12.0)
        self.assertEqual(t3, 0)

        # Wait > 1.0s -> EN(0) commits as MRU head ([0, 2, 1]).
        # Next single tap toggles directly back to DE(2) (not RU(1)).
        t4 = mru.next_layout(3, 0, now_s=14.0)
        self.assertEqual(t4, 2)

    def test_reverse_cycle(self) -> None:
        mru = MruLayoutEngine(commit_window_s=1.0)
        # Initial MRU [0, 1, 2]; reverse from 0 selects last (2).
        self.assertEqual(mru.next_layout(3, 0, now_s=1.0, reverse=True), 2)

    def test_external_layout_change_reconciled(self) -> None:
        mru = MruLayoutEngine(commit_window_s=1.0)
        # Tap once during uncommitted cycle: 0 -> 1 (MRU head is still 0)
        self.assertEqual(mru.next_layout(3, 0, now_s=1.0), 1)
        # User manually clicked DE(2) in taskbar at t=1.2s -> interrupted cycle
        # is discarded (1 is NOT falsely promoted), so next tap returns to 0!
        nxt = mru.next_layout(3, 2, now_s=1.2)
        self.assertEqual(nxt, 0)


class TestCompositorBackends(unittest.TestCase):
    def test_kwin_busctl_json_backend(self) -> None:
        calls: list[list[str]] = []

        def fake_run(cmd):
            calls.append(list(cmd))
            if "getLayout" in cmd:
                return subprocess.CompletedProcess(cmd, 0, json.dumps({"type": "u", "data": [1]}), "")
            if "getLayoutsList" in cmd:
                payload = {
                    "type": "a(sss)",
                    "data": [[["us", "", "English (US)"], ["ru", "", "Russian"]]],
                }
                return subprocess.CompletedProcess(cmd, 0, json.dumps(payload), "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        b = KWinBackend(runner=fake_run)
        b._tool = "busctl"
        snap = b.get_snapshot()
        self.assertIsNotNone(snap)
        assert snap is not None
        self.assertEqual(snap.layouts, ["English (US)", "Russian"])
        self.assertEqual(snap.current_index, 1)
        self.assertTrue(b.set_layout(0, snap))
        self.assertEqual(calls[-1][-3:], ["setLayout", "u", "0"])

    def test_kwin_gdbus_backend(self) -> None:
        calls: list[list[str]] = []

        def fake_run(cmd):
            calls.append(list(cmd))
            if cmd[-1].endswith(".getLayout"):
                return subprocess.CompletedProcess(cmd, 0, "(uint32 1,)\n", "")
            if cmd[-1].endswith(".getLayoutsList"):
                return subprocess.CompletedProcess(
                    cmd, 0, "([('us', '', 'English (US)'), ('ru', 'dos', 'Russian')],)\n", ""
                )
            return subprocess.CompletedProcess(cmd, 0, "()\n", "")

        b = KWinBackend(runner=fake_run)
        b._tool = "gdbus"
        snap = b.get_snapshot()
        self.assertIsNotNone(snap)
        assert snap is not None
        self.assertEqual(snap.layouts, ["English (US)", "Russian"])
        self.assertEqual(snap.current_index, 1)
        self.assertTrue(b.set_layout(0, snap))
        self.assertEqual(calls[-1][-2:], ["org.kde.KeyboardLayouts.setLayout", "0"])

    def test_hyprland_backend_switches_all_and_prefers_virtual_snapshot(self) -> None:
        calls: list[list[str]] = []
        devices = {
            "keyboards": [
                {"name": "internal-spi-keyboard", "layout": "us,ru", "active_layout_index": 0, "main": False},
                {"name": "globetrot-virtual-keyboard", "layout": "us,ru", "active_layout_index": 0, "main": True},
            ]
        }

        def fake_run(cmd):
            calls.append(list(cmd))
            if cmd[:2] == ["hyprctl", "-j"]:
                return subprocess.CompletedProcess(cmd, 0, json.dumps(devices), "")
            return subprocess.CompletedProcess(cmd, 0, "ok\n", "")

        b = HyprlandBackend(runner=fake_run)
        snap = b.get_snapshot()
        self.assertIsNotNone(snap)
        assert snap is not None
        self.assertEqual(snap.device_name, "globetrot-virtual-keyboard")
        self.assertEqual(snap.layouts, ["us", "ru"])
        self.assertTrue(b.set_layout(1, snap))
        self.assertEqual(calls[-1], ["hyprctl", "switchxkblayout", "all", "1"])

    def test_niri_and_sway_and_gnome_and_fcitx5(self) -> None:
        def fake_niri(cmd):
            if "keyboard-layouts" in cmd:
                return subprocess.CompletedProcess(cmd, 0, json.dumps({"names": ["English", "Russian"], "current_idx": 0}), "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        nb = NiriBackend(runner=fake_niri)
        ns = nb.get_snapshot()
        assert ns is not None
        self.assertEqual(ns.layouts, ["English", "Russian"])
        self.assertTrue(nb.set_layout(1, ns))

        def fake_sway(cmd):
            if "get_inputs" in cmd:
                return subprocess.CompletedProcess(
                    cmd, 0, json.dumps([{"type": "keyboard", "xkb_layout_names": ["US", "RU"], "xkb_active_layout_index": 1}]), ""
                )
            return subprocess.CompletedProcess(cmd, 0, "", "")

        sb = SwayBackend(runner=fake_sway)
        ss = sb.get_snapshot()
        assert ss is not None
        self.assertEqual(ss.current_index, 1)
        self.assertTrue(sb.set_layout(0, ss))

        gnome_calls: list[list[str]] = []

        def fake_gnome(cmd):
            gnome_calls.append(list(cmd))
            if cmd[-1] == "sources":
                return subprocess.CompletedProcess(cmd, 0, "[('xkb', 'us'), ('xkb', 'ru')]\n", "")
            if cmd[-1] == "current":
                return subprocess.CompletedProcess(cmd, 0, "uint32 0\n", "")
            return subprocess.CompletedProcess(cmd, 0, "", "")

        gb = GnomeBackend(runner=fake_gnome)
        gs = gb.get_snapshot()
        assert gs is not None
        self.assertEqual(gs.layouts, ["us", "ru"])
        self.assertEqual(gs.current_index, 0)
        self.assertTrue(gb.set_layout(1, gs))
        # Verify GNOME writes mru-sources (modern GNOME Shell) and current
        set_keys = [c[-2] for c in gnome_calls if len(c) >= 3 and c[1] == "set"]
        self.assertIn("mru-sources", set_keys)
        self.assertIn("current", set_keys)

        fb = Fcitx5Backend(runner=lambda cmd: subprocess.CompletedProcess(cmd, 0, "keyboard-us\n", ""))
        self.assertTrue(fb.switch_next())


class TestSessionDispatcherAndCli(unittest.TestCase):
    def test_dispatcher_mru_events(self) -> None:
        now = [10.0]
        backend = FakeBackend(["US", "RU", "DE"], initial=0)
        disp = GlobeSessionDispatcher(SessionConfig(mru_commit_ms=1000), backend=backend, clock=lambda: now[0])

        self.assertTrue(disp.handle_event({"type": "globe_tap", "mode": "layout", "action": "switch_layout"}))
        self.assertEqual(backend.current, 1)

        now[0] = 10.2
        self.assertTrue(disp.handle_event({"type": "globe_tap", "mode": "layout", "action": "switch_layout"}))
        self.assertEqual(backend.current, 2)

        # Debounced event does nothing
        self.assertFalse(disp.handle_event({"type": "globe_debounced", "mode": "layout"}))
        self.assertEqual(backend.current, 2)

        # After 1.5s, DE(2) is committed; chord switches back to previous MRU layout
        now[0] = 12.0
        self.assertTrue(disp.handle_event({"type": "globe_chord", "action": "action:switch_layout"}))
        self.assertEqual(backend.current, 0)

    def test_cli_set_bind_unbind_and_status_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cfg_path = str(Path(td) / "globe.conf")
            status_path = Path(td) / "status.json"
            status_path.write_text(
                json.dumps(
                    {
                        "tap_mode": "both",
                        "tap_timeout_ms": 280,
                        "bindings": 1,
                        "ipc_clients": 2,
                        "keyboards": [{"path": "/dev/input/event3", "name": "Internal Keyboard"}],
                        "pointers": [{"path": "/dev/input/event4", "name": "Touchpad"}],
                    }
                ),
                encoding="utf-8",
            )

            rc = ctl.main([
                "-c", cfg_path, "set",
                "--tap-mode", "both",
                "--tap-timeout-ms", "280",
                "--combo-modifier", "super_alt",
                "--no-reload",
            ])
            self.assertEqual(rc, 0)
            rc = ctl.main(["-c", cfg_path, "bind", "e", "action:emoji", "--no-reload"])
            self.assertEqual(rc, 0)
            text = Path(cfg_path).read_text(encoding="utf-8")
            self.assertIn("tap_mode = both", text)
            self.assertIn("tap_timeout_ms = 280", text)
            self.assertIn("globe+e = action:emoji", text)

            buf = io.StringIO()
            with redirect_stdout(buf):
                rc = ctl.main(["-c", cfg_path, "--status-file", str(status_path), "status"])
            self.assertEqual(rc, 0)
            out = buf.getvalue()
            self.assertIn("Internal Keyboard", out)
            self.assertIn("Session clients: 2", out)

            rc = ctl.main(["-c", cfg_path, "unbind", "globe+e", "--no-reload"])
            self.assertEqual(rc, 0)
            self.assertNotIn("globe+e", Path(cfg_path).read_text(encoding="utf-8"))

    def test_unix_socket_jsonl_delivery(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sock_path = str(Path(td) / "events.sock")
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(sock_path)
            srv.listen(1)

            def producer():
                conn, _ = srv.accept()
                conn.sendall(b'{"type":"globe_tap","mode":"layout","action":"switch_layout"}\n')
                conn.close()
                srv.close()

            th = threading.Thread(target=producer, daemon=True)
            th.start()

            backend = FakeBackend(["US", "RU"], initial=0)
            disp = GlobeSessionDispatcher(SessionConfig(), backend=backend)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as cli:
                cli.connect(sock_path)
                for line in cli.makefile("r", encoding="utf-8"):
                    disp.handle_event(json.loads(line))
            th.join(timeout=2.0)
            self.assertEqual(backend.current, 1)

    def test_runner_and_command_launcher_resilience(self) -> None:
        r = default_runner(["__gt_nonexistent_binary_xyz__"])
        self.assertEqual(r.returncode, 127)
        self.assertFalse(launch_user_command("exec:'unterminated quote"))
        self.assertFalse(launch_user_command("exec:__gt_nonexistent_binary_xyz__"))

    def test_session_daemon_prompt_sigterm_shutdown(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            sock_path = str(Path(td) / "events.sock")
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(sock_path)
            srv.listen(1)

            accepted = threading.Event()
            holder: list[socket.socket] = []

            def accept_one() -> None:
                try:
                    conn, _ = srv.accept()
                    holder.append(conn)
                    accepted.set()
                except OSError:
                    pass

            th = threading.Thread(target=accept_one, daemon=True)
            th.start()

            proc = subprocess.Popen(
                [sys.executable, str(ROOT / "bin" / "globetrot-session"), "-s", sock_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                self.assertTrue(accepted.wait(timeout=3.0), "session agent did not connect in time")
                proc.terminate()
                rc = proc.wait(timeout=2.0)
                self.assertEqual(rc, 0)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=2.0)
                for c in holder:
                    c.close()
                srv.close()
                th.join(timeout=1.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
