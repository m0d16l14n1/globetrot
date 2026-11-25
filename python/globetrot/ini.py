# SPDX-License-Identifier: MIT
"""Line-preserving reader/editor for /etc/globetrot.conf.

It follows the C parser in src/globe_config.c, so the daemon, the session
agent and globetrot agree on what a file means:
  - '#' or ';' at the start of a line is a comment;
  - a value is cut at '#' unless it is a command (exec:..., tap_command,
    double_tap_command, emoji_command), where '#' is part of the command;
  - section and key names are case-insensitive; [general] means [globe] and
    [chords] means [bindings].
Edits change only the lines they touch, so comments and layout survive.
"""
from __future__ import annotations

import re

_SECTION_RE = re.compile(r"^\s*\[([^\]]*)\]")
_SECTION_ALIASES = {"general": "globe", "chords": "bindings"}
_RAW_VALUE_KEYS = {"tap_command", "double_tap_command", "emoji_command"}


def canonical_section(name: str) -> str:
    n = name.strip().lower()
    return _SECTION_ALIASES.get(n, n)


def _split_kv(line: str) -> tuple[str, str] | None:
    s = line.strip()
    if not s or s[0] in "#;[" or "=" not in s:
        return None
    key, val = s.split("=", 1)
    key = key.strip().lower()
    val = val.strip()
    if not val.startswith("exec:") and key not in _RAW_VALUE_KEYS and "#" in val:
        val = val.split("#", 1)[0].strip()
    return key, val


class IniDoc:
    """An editable view of a config file that keeps every untouched line."""

    def __init__(self, text: str = "") -> None:
        self.lines: list[str] = text.splitlines()

    def text(self) -> str:
        return "\n".join(self.lines) + "\n" if self.lines else ""

    def _sections(self) -> list[tuple[str, int, int]]:
        """(canonical name, header line index or -1, end index) for each section."""
        out: list[tuple[str, int, int]] = []
        cur, start = "globe", -1
        for i, line in enumerate(self.lines):
            m = _SECTION_RE.match(line)
            if m:
                out.append((cur, start, i))
                cur, start = canonical_section(m.group(1)), i
        out.append((cur, start, len(self.lines)))
        return out

    def items(self, section: str) -> list[tuple[str, str]]:
        sec = canonical_section(section)
        merged: dict[str, str] = {}
        for name, start, end in self._sections():
            if name != sec:
                continue
            for line in self.lines[start + 1:end]:
                kv = _split_kv(line)
                if kv:
                    merged[kv[0]] = kv[1]
        return list(merged.items())

    def get(self, section: str, key: str, default: str = "") -> str:
        return dict(self.items(section)).get(key.strip().lower(), default)

    def set(self, section: str, key: str, value: str) -> None:
        sec, k = canonical_section(section), key.strip().lower()
        new_line = f"{k} = {value}"
        last_block: tuple[int, int] | None = None
        for name, start, end in self._sections():
            if name != sec:
                continue
            last_block = (start, end)
            for i in range(start + 1, end):
                kv = _split_kv(self.lines[i])
                if kv and kv[0] == k:
                    self.lines[i] = new_line
                    return
        if last_block is None:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines.extend([f"[{sec}]", new_line])
            return
        start, end = last_block
        insert_at = end
        while insert_at - 1 > start and not self.lines[insert_at - 1].strip():
            insert_at -= 1
        self.lines.insert(insert_at, new_line)

    def remove(self, section: str, key: str) -> bool:
        sec, k = canonical_section(section), key.strip().lower()
        drop: list[int] = []
        for name, start, end in self._sections():
            if name != sec:
                continue
            for i in range(start + 1, end):
                kv = _split_kv(self.lines[i])
                if kv and kv[0] == k:
                    drop.append(i)
        for i in reversed(drop):
            del self.lines[i]
        return bool(drop)
