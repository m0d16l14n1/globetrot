# SPDX-License-Identifier: MIT
"""Desktop compositor & IME layout-switching backends for globetrot.

Supported backends (auto-detected from session environment):
  - kwin      : KDE Plasma / KWin Wayland (org.kde.keyboard D-Bus via busctl/gdbus/qdbus6)
  - hyprland  : Hyprland (hyprctl -j devices + switchxkblayout all)
  - niri      : Niri (niri msg -j keyboard-layouts + action switch-layout)
  - sway      : Sway (swaymsg -t get_inputs + input type:keyboard xkb_switch_layout)
  - gnome     : GNOME Shell / Mutter (gsettings org.gnome.desktop.input-sources)
  - fcitx5    : fcitx5 IME (fcitx5-remote)
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import logging
import os
import re
import shlex
import shutil
import subprocess
from typing import Callable, Sequence

log = logging.getLogger("globetrot-backends")

Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def default_runner(cmd: Sequence[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(cmd),
            capture_output=True,
            text=True,
            timeout=3.0,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return subprocess.CompletedProcess(list(cmd), 127, "", str(exc))


def _parse_gvariant_uint(text: str) -> int | None:
    """Parse GVariant / gdbus / gsettings integer like '(uint32 1,)' or 'uint32 0' or '1'."""
    m = re.search(r"(?:^|[\s(])(?:u?int(?:16|32|64)\s+)?(\d+)(?=[,\s)]|$)", text.strip())
    return int(m.group(1)) if m else None


@dataclass
class LayoutSnapshot:
    backend: str
    layouts: list[str]
    current_index: int
    device_name: str = ""


class LayoutBackend:
    name: str = "none"

    def __init__(self, runner: Runner = default_runner) -> None:
        self._run = runner

    def is_available(self) -> bool:
        return False

    def get_snapshot(self) -> LayoutSnapshot | None:
        return None

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        return False

    def switch_next(self) -> bool:
        return False


class KWinBackend(LayoutBackend):
    name = "kwin"

    def __init__(self, runner: Runner = default_runner) -> None:
        super().__init__(runner)
        self._tool = self._find_dbus_tool()

    @staticmethod
    def _find_dbus_tool() -> str:
        # Prefer busctl (structured JSON, always installed on systemd Linux) then gdbus.
        for cand in ("busctl", "gdbus", "qdbus6", "qdbus"):
            if shutil.which(cand):
                return cand
        return ""

    def is_available(self) -> bool:
        if not self._tool:
            return False
        snap = self.get_snapshot()
        return snap is not None and len(snap.layouts) > 0

    def get_snapshot(self) -> LayoutSnapshot | None:
        if not self._tool:
            return None

        if self._tool == "busctl":
            r_cur = self._run([
                "busctl", "--user", "--json=short", "call",
                "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "getLayout",
            ])
            r_lst = self._run([
                "busctl", "--user", "--json=short", "call",
                "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "getLayoutsList",
            ])
            if r_cur.returncode != 0 or r_lst.returncode != 0:
                return None
            try:
                cur_data = json.loads(r_cur.stdout)["data"][0]
                lst_data = json.loads(r_lst.stdout)["data"][0]
                layouts = [str(item[2] or item[0]) for item in lst_data]
                if not layouts:
                    return None
                return LayoutSnapshot(backend=self.name, layouts=layouts, current_index=int(cur_data))
            except (KeyError, IndexError, ValueError, TypeError):
                return None

        if self._tool == "gdbus":
            r_cur = self._run([
                "gdbus", "call", "--session",
                "--dest", "org.kde.keyboard", "--object-path", "/Layouts",
                "--method", "org.kde.KeyboardLayouts.getLayout",
            ])
            r_lst = self._run([
                "gdbus", "call", "--session",
                "--dest", "org.kde.keyboard", "--object-path", "/Layouts",
                "--method", "org.kde.KeyboardLayouts.getLayoutsList",
            ])
            if r_cur.returncode != 0 or r_lst.returncode != 0:
                return None
            cur = _parse_gvariant_uint(r_cur.stdout)
            if cur is None:
                return None
            tuples = re.findall(r"\('([^']*)',\s*'([^']*)',\s*'([^']*)'\)", r_lst.stdout)
            layouts = [t[2] or t[0] for t in tuples]
            if not layouts:
                return None
            return LayoutSnapshot(backend=self.name, layouts=layouts, current_index=cur)

        if self._tool in ("qdbus6", "qdbus"):
            r_cur = self._run([self._tool, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts.getLayout"])
            r_lst = self._run([self._tool, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts.getLayoutsList"])
            if r_cur.returncode != 0 or r_lst.returncode != 0:
                return None
            cur = _parse_gvariant_uint(r_cur.stdout)
            layouts = [ln.strip() for ln in r_lst.stdout.splitlines() if ln.strip()]
            if cur is None or not layouts:
                return None
            return LayoutSnapshot(backend=self.name, layouts=layouts, current_index=cur)

        return None

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        if not self._tool:
            return False
        if self._tool == "busctl":
            r = self._run([
                "busctl", "--user", "call",
                "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "setLayout", "u", str(index),
            ])
            return r.returncode == 0
        if self._tool == "gdbus":
            r = self._run([
                "gdbus", "call", "--session",
                "--dest", "org.kde.keyboard", "--object-path", "/Layouts",
                "--method", "org.kde.KeyboardLayouts.setLayout", str(index),
            ])
            return r.returncode == 0
        if self._tool in ("qdbus6", "qdbus"):
            r = self._run([self._tool, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts.setLayout", str(index)])
            return r.returncode == 0
        return False

    def switch_next(self) -> bool:
        if not self._tool:
            return False
        if self._tool == "busctl":
            return self._run([
                "busctl", "--user", "call",
                "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "switchToNextLayout",
            ]).returncode == 0
        if self._tool == "gdbus":
            return self._run([
                "gdbus", "call", "--session",
                "--dest", "org.kde.keyboard", "--object-path", "/Layouts",
                "--method", "org.kde.KeyboardLayouts.switchToNextLayout",
            ]).returncode == 0
        if self._tool in ("qdbus6", "qdbus"):
            return self._run([self._tool, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts.switchToNextLayout"]).returncode == 0
        return False


class HyprlandBackend(LayoutBackend):
    name = "hyprland"

    def is_available(self) -> bool:
        return bool(os.environ.get("HYPRLAND_INSTANCE_SIGNATURE") and shutil.which("hyprctl"))

    def get_snapshot(self) -> LayoutSnapshot | None:
        r = self._run(["hyprctl", "-j", "devices"])
        if r.returncode != 0:
            return None
        try:
            data = json.loads(r.stdout)
        except ValueError:
            return None
        kbds = data.get("keyboards", [])
        if not kbds:
            return None
        chosen = None
        for k in kbds:
            if "globetrot" in str(k.get("name", "")).lower():
                chosen = k
                break
        if chosen is None:
            for k in kbds:
                if k.get("main"):
                    chosen = k
                    break
        if chosen is None:
            chosen = kbds[0]

        layout_str = str(chosen.get("layout", ""))
        layouts = [x.strip() for x in layout_str.split(",") if x.strip()]
        if not layouts:
            layouts = [str(chosen.get("active_keymap", "default"))]
        idx = int(chosen.get("active_layout_index", 0))
        return LayoutSnapshot(
            backend=self.name,
            layouts=layouts,
            current_index=max(0, min(idx, len(layouts) - 1)),
            device_name=str(chosen.get("name", "all")),
        )

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        # Switch all keyboards together so bars/widgets stay synchronized.
        r = self._run(["hyprctl", "switchxkblayout", "all", str(index)])
        if r.returncode == 0:
            return True
        if snapshot and snapshot.device_name:
            return self._run(["hyprctl", "switchxkblayout", snapshot.device_name, str(index)]).returncode == 0
        return False

    def switch_next(self) -> bool:
        return self._run(["hyprctl", "switchxkblayout", "all", "next"]).returncode == 0


class NiriBackend(LayoutBackend):
    name = "niri"

    def is_available(self) -> bool:
        return bool(os.environ.get("NIRI_SOCKET") and shutil.which("niri"))

    def get_snapshot(self) -> LayoutSnapshot | None:
        r = self._run(["niri", "msg", "-j", "keyboard-layouts"])
        if r.returncode != 0:
            return None
        try:
            data = json.loads(r.stdout)
            names = [str(x) for x in data.get("names", [])]
            cur = int(data.get("current_idx", 0))
            if not names:
                return None
            return LayoutSnapshot(backend=self.name, layouts=names, current_index=cur)
        except (ValueError, TypeError):
            return None

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        return self._run(["niri", "msg", "action", "switch-layout", str(index)]).returncode == 0

    def switch_next(self) -> bool:
        return self._run(["niri", "msg", "action", "switch-layout", "next"]).returncode == 0


class SwayBackend(LayoutBackend):
    name = "sway"

    def is_available(self) -> bool:
        return bool(os.environ.get("SWAYSOCK") and shutil.which("swaymsg"))

    def get_snapshot(self) -> LayoutSnapshot | None:
        r = self._run(["swaymsg", "-t", "get_inputs"])
        if r.returncode != 0:
            return None
        try:
            inputs = json.loads(r.stdout)
        except ValueError:
            return None
        for dev in inputs:
            if dev.get("type") == "keyboard" and dev.get("xkb_layout_names"):
                names = [str(x) for x in dev["xkb_layout_names"]]
                cur = int(dev.get("xkb_active_layout_index", 0))
                return LayoutSnapshot(backend=self.name, layouts=names, current_index=cur)
        return None

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        return self._run(["swaymsg", "input", "type:keyboard", "xkb_switch_layout", str(index)]).returncode == 0

    def switch_next(self) -> bool:
        return self._run(["swaymsg", "input", "type:keyboard", "xkb_switch_layout", "next"]).returncode == 0


class GnomeBackend(LayoutBackend):
    name = "gnome"

    def is_available(self) -> bool:
        desktop = os.environ.get("XDG_CURRENT_DESKTOP", "").lower()
        return "gnome" in desktop and bool(shutil.which("gsettings"))

    def get_snapshot(self) -> LayoutSnapshot | None:
        r_src = self._run(["gsettings", "get", "org.gnome.desktop.input-sources", "sources"])
        r_cur = self._run(["gsettings", "get", "org.gnome.desktop.input-sources", "current"])
        if r_src.returncode != 0 or r_cur.returncode != 0:
            return None
        pairs = re.findall(r"\('([^']+)',\s*'([^']+)'\)", r_src.stdout)
        cur = _parse_gvariant_uint(r_cur.stdout)
        if not pairs or cur is None:
            return None
        return LayoutSnapshot(backend=self.name, layouts=[p[1] for p in pairs], current_index=cur)

    def set_layout(self, index: int, snapshot: LayoutSnapshot | None = None) -> bool:
        # Update mru-sources on modern GNOME Shell (42+) and fall back to current key.
        r_src = self._run(["gsettings", "get", "org.gnome.desktop.input-sources", "sources"])
        pairs = re.findall(r"\('([^']+)',\s*'([^']+)'\)", r_src.stdout) if r_src.returncode == 0 else []
        ok_mru = False
        if pairs and 0 <= index < len(pairs):
            ordered = [pairs[index]] + [p for i, p in enumerate(pairs) if i != index]
            gv = "[" + ", ".join(f"('{k}', '{v}')" for k, v in ordered) + "]"
            ok_mru = self._run(["gsettings", "set", "org.gnome.desktop.input-sources", "mru-sources", gv]).returncode == 0
        ok_cur = self._run(["gsettings", "set", "org.gnome.desktop.input-sources", "current", str(index)]).returncode == 0
        return ok_mru or ok_cur

    def switch_next(self) -> bool:
        snap = self.get_snapshot()
        if not snap or len(snap.layouts) <= 1:
            return False
        return self.set_layout((snap.current_index + 1) % len(snap.layouts), snap)


class Fcitx5Backend(LayoutBackend):
    name = "fcitx5"

    def is_available(self) -> bool:
        if not shutil.which("fcitx5-remote"):
            return False
        return self._run(["fcitx5-remote", "--check"]).returncode == 0

    def switch_next(self) -> bool:
        return self._run(["fcitx5-remote", "-t"]).returncode == 0

    def get_snapshot(self) -> LayoutSnapshot | None:
        r = self._run(["fcitx5-remote", "-n"])
        if r.returncode != 0:
            return None
        return LayoutSnapshot(backend=self.name, layouts=[r.stdout.strip() or "fcitx5"], current_index=0)


def detect_backend(preferred: str = "auto", runner: Runner = default_runner) -> LayoutBackend:
    """Detect the active Linux desktop layout backend."""
    by_name: dict[str, type[LayoutBackend]] = {
        "hyprland": HyprlandBackend,
        "niri": NiriBackend,
        "sway": SwayBackend,
        "kwin": KWinBackend,
        "gnome": GnomeBackend,
        "fcitx5": Fcitx5Backend,
    }
    pref = (preferred or "auto").strip().lower()
    if pref != "auto" and pref in by_name:
        return by_name[pref](runner=runner)

    order = (HyprlandBackend, NiriBackend, SwayBackend, KWinBackend, GnomeBackend, Fcitx5Backend)
    for cls in order:
        inst = cls(runner=runner)
        if inst.is_available():
            return inst
    return LayoutBackend(runner=runner)


def launch_emoji_picker(override_cmd: str = "") -> bool:
    """Launch the desktop environment's emoji picker cleanly in the background."""
    candidates: list[list[str]] = []
    if override_cmd.strip():
        try:
            parsed = shlex.split(override_cmd)
            if parsed:
                candidates.append(parsed)
        except ValueError as exc:
            log.warning("Invalid emoji_command %r: %s", override_cmd, exc)
    candidates.extend([
        ["plasma-emojier"],
        ["ibus-ui-emojier-plasma"],
        ["rofimoji"],
        ["bemoji", "-t"],
        ["wofi-emoji"],
        ["gnome-characters"],
    ])
    for cmd in candidates:
        if cmd and shutil.which(cmd[0]):
            try:
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                return True
            except OSError as exc:
                log.warning("Failed to launch %r: %s", cmd[0], exc)
    return False


def launch_user_command(command: str) -> bool:
    """Spawn a user-configured session command without blocking the event loop."""
    cmd = command.strip()
    if cmd.startswith("exec:"):
        cmd = cmd[5:].strip()
    if not cmd:
        return False
    try:
        argv = shlex.split(cmd)
    except ValueError as exc:
        log.warning("Invalid command syntax %r: %s", cmd, exc)
        return False
    if not argv:
        return False
    try:
        subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        return True
    except OSError as exc:
        log.warning("Failed to launch %r: %s", argv[0], exc)
        return False
