#!/usr/bin/env python3
"""Profile a recompiled binary with Instruments' Time Profiler (xctrace, headless) and print where time goes.

    tools/profile.py [-n TOP] [--exe game.exe] [--keep trace.trace] -- ./build/x.bin x.exe [args...]

Prints the hottest functions by self time (top of stack) and by inclusive time. Recompiled functions
`f_<addr>` are annotated with the guest symbol from the PE's symbol table when it has one (mingw builds do;
pass --exe, or it is taken from the first argument ending in .exe/.dll). Runs the program to completion, so
only use it on headless programs (the benchmarks, tests): it does not suppress windows or sound.
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def guest_symbols(exe: str) -> dict[int, str]:
    syms = {}
    try:
        out = subprocess.run(["x86_64-w64-mingw32-nm", "-C", exe], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return syms
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tT":
            syms.setdefault(int(p[0], 16), p[2])
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=25)
    ap.add_argument("--exe")
    ap.add_argument("--keep")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = [c for c in a.cmd if c != "--"]
    if not cmd:
        ap.error("no command")
    exe = a.exe or next((c for c in cmd[1:] if c.lower().endswith((".exe", ".dll"))), None)
    tmp = tempfile.mkdtemp()
    trace = a.keep or os.path.join(tmp, "p.trace")
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy")
    subprocess.run(["xcrun", "xctrace", "record", "--template", "Time Profiler", "--output", trace, "--launch", "--"] + cmd,
                   check=True, env=env, stdout=subprocess.DEVNULL)
    xml = os.path.join(tmp, "p.xml")
    with open(xml, "w") as f:
        subprocess.run(["xcrun", "xctrace", "export", "--input", trace, "--xpath",
                        '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'], check=True, stdout=f)
    byid: dict[str, str] = {}
    selfc, incl = collections.Counter(), collections.Counter()
    total = 0
    for _, el in ET.iterparse(xml, events=("end",)):
        if el.tag == "frame" and el.get("id"):
            byid[el.get("id")] = el.get("name") or el.get("addr") or "?"
        if el.tag != "row":
            continue
        bt = el.find("tagged-backtrace") or el.find("backtrace")
        if bt is None:
            el.clear(); continue
        names = []
        for fr in bt.iter("frame"):
            names.append(byid.get(fr.get("ref"), "?") if fr.get("ref") else byid.get(fr.get("id"), "?"))
        if names:
            total += 1
            selfc[names[0]] += 1
            for n in set(names):
                incl[n] += 1
        el.clear()
    syms = guest_symbols(exe) if exe else {}

    def label(n):
        m = re.fullmatch(r"_?f_([0-9a-f]+)", n)
        if m and int(m.group(1), 16) in syms:
            return f"{n}  [{syms[int(m.group(1), 16)]}]"
        return n

    print(f"{total} samples (1 ms each)\n\nself time:")
    for n, k in selfc.most_common(a.n):
        print(f"  {100.0 * k / total:5.1f}%  {label(n)}")
    print("\ninclusive time:")
    for n, k in incl.most_common(a.n):
        print(f"  {100.0 * k / total:5.1f}%  {label(n)}")


if __name__ == "__main__":
    main()
