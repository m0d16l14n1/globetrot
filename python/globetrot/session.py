# SPDX-License-Identifier: MIT
"""globetrot-session: Unprivileged user-session agent for MRU layout switching & Globe actions."""
from __future__ import annotations

import json
import logging
import os
from pathlib import Path
import selectors
import signal
import socket
import sys
import time
from typing import Any

from .backends import (
    LayoutBackend,
    detect_backend,
    launch_emoji_picker,
    launch_user_command,
)
from .ini import IniDoc
from .mru import MruLayoutEngine

DEFAULT_SOCKET_PATH = "/run/globetrot/events.sock"
DEFAULT_SYSTEM_CONFIG = "/etc/globetrot.conf"
DEFAULT_USER_CONFIG = os.path.expanduser("~/.config/globetrot.conf")

log = logging.getLogger("globetrot-session")


class SessionConfig:
    def __init__(
        self,
        backend: str = "auto",
        mru_commit_ms: int = 1000,
        emoji_command: str = "",
    ) -> None:
        self.backend = backend
        self.mru_commit_ms = max(100, min(mru_commit_ms, 5000))
        self.emoji_command = emoji_command

    @classmethod
    def load(cls, paths: list[str] | None = None) -> "SessionConfig":
        """Read [session] from the system config, then the user's override file."""
        cfg = cls()
        candidates = paths if paths is not None else [DEFAULT_SYSTEM_CONFIG, DEFAULT_USER_CONFIG]
        for p in candidates:
            if not p or not Path(p).is_file():
                continue
            try:
                doc = IniDoc(Path(p).read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError) as exc:
                log.warning("Failed to read %s: %s", p, exc)
                continue
            sec = dict(doc.items("session"))
            if sec.get("backend"):
                cfg.backend = sec["backend"]
            if "mru_commit_ms" in sec:
                try:
                    cfg.mru_commit_ms = max(100, min(int(sec["mru_commit_ms"]), 5000))
                except ValueError:
                    log.warning("%s: invalid mru_commit_ms %r", p, sec["mru_commit_ms"])
            if "emoji_command" in sec:
                cfg.emoji_command = sec["emoji_command"]
        return cfg


class GlobeSessionDispatcher:
    """Processes JSONL IPC events from globetrotd and executes desktop actions."""

    def __init__(
        self,
        cfg: SessionConfig,
        backend: LayoutBackend | None = None,
        clock=time.monotonic,
    ) -> None:
        self.cfg = cfg
        self.backend = backend or detect_backend(cfg.backend)
        self.mru = MruLayoutEngine(commit_window_s=cfg.mru_commit_ms / 1000.0)
        self._clock = clock

    def switch_layout(self, *, reverse: bool = False) -> bool:
        if self.backend.name == "none":
            self.backend = detect_backend(self.cfg.backend)
        snap = self.backend.get_snapshot()
        if snap and len(snap.layouts) >= 2:
            target = self.mru.next_layout(
                total=len(snap.layouts),
                current_idx=snap.current_index,
                now_s=self._clock(),
                reverse=reverse,
            )
            if self.backend.set_layout(target, snap):
                log.debug(
                    "Switched layout (%s): %d -> %d (%s)",
                    snap.backend,
                    snap.current_index,
                    target,
                    snap.layouts[target] if target < len(snap.layouts) else "?",
                )
                return True
        return self.backend.switch_next()

    def handle_event(self, ev: dict[str, Any]) -> bool:
        ev_type = ev.get("type", "")
        action = str(ev.get("action", "")).strip()

        if ev_type in ("globe_tap", "globe_double_tap"):
            mode = str(ev.get("mode", "")).strip()
            if mode in ("layout", "both") or action == "switch_layout":
                return self.switch_layout(reverse=False)
            if mode == "emoji" or action == "emoji":
                return launch_emoji_picker(self.cfg.emoji_command)
            if mode == "command" and action:
                return launch_user_command(action)
            return False

        if ev_type == "globe_chord":
            if action == "action:switch_layout":
                return self.switch_layout(reverse=False)
            if action == "action:prev_layout":
                return self.switch_layout(reverse=True)
            if action == "action:emoji":
                return launch_emoji_picker(self.cfg.emoji_command)
            if action.startswith("exec:"):
                return launch_user_command(action)
            return False

        return False


# Where desktop tools (hyprctl, niri, busctl, ...) live. A user service can start
# with a minimal PATH (notably on NixOS), so add the usual locations that exist.
_EXTRA_PATH_DIRS = (
    "/run/current-system/sw/bin",
    "/etc/profiles/per-user/{user}/bin",
    "{home}/.nix-profile/bin",
    "{home}/.local/bin",
    "/usr/local/bin",
    "/usr/bin",
    "/bin",
)


def _extend_path() -> None:
    user = os.environ.get("USER", "")
    home = os.path.expanduser("~")
    parts = [p for p in os.environ.get("PATH", "").split(os.pathsep) if p]
    for tmpl in _EXTRA_PATH_DIRS:
        d = tmpl.format(user=user, home=home)
        if d not in parts and os.path.isdir(d):
            parts.append(d)
    os.environ["PATH"] = os.pathsep.join(parts)


def run_session_daemon(socket_path: str = DEFAULT_SOCKET_PATH, verbose: bool = False) -> int:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="globetrot-session: %(levelname)s: %(message)s",
    )
    _extend_path()
    cfg = SessionConfig.load()
    dispatcher = GlobeSessionDispatcher(cfg)
    log.info("started (backend=%s, mru_commit_ms=%d, sock=%s)", dispatcher.backend.name, cfg.mru_commit_ms, socket_path)

    stop = False
    reload_pending = False
    wake_r, wake_w = os.pipe()
    os.set_blocking(wake_r, False)
    os.set_blocking(wake_w, False)

    def _on_sig(signum: int, _frame: Any) -> None:
        nonlocal stop, reload_pending
        if signum == signal.SIGHUP:
            reload_pending = True
        else:
            stop = True
        try:
            os.write(wake_w, b"x")
        except OSError:
            pass

    def _drain_wake_and_reload() -> None:
        nonlocal reload_pending
        while True:
            try:
                if not os.read(wake_r, 64):
                    break
            except OSError:
                break
        if reload_pending:
            reload_pending = False
            new_cfg = SessionConfig.load()
            dispatcher.cfg = new_cfg
            dispatcher.backend = detect_backend(new_cfg.backend)
            dispatcher.mru.commit_window_s = new_cfg.mru_commit_ms / 1000.0
            log.info("reloaded config (backend=%s)", dispatcher.backend.name)

    prev_sigint = signal.signal(signal.SIGINT, _on_sig)
    prev_sigterm = signal.signal(signal.SIGTERM, _on_sig)
    prev_sighup = signal.signal(signal.SIGHUP, _on_sig)

    backoff = 0.5
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(wake_r, selectors.EVENT_READ, data="wake")
            while not stop:
                _drain_wake_and_reload()
                if stop:
                    break
                try:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                        sock.connect(socket_path)
                        sock.setblocking(False)
                        backoff = 0.5
                        log.info("connected to %s", socket_path)
                        sel.register(sock, selectors.EVENT_READ, data="sock")
                        buf = b""
                        try:
                            while not stop:
                                for key, _ in sel.select():
                                    if key.data == "wake":
                                        _drain_wake_and_reload()
                                        if stop:
                                            break
                                    elif key.data == "sock":
                                        try:
                                            chunk = sock.recv(4096)
                                        except BlockingIOError:
                                            continue
                                        if not chunk:
                                            raise ConnectionResetError("daemon closed IPC socket")
                                        buf += chunk
                                        if len(buf) > 65536:
                                            buf = b""
                                            continue
                                        while b"\n" in buf:
                                            raw_line, buf = buf.split(b"\n", 1)
                                            line = raw_line.decode("utf-8", errors="replace").strip()
                                            if not line:
                                                continue
                                            try:
                                                ev = json.loads(line)
                                            except ValueError:
                                                continue
                                            dispatcher.handle_event(ev)
                        finally:
                            try:
                                sel.unregister(sock)
                            except (KeyError, ValueError):
                                pass
                except OSError as exc:
                    if stop:
                        break
                    log.debug("waiting for %s (%s); retry in %.1fs", socket_path, exc, backoff)
                    events = sel.select(timeout=backoff)
                    if events:
                        _drain_wake_and_reload()
                    backoff = min(backoff * 1.5, 5.0)
    finally:
        signal.signal(signal.SIGINT, prev_sigint)
        signal.signal(signal.SIGTERM, prev_sigterm)
        signal.signal(signal.SIGHUP, prev_sighup)
        os.close(wake_r)
        os.close(wake_w)
    log.info("stopped cleanly")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    sock = DEFAULT_SOCKET_PATH
    verbose = False
    i = 0
    while i < len(args):
        if args[i] in ("-s", "--socket") and i + 1 < len(args):
            sock = args[i + 1]
            i += 2
        elif args[i] in ("-v", "--verbose"):
            verbose = True
            i += 1
        elif args[i] in ("-h", "--help"):
            print("Usage: globetrot-session [-s SOCKET] [-v]")
            return 0
        else:
            print(f"Unknown option: {args[i]}", file=sys.stderr)
            return 2
    return run_session_daemon(sock, verbose=verbose)
