# SPDX-License-Identifier: MIT
"""MRU (Most Recently Used) keyboard layout switching engine.

Semantics:
  - Default MRU commit timer: 1000 ms.
  - With 2 layouts (e.g. EN, RU): every tap toggles immediately EN <-> RU.
  - With 3+ layouts (e.g. EN, RU, DE):
      * First tap switches immediately to the most recently used previous layout
        and opens a 1000 ms cycling window without mutating MRU order yet.
      * Each subsequent tap within 1000 ms advances further down the MRU list
        (wrapping around) and refreshes the commit timer.
      * Once 1000 ms elapses (or commit() is called), the chosen layout becomes
        the new MRU head (index 0).
      * External layout changes are automatically reconciled on the next tap.
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class MruLayoutEngine:
    commit_window_s: float = 1.0
    mru_order: list[int] = field(default_factory=list)
    _cycling: bool = False
    _cycle_snapshot: list[int] = field(default_factory=list)
    _cycle_pos: int = 0
    _last_switch_ts: float = 0.0
    _last_set_idx: int | None = None

    def _sync_layouts(self, total: int, current_idx: int) -> None:
        if total <= 0:
            self.mru_order.clear()
            self._cycling = False
            self._cycle_snapshot.clear()
            self._cycle_pos = 0
            return

        current_idx = max(0, min(current_idx, total - 1))
        valid = [i for i in self.mru_order if 0 <= i < total]
        seen = set(valid)
        for i in range(total):
            if i not in seen:
                valid.append(i)
        self.mru_order = valid

        if not self._cycling and self.mru_order and self.mru_order[0] != current_idx:
            self._promote(current_idx)

    def _promote(self, idx: int) -> None:
        if idx in self.mru_order:
            self.mru_order.remove(idx)
        self.mru_order.insert(0, idx)

    def commit(self) -> None:
        """Finalize any active cycling session, promoting the chosen layout to MRU head."""
        if not self._cycling:
            return
        if self._cycle_snapshot and 0 <= self._cycle_pos < len(self._cycle_snapshot):
            self._promote(self._cycle_snapshot[self._cycle_pos])
        self._cycling = False
        self._cycle_snapshot.clear()
        self._cycle_pos = 0

    def next_layout(self, total: int, current_idx: int, now_s: float, *, reverse: bool = False) -> int:
        """Compute the next layout index to select on a Globe tap."""
        if total <= 1:
            return 0

        if self._cycling and (now_s - self._last_switch_ts) > self.commit_window_s:
            self.commit()

        if self._cycling and self._last_set_idx is not None and current_idx != self._last_set_idx:
            # External layout switch interrupted the cycle: discard the uncommitted
            # transient position so MRU preserves the last actually committed layout.
            self._cycling = False
            self._cycle_snapshot.clear()
            self._cycle_pos = 0

        self._sync_layouts(total, current_idx)

        if not self._cycling:
            self._promote(current_idx)
            self._cycle_snapshot = list(self.mru_order)
            self._cycle_pos = (len(self._cycle_snapshot) - 1) if reverse else 1
            self._cycling = True
        else:
            step = -1 if reverse else 1
            self._cycle_pos = (self._cycle_pos + step) % len(self._cycle_snapshot)

        target = self._cycle_snapshot[self._cycle_pos]
        self._last_switch_ts = now_s
        self._last_set_idx = target

        if total == 2:
            self.commit()

        return target
