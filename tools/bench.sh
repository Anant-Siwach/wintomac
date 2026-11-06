#!/bin/sh
# Benchmark harness: each tests/bench/*.c(pp) program is built three ways from the same source and timed.
#   ours    : Windows x64 build (clang --target=x86_64-w64-windows-gnu + mingw), recompiled by wintomac
#   rosetta : x86_64 macOS build (clang -arch x86_64), run by Rosetta 2  -- the competitor
#   native  : arm64 macOS build (clang -mcpu=apple-m1)                     -- the ceiling
# All three use clang -O2 -ffp-contract=off and baseline x86-64 (SSE2), so the code being compared is the same.
#
#   tools/bench.sh [-r RUNS] [-o results.md] [-v variants] [bench ...]     (bench = file name without extension)
#   variants: comma list of base,ours,notso,rosetta,native (default ours,rosetta,native)
#             notso = ours recompiled with --no-tso (no x86 memory-ordering emulation; single-threaded-safe only)
#             base  = an older wintomac's binaries, for before/after: BASE_DIR=<that checkout>/build/bench
#                     (build them there first with its own tools/bench.sh -v ours)
# Prints a markdown table of per-kernel medians (ms) and checks that every build computes the same checksums.
# Environment: RECOMP_FLAGS (extra flags for the recompiler, e.g. to A/B an optimisation), BENCH_TAG (build dir suffix).
set -e
cd "$(dirname "$0")/.."
RUNS=5; OUT=; VARIANTS=ours,rosetta,native
while getopts "r:o:v:" o; do case $o in r) RUNS=$OPTARG;; o) OUT=$OPTARG;; v) VARIANTS=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
BENCHES=${*:-$(ls tests/bench/*.c tests/bench/*.cpp | xargs -n1 basename | sed 's/\.[a-z]*$//')}

export SDL_AUDIO_DRIVER=dummy
PY=.venv/bin/python
B=build/bench${BENCH_TAG:+-$BENCH_TAG}
mkdir -p $B
SYSROOT=$(x86_64-w64-mingw32-gcc -print-sysroot)/x86_64-w64-mingw32
GXXVER=$(x86_64-w64-mingw32-g++ -dumpversion)
WINC="clang --target=x86_64-w64-windows-gnu -isystem $SYSROOT/include"
WINCXX="clang++ --target=x86_64-w64-windows-gnu -nostdinc++ -isystem $SYSROOT/include/c++/$GXXVER -isystem $SYSROOT/include/c++/$GXXVER/x86_64-w64-mingw32 -isystem $SYSROOT/include -std=c++17"
COMMON="-O2 -ffp-contract=off -w -Itests/bench"

# runtime objects + flags, exactly as the Makefile builds them
RT_OBJS=$(make -s -p -n build/free_hello.bin 2>/dev/null | sed -n 's/^RT_OBJS := //p' | head -1)
[ -n "$RT_OBJS" ] || { echo "bench: could not get RT_OBJS from the Makefile" >&2; exit 1; }
make -s $RT_OBJS >/dev/null
CFLAGS=$(make -s -p -n build/free_hello.bin 2>/dev/null | sed -n 's/^CFLAGS := //p' | head -1)
LDFLAGS=$(make -s -p -n build/free_hello.bin 2>/dev/null | sed -n 's/^LDFLAGS := //p' | head -1)

stale() { t=$1; shift; [ ! -f "$t" ] && return 0; for d; do [ "$d" -nt "$t" ] && return 0; done; return 1; }
has() { case ",$VARIANTS," in *",$1,"*) return 0;; esac; return 1; }

for b in $BENCHES; do
  src=tests/bench/$b.c; cxx=0
  [ -f $src ] || { src=tests/bench/$b.cpp; cxx=1; }
  [ -f $src ] || { echo "bench: no tests/bench/$b.c(pp)" >&2; exit 1; }
  if [ $cxx = 1 ]; then
    stale $B/$b.exe $src tests/bench/bench.h && { $WINCXX $COMMON -c $src -o $B/$b.win.o && x86_64-w64-mingw32-g++ -static -o $B/$b.exe $B/$b.win.o; }
    if has rosetta && stale $B/$b.rosetta $src tests/bench/bench.h; then clang++ -std=c++17 -arch x86_64 $COMMON -o $B/$b.rosetta $src; fi
    if has native && stale $B/$b.native $src tests/bench/bench.h; then clang++ -std=c++17 -arch arm64 -mcpu=apple-m1 $COMMON -o $B/$b.native $src; fi
  else
    stale $B/$b.exe $src tests/bench/bench.h && { $WINC $COMMON -c $src -o $B/$b.win.o && x86_64-w64-mingw32-gcc -o $B/$b.exe $B/$b.win.o; }
    if has rosetta && stale $B/$b.rosetta $src tests/bench/bench.h; then clang -arch x86_64 $COMMON -o $B/$b.rosetta $src; fi
    if has native && stale $B/$b.native $src tests/bench/bench.h; then clang -arch arm64 -mcpu=apple-m1 $COMMON -o $B/$b.native $src; fi
  fi
  for v in ours notso; do
    has $v || continue
    flags=$RECOMP_FLAGS; [ $v = notso ] && flags="$flags --no-tso"
    gen=$B/$b.$v.gen
    # rebuild when the exe, the recompiler or the runtime changed
    if [ ! -f $B/$b.$v.bin ] || [ -n "$(find $B/$b.exe recomp/*.py runtime/*.h runtime/*.c -newer $B/$b.$v.bin 2>/dev/null | head -1)" ]; then
      $PY -m recomp $B/$b.exe -o $gen $flags >$B/$b.$v.recomp.log 2>&1 || { cat $B/$b.$v.recomp.log; exit 1; }
      ./tools/link.sh $gen $B/$b.$v.bin "clang $CFLAGS" "$LDFLAGS" $RT_OBJS >/dev/null
    fi
  done
done

# run: every variant of every bench RUNS times, interleaved so thermal/frequency drift hits all variants alike
RES=$B/results.txt; : > $RES
for b in $BENCHES; do
  for r in $(seq $RUNS); do
    for v in $(echo $VARIANTS | tr , ' '); do
      case $v in
        ours|notso) cmd="./$B/$b.$v.bin $B/$b.exe";;
        base) cmd="$BASE_DIR/$b.ours.bin $BASE_DIR/$b.exe";;
        rosetta) cmd="./$B/$b.rosetta";;
        native) cmd="./$B/$b.native";;
      esac
      perl -e 'alarm 600; exec @ARGV' $cmd 2>/dev/null | sed -n "s/^BENCH /$b $v /p" >> $RES || true
    done
  done
done

python3 -I - "$RES" "$VARIANTS" "$OUT" <<'EOF'
import sys, statistics, collections
res, variants, out = sys.argv[1], sys.argv[2].split(","), sys.argv[3]
t = collections.defaultdict(list); ck = collections.defaultdict(set); ck_notso = collections.defaultdict(set); order = []
for line in open(res):
    b, v, k, ms, c = line.split()
    key = (b, k)
    if key not in order: order.append(key)
    t[(b, k, v)].append(float(ms))
    (ck_notso if v == "notso" else ck)[key].add(c)
name = {"base": "before", "ours": "wintomac", "notso": "wintomac --no-tso", "rosetta": "Rosetta 2", "native": "native arm64"}
hdr = "| bench | kernel | " + " | ".join(name[v] + " ms" for v in variants)
ratios = [v for v in variants if v in ("base", "ours", "notso")]
if "rosetta" in variants: hdr += "".join(f" | {name[v]} / Rosetta" for v in ratios)
if "native" in variants: hdr += "".join(f" | {name[v]} / native" for v in ratios)
lines = [hdr + " |", "|" + "---|" * (hdr.count("|"))]
geo = collections.defaultdict(list); bad = []; bad_notso = []
for b, k in order:
    med = {v: statistics.median(t[(b, k, v)]) for v in variants if t[(b, k, v)]}
    row = f"| {b} | {k} | " + " | ".join(f"{med[v]:.1f}" if v in med else "—" for v in variants)
    for base in ("rosetta", "native"):
        if base in variants:
            for v in ratios:
                if v in med and base in med and med[base] > 0:
                    r = med[v] / med[base]; geo[(v, base)].append(r); row += f" | {r:.2f}x"
                else: row += " | —"
    lines.append(row + " |")
    if len(ck[(b, k)]) > 1: bad.append(f"{b}/{k}")
    if ck_notso[(b, k)] and ck_notso[(b, k)] != ck[(b, k)]: bad_notso.append(f"{b}/{k}")
gm = "| **geomean** | | " + " | ".join("" for v in variants)
for base in ("rosetta", "native"):
    if base in variants:
        for v in ratios:
            g = geo[(v, base)]
            gm += f" | **{statistics.geometric_mean(g):.2f}x**" if g else " | —"
lines.append(gm + " |")
text = "\n".join(lines) + "\n"
if bad: text += "\nCHECKSUM MISMATCH: " + ", ".join(bad) + "\n"
if bad_notso: text += "\n--no-tso computed different results (expected for racy code without x86 ordering): " + ", ".join(bad_notso) + "\n"
print(text)
if out: open(out, "w").write(text)
EOF
