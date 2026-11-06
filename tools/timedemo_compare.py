#!/usr/bin/env python3
"""Compare `timedemo demo1` results recorded by tools/timedemo.sh.

    tools/timedemo_compare.py [build/timedemo/results.txt] [baseline-label]

Prints median fps per label and the ratio to the baseline label (default: the first label in the file).
"""
import collections
import statistics
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/timedemo/results.txt"
fps = collections.OrderedDict()
for line in open(path):
    p = line.split()
    if len(p) == 4:
        fps.setdefault(p[0], []).append(float(p[3]))
if not fps:
    sys.exit(f"no results in {path}")
base = sys.argv[2] if len(sys.argv) > 2 else next(iter(fps))
bmed = statistics.median(fps[base])
print(f"| label | runs | median fps | min | max | vs {base} |")
print("|---|---|---|---|---|---|")
for k, v in fps.items():
    m = statistics.median(v)
    print(f"| {k} | {len(v)} | {m:.1f} | {min(v):.1f} | {max(v):.1f} | {m / bmed:.2f}x |")
