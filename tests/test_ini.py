# SPDX-License-Identifier: MIT
"""Config editing keeps comments, and Python reads values the way the C daemon does."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

from globetrot import ctl  # noqa: E402
from globetrot.ini import IniDoc  # noqa: E402
from globetrot.session import SessionConfig  # noqa: E402

EXAMPLE = ROOT / "config" / "globetrot.conf"


class TestIniDoc(unittest.TestCase):
    def test_values_follow_c_parser_rules(self) -> None:
        doc = IniDoc(
            "tap_mode = key  # preamble lines belong to [globe]\n"
            "[General]\n"
            "tap_timeout_ms = 300 # ms\n"
            "tap_command = notify-send '#1'\n"
            "[Chords]\n"
            "Globe+Q = exec:foot -e sh -c 'echo #hash'\n"
            "globe+f = key:f11 # fullscreen\n"
            "[session]\n"
            "backend = kwin   # forced\n"
            "emoji_command = picker --tag #x\n"
        )
        self.assertEqual(doc.get("globe", "tap_mode"), "key")
        self.assertEqual(doc.get("globe", "tap_timeout_ms"), "300")
        self.assertEqual(doc.get("globe", "tap_command"), "notify-send '#1'")
        binds = dict(doc.items("bindings"))
        self.assertEqual(binds["globe+q"], "exec:foot -e sh -c 'echo #hash'")
        self.assertEqual(binds["globe+f"], "key:f11")
        self.assertEqual(doc.get("session", "backend"), "kwin")
        self.assertEqual(doc.get("session", "emoji_command"), "picker --tag #x")

    def test_set_and_remove_keep_comments_and_order(self) -> None:
        original = EXAMPLE.read_text(encoding="utf-8")
        doc = IniDoc(original)
        doc.set("globe", "tap_mode", "both")
        doc.set("globe", "combo_modifier", "super_alt")
        doc.set("bindings", "globe+e", "action:emoji")
        doc.set("session", "backend", "kwin")
        doc.set("newsection", "x", "1")
        text = doc.text()

        # Every comment line of the shipped file survives.
        for line in original.splitlines():
            if line.lstrip().startswith("#"):
                self.assertIn(line, text)
        self.assertIn("tap_mode = both", text)
        self.assertNotIn("tap_mode = layout", text)
        self.assertEqual(text.count("combo_modifier ="), 1)
        # The new binding lands inside [bindings], before [session].
        lines = text.splitlines()
        at = lines.index("globe+e = action:emoji")
        self.assertLess(at, lines.index("[session]"))
        self.assertGreater(at, lines.index("[bindings]"))
        self.assertTrue(text.rstrip().endswith("[newsection]\nx = 1"))

        doc2 = IniDoc(text)
        self.assertTrue(doc2.remove("bindings", "GLOBE+E"))
        self.assertFalse(doc2.remove("bindings", "globe+e"))
        self.assertNotIn("globe+e = action:emoji", doc2.text().splitlines())
        self.assertIn("# globe+e = action:emoji", doc2.text().splitlines())

    def test_ctl_set_preserves_comments_on_disk(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / "globe.conf"
            cfg.write_text(EXAMPLE.read_text(encoding="utf-8"), encoding="utf-8")
            comments_before = sum(1 for l in cfg.read_text().splitlines() if l.lstrip().startswith("#"))
            self.assertEqual(ctl.main(["-c", str(cfg), "set", "--tap-mode", "key", "--no-reload"]), 0)
            self.assertEqual(ctl.main(["-c", str(cfg), "bind", "ctrl+home", "key:leftmeta+left", "--no-reload"]), 0)
            text = cfg.read_text()
            comments_after = sum(1 for l in text.splitlines() if l.lstrip().startswith("#"))
            self.assertEqual(comments_before, comments_after)
            self.assertIn("tap_mode = key", text)
            self.assertIn("globe+ctrl+home = key:leftmeta+left", text)

    def test_session_config_reads_inline_comments_like_daemon(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / "globe.conf"
            cfg.write_text(
                "[bindings]\nglobe+e = action:emoji\nglobe+e = action:emoji\n"
                "[session]\nbackend = niri # pinned\nmru_commit_ms = 700 # ms\n",
                encoding="utf-8",
            )
            sc = SessionConfig.load([str(cfg)])
            self.assertEqual(sc.backend, "niri")
            self.assertEqual(sc.mru_commit_ms, 700)


if __name__ == "__main__":
    unittest.main()
