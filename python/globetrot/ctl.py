# SPDX-License-Identifier: MIT
"""globetrot: CLI configuration, validation, monitoring, and diagnostics."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

from .backends import detect_backend
from .ini import IniDoc
from .session import DEFAULT_SOCKET_PATH, DEFAULT_SYSTEM_CONFIG, GlobeSessionDispatcher, SessionConfig

DEFAULT_STATUS_PATH = "/run/globetrot/status.json"


def _load_ini(path: str) -> IniDoc:
    p = Path(path)
    return IniDoc(p.read_text(encoding="utf-8") if p.is_file() else "")


def _write_ini(path: str, doc: IniDoc) -> bool:
    p = Path(path)
    tmp_path: Path | None = None
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=str(p.parent),
            prefix=f".{p.name}.",
            suffix=".tmp",
            delete=False,
        ) as f:
            tmp_path = Path(f.name)
            f.write(doc.text())
        # Validate with globetrotd --check-config when installed
        daemon_bin = shutil.which("globetrotd")
        if daemon_bin:
            r = subprocess.run([daemon_bin, "--check-config", "-c", str(tmp_path)], capture_output=True, text=True, check=False)
            if r.returncode != 0:
                tmp_path.unlink(missing_ok=True)
                print(r.stderr.strip() or "Invalid configuration", file=sys.stderr)
                return False
        os.chmod(tmp_path, 0o644)
        os.replace(tmp_path, p)
        return True
    except PermissionError:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)
        print(f"ERROR: permission denied writing {path} (re-run with sudo)", file=sys.stderr)
        return False
    except OSError as exc:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)
        print(f"ERROR: failed writing {path}: {exc}", file=sys.stderr)
        return False


def _reload_services() -> None:
    subprocess.run(["systemctl", "reload-or-restart", "globetrotd.service"], check=False, stderr=subprocess.DEVNULL)
    sudo_user = os.environ.get("SUDO_USER", "").strip()
    if hasattr(os, "geteuid") and os.geteuid() == 0 and sudo_user and sudo_user != "root":
        subprocess.run(
            ["systemctl", "--user", "-M", f"{sudo_user}@.host", "reload-or-restart", "globetrot-session.service"],
            check=False,
            stderr=subprocess.DEVNULL,
        )
    else:
        subprocess.run(
            ["systemctl", "--user", "reload-or-restart", "globetrot-session.service"],
            check=False,
            stderr=subprocess.DEVNULL,
        )


def cmd_status(args: argparse.Namespace) -> int:
    cp = _load_ini(args.config)
    sess_cfg = SessionConfig.load([args.config])
    backend = detect_backend(sess_cfg.backend)
    snap = backend.get_snapshot()

    live = None
    if Path(args.status_file).is_file():
        try:
            live = json.loads(Path(args.status_file).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            live = None

    print("=== globetrot status ===")
    print(f"Config file:     {args.config} ({'present' if Path(args.config).is_file() else 'defaults'})")
    print(f"IPC socket:      {args.socket} ({'listening' if Path(args.socket).exists() else 'not running'})")
    print(f"Tap mode:        {cp.get('globe', 'tap_mode', 'layout')}")
    print(f"Tap timeout:     {cp.get('globe', 'tap_timeout_ms', '400')} ms")
    print(f"Tap key:         {cp.get('globe', 'tap_key', 'f24')}")
    print(f"Combo modifier:  {cp.get('globe', 'combo_modifier', 'none')}")
    print(f"Rollover safe:   {cp.get('globe', 'rollover_safe', 'true')}")
    print(f"Pointer cancel:  {cp.get('globe', 'cancel_on_pointer', 'true')}")
    print(f"Desktop backend: {backend.name}")
    if snap:
        print(f"Layouts ({len(snap.layouts)}):     {', '.join(snap.layouts)} (active: #{snap.current_index})")
    else:
        print("Layouts:         unavailable (run inside graphical desktop session)")

    if live:
        kbds = [f"{k['path']} ({k['name']})" for k in live.get("keyboards", [])]
        ptrs = live.get("pointers", [])
        print(f"Grabbed kbds:    {', '.join(kbds) if kbds else 'none'}")
        print(f"Observed ptrs:   {len(ptrs)}")
        print(f"Session clients: {live.get('ipc_clients', 0)}")

    if cp.items("bindings"):
        print("\nCustom Globe bindings:")
        for k, v in cp.items("bindings"):
            print(f"  {k} = {v}")
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    daemon_bin = shutil.which("globetrotd")
    if not daemon_bin:
        print("ERROR: globetrotd binary not found in PATH", file=sys.stderr)
        return 1
    r = subprocess.run([daemon_bin, "--check-config", "-c", args.config], check=False)
    return r.returncode


def cmd_set(args: argparse.Namespace) -> int:
    cp = _load_ini(args.config)
    changed = False
    mapping = {
        "tap_mode": args.tap_mode,
        "tap_timeout_ms": str(args.tap_timeout_ms) if args.tap_timeout_ms is not None else None,
        "tap_key": args.tap_key,
        "tap_command": args.tap_command,
        "double_tap_mode": args.double_tap_mode,
        "double_tap_window_ms": str(args.double_tap_window_ms) if args.double_tap_window_ms is not None else None,
        "combo_modifier": args.combo_modifier,
        "rollover_safe": args.rollover_safe,
        "cancel_on_pointer": args.cancel_on_pointer,
    }
    for k, v in mapping.items():
        if v is not None:
            cp.set("globe", k, v)
            changed = True
    if args.backend is not None:
        cp.set("session", "backend", args.backend)
        changed = True
    if args.mru_commit_ms is not None:
        cp.set("session", "mru_commit_ms", str(args.mru_commit_ms))
        changed = True
    if not changed:
        print("No options specified; nothing changed.", file=sys.stderr)
        return 1
    if not _write_ini(args.config, cp):
        return 1
    if not args.no_reload:
        _reload_services()
    print(f"Updated {args.config}")
    return 0


def cmd_bind(args: argparse.Namespace) -> int:
    cp = _load_ini(args.config)
    trigger = args.trigger.strip().lower()
    if not trigger.startswith(("globe+", "fn+")):
        trigger = "globe+" + trigger
    cp.set("bindings", trigger, args.target.strip())
    if not _write_ini(args.config, cp):
        return 1
    if not args.no_reload:
        _reload_services()
    print(f"Bound {trigger} = {args.target.strip()}")
    return 0


def cmd_unbind(args: argparse.Namespace) -> int:
    cp = _load_ini(args.config)
    trigger = args.trigger.strip().lower()
    removed = cp.remove("bindings", trigger)
    if not removed and not trigger.startswith(("globe+", "fn+")):
        removed = cp.remove("bindings", "globe+" + trigger)
    if not removed:
        print(f"No binding found for {args.trigger}", file=sys.stderr)
        return 1
    if not _write_ini(args.config, cp):
        return 1
    if not args.no_reload:
        _reload_services()
    print(f"Unbound {args.trigger}")
    return 0


def cmd_monitor(args: argparse.Namespace) -> int:
    print(f"Monitoring Globe events on {args.socket} (Ctrl+C to stop)...")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.connect(args.socket)
            f = sock.makefile("r", encoding="utf-8", errors="replace")
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                    print(
                        f"[{ev.get('type')}] mode={ev.get('mode')} elapsed={ev.get('elapsed_ms')}ms "
                        f"key={ev.get('key')} action={ev.get('action')!r}"
                    )
                except ValueError:
                    print(line)
    except KeyboardInterrupt:
        return 0
    except OSError as exc:
        print(f"ERROR: cannot connect to {args.socket}: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_test_switch(args: argparse.Namespace) -> int:
    cfg = SessionConfig.load([args.config])
    disp = GlobeSessionDispatcher(cfg)
    ok = disp.switch_layout(reverse=args.reverse)
    print(f"switch_layout(backend={disp.backend.name}, reverse={args.reverse}) -> {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="globetrot", description="Configure and inspect Globetrot/Fn key engine")
    p.add_argument("-c", "--config", default=DEFAULT_SYSTEM_CONFIG, help="Config path")
    p.add_argument("-s", "--socket", default=DEFAULT_SOCKET_PATH, help="Daemon IPC socket path")
    p.add_argument("--status-file", default=DEFAULT_STATUS_PATH, help="Daemon runtime status JSON path")
    sub = p.add_subparsers(dest="cmd", required=True)

    s_st = sub.add_parser("status", help="Show daemon, config, and desktop layout status")
    s_st.set_defaults(func=cmd_status)

    s_val = sub.add_parser("validate", help="Validate configuration file syntax")
    s_val.set_defaults(func=cmd_validate)

    s_set = sub.add_parser("set", help="Update Globe configuration options")
    s_set.add_argument("--tap-mode", choices=["disabled", "layout", "key", "both", "emoji", "command"])
    s_set.add_argument("--tap-timeout-ms", type=int)
    s_set.add_argument("--tap-key")
    s_set.add_argument("--tap-command")
    s_set.add_argument("--double-tap-mode", choices=["disabled", "layout", "key", "emoji", "command"])
    s_set.add_argument("--double-tap-window-ms", type=int)
    s_set.add_argument("--combo-modifier", help="none | super_alt | ctrl_alt_super | hyper | hyper_chord | rightalt | <chord>")
    s_set.add_argument("--rollover-safe", choices=["true", "false"])
    s_set.add_argument("--cancel-on-pointer", choices=["true", "false"])
    s_set.add_argument("--backend", choices=["auto", "kwin", "hyprland", "niri", "sway", "gnome", "fcitx5"])
    s_set.add_argument("--mru-commit-ms", type=int)
    s_set.add_argument("--no-reload", action="store_true")
    s_set.set_defaults(func=cmd_set)

    s_b = sub.add_parser("bind", help="Bind Globe+[mods+]<key> to key:..., action:..., or exec:...")
    s_b.add_argument("trigger", help="e.g. globe+e, globe+f, globe+ctrl+home (Fn+Left arrives as home)")
    s_b.add_argument("target", help="e.g. action:emoji, key:f11, key:leftmeta+d, exec:foot")
    s_b.add_argument("--no-reload", action="store_true")
    s_b.set_defaults(func=cmd_bind)

    s_ub = sub.add_parser("unbind", help="Remove a custom Globe binding")
    s_ub.add_argument("trigger")
    s_ub.add_argument("--no-reload", action="store_true")
    s_ub.set_defaults(func=cmd_unbind)

    s_m = sub.add_parser("monitor", help="Stream live Globe tap/debounce/cancel/chord events")
    s_m.set_defaults(func=cmd_monitor)

    s_t = sub.add_parser("test-switch", help="Trigger an immediate desktop layout switch")
    s_t.add_argument("--reverse", action="store_true")
    s_t.set_defaults(func=cmd_test_switch)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))
