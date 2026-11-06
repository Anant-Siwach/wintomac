"""Terminal progress bars for long recompiler phases.

Drawn on stderr only when it is a terminal (logs and pipes stay clean); WINTOMAC_NO_PROGRESS=1 turns them off.
Redraws at most ~8 times a second. Usage:

    bar = Bar("analysis", total)
    bar.update(n, note="game.exe")    # add n done
    bar.done()                        # final 100% line
"""
import os
import sys
import time

ENABLED = sys.stderr.isatty() and not os.environ.get("WINTOMAC_NO_PROGRESS")


def _fmt_eta(s):
    s = int(s)
    return f"{s // 60}m{s % 60:02d}s" if s >= 60 else f"{s}s"


class Bar:
    def __init__(self, label, total, width=28):
        self.label, self.total, self.width = label, max(int(total), 1), width
        self.n, self.t0, self.last, self.note = 0, time.time(), 0.0, ""
        self.on = ENABLED

    def set(self, n, note=None, force=False):
        self.n = min(int(n), self.total)
        if note is not None:
            self.note = note
        now = time.time()
        if self.on and (force or now - self.last >= 0.12):
            self.last = now
            self._draw(now)

    def update(self, k=1, note=None):
        self.set(self.n + k, note)

    def _draw(self, now):
        frac = self.n / self.total
        full = int(frac * self.width)
        bar = "█" * full + "░" * (self.width - full)
        el = now - self.t0
        eta = f" ETA {_fmt_eta(el / frac - el)}" if 0.02 < frac < 1 and el > 2 else ""
        note = f"  {self.note}" if self.note else ""
        cols = 120
        try:
            cols = os.get_terminal_size(sys.stderr.fileno()).columns
        except OSError:
            pass
        line = f"[{self.label}] {bar} {frac * 100:5.1f}%{eta}{note}"
        sys.stderr.write("\r" + line[:cols - 1].ljust(cols - 1))
        sys.stderr.flush()

    def done(self, note=""):
        if not self.on:
            return
        self.n = self.total
        took = f"done in {_fmt_eta(time.time() - self.t0)}"
        self.note = f"{note}  {took}" if note else took
        self._draw(time.time())
        sys.stderr.write("\n")
        sys.stderr.flush()
