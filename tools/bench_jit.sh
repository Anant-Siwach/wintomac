#!/bin/sh
# JIT benchmark: the same x86-64 machine code (tests/jit/jit_hot.s, run from a VirtualAlloc'd buffer by
# tests/jit/jit_bench.c) under wintomac's interpreter (WINTOMAC_JIT=0), wintomac's JIT, and Rosetta 2.
#   tools/bench_jit.sh [-r RUNS] [-s SCALE]      (SCALE multiplies the iteration counts, default 0.25)
# Prints a markdown table of per-kernel medians (ms) and the speedups; checks that all variants agree.
set -e
cd "$(dirname "$0")/.."
RUNS=3; SCALE=0.25
while getopts "r:s:" o; do case $o in r) RUNS=$OPTARG;; s) SCALE=$OPTARG;; *) exit 2;; esac; done
export SDL_AUDIO_DRIVER=dummy
make -s build/jit_bench.bin build/jit_bench.rosetta >/dev/null
B=build/jit_bench_out; mkdir -p $B; rm -f $B/*.txt
for r in $(seq $RUNS); do
  (cd build && WINTOMAC_JIT=0 ./jit_bench.bin jit_bench.exe $SCALE) >> $B/interp.txt
  (cd build && ./jit_bench.bin jit_bench.exe $SCALE) >> $B/jit.txt
  ./build/jit_bench.rosetta $SCALE >> $B/rosetta.txt
done
python3 - $B <<'PY'
import sys, statistics, collections
d = sys.argv[1]
res = {}
for v in ("interp", "jit", "rosetta"):
    t = collections.defaultdict(list); ck = {}
    for line in open(f"{d}/{v}.txt"):
        p = line.split()
        if p and p[0] == "BENCH":
            t[p[1]].append(float(p[2])); ck[p[1]] = p[3]
    res[v] = (t, ck)
print("| kernel | interpreter ms | JIT ms | Rosetta 2 ms | JIT speedup vs interpreter | JIT / Rosetta |")
print("|---|---|---|---|---|---|")
bad = 0
for k in res["jit"][0]:
    m = {v: statistics.median(res[v][0][k]) for v in res}
    if len({res[v][1][k] for v in res}) != 1: bad = 1; print(f"checksum mismatch for {k}: " + str({v: res[v][1][k] for v in res}))
    print(f"| {k} | {m['interp']:.1f} | {m['jit']:.1f} | {m['rosetta']:.1f} | {m['interp'] / max(m['jit'], 0.05):.1f}x | {m['jit'] / max(m['rosetta'], 0.05):.2f}x |")
sys.exit(bad)
PY
