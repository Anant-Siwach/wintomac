#!/bin/sh
# link.sh <gen-dir> <out-bin> "<cc+cflags>" "<ldflags>" runtime-objs...
# Compiles the generated C in parallel, then links.
#
# Incremental: each object records the headers it really includes (-MMD -> .d), and is rebuilt only when its
# .c or one of those headers is newer. Editing an unrelated runtime header (D3D12, Steam, ...) no longer
# recompiles millions of lines of generated code. On top of that, if ccache is installed, compiles go through
# it: a header whose timestamp changed but whose content didn't, or a file identical to one in another game's
# build, comes from the cache (content-hashed, ~/Library/Caches/ccache). WINTOMAC_NO_CCACHE=1 disables it.
set -e
gen=$1; out=$2; WCC=$3; ldflags=$4; shift 4
if [ -z "$WINTOMAC_NO_CCACHE" ] && command -v ccache >/dev/null 2>&1; then
  WCC="ccache $WCC"
  export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-time_macros,include_file_mtime,include_file_ctime}"
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-40G}"   # generated code is big; keep several games' worth
fi
# Generated code gets line tables only (backtraces keep file:line; full -g costs ~35% compile time on
# millions of lines). WINTOMAC_FULL_DEBUG=1 keeps whatever -g the caller passed.
if [ -z "$WINTOMAC_FULL_DEBUG" ]; then
  WCC=$(printf '%s\n' "$WCC" | sed -e 's/ -g / -gline-tables-only /g' -e 's/ -g$/ -gline-tables-only/')
fi
# LLVM's AArch64 "redundant copy elimination" pass miscompiles generated code (Apple clang 21, LLVM 23): after
# `cmp wN, #imm; b.eq`, it deletes a later `mov wM, #imm` when wM is the compared register, as if the whole xM were
# known to be imm. If xM's upper half isn't zero (a 64-bit add that the IR masks with `and 0xffffffff`), a
# zero-extended 32-bit guest value comes out as e.g. 0x100000001. GZDoom crashed in V_Init on that (a table index
# with bit 32 set). Reproducer: tests/regress/copyelim_miscompile.c. The pass only removes redundant moves; turning
# it off only keeps a few redundant moves.
WCOPT=""
case " $WCC " in *aarch64-enable-copyelim*) ;; *) WCOPT="-mllvm -aarch64-enable-copyelim=false";; esac
# The same generated file in another build dir must be the same ccache entry: compile from inside the gen dir
# with relative file names, make the compiler's include paths absolute, and keep the directory out of the hash
# (CCACHE_NOHASHDIR; only the debug info's compilation directory differs, which doesn't matter here).
ROOT=$(pwd)
WCC=$(printf '%s\n' "$WCC" | sed -e "s| -I\([^/][^ ]*\)| -I$ROOT/\1|g")
export CCACHE_NOHASHDIR=1
export WCC WCOPT
GEN=$(cd "$gen" && pwd)
# Largest files first (ls -S): the biggest compile starts immediately instead of becoming the tail.
# WINTOMAC_JOBS=N caps parallel compiles (default: all cores; each clang on a big file can take 1-2 GB).
# Progress bar (only on a terminal; WINTOMAC_NO_PROGRESS=1 turns it off): every finished file appends a byte to
# .progress, and a background loop draws done/total on stderr.
PROG="$GEN/.progress"; : > "$PROG"; export PROG
total=$(cd "$GEN" && ls recomp_*.c recomp_table.c | wc -l | tr -d ' ')
barpid=""
if [ -t 2 ] && [ -z "$WINTOMAC_NO_PROGRESS" ]; then
  t0=$(date +%s)
  ( while :; do
      n=$(wc -c < "$PROG" | tr -d ' '); pct=$((n * 100 / total)); w=$((n * 28 / total))
      el=$(( $(date +%s) - t0 )); eta=""
      [ "$n" -gt 0 ] && [ "$n" -lt "$total" ] && [ "$el" -gt 2 ] && { r=$(( el * (total - n) / n )); eta=" ETA $((r / 60))m$(printf %02d $((r % 60)))s"; }
      printf '\r[compile] %s%s %3d%%  %d/%d files%s   ' "$(printf '%*s' $w '' | tr ' ' '#' | sed 's/#/█/g')" \
        "$(printf '%*s' $((28 - w)) '' | tr ' ' '#' | sed 's/#/░/g')" "$pct" "$n" "$total" "$eta" >&2
      [ "$n" -ge "$total" ] && break
      sleep 0.3
    done ) &
  barpid=$!
fi
(cd "$GEN" && ls -S recomp_*.c recomp_table.c) | xargs -P "${WINTOMAC_JOBS:-$(sysctl -n hw.ncpu)}" -n 1 sh -c '
  cd "$0" || exit 1
  c="$1"; o="${c%.c}.o"; d="${c%.c}.d"
  if [ "$o" -nt "$c" ] && [ -f "$d" ]; then
    stale=0
    # dependency list from the .d file (paths relative to the gen dir): drop "target:" and line continuations
    for h in $(sed -e "s/^[^:]*://" -e "s/\\\\$//" "$d"); do
      if [ "$h" -nt "$o" ]; then stale=1; break; fi
    done
    [ "$stale" = 0 ] && { printf . >> "$PROG"; exit 0; }
  fi
  $WCC $WCOPT -MMD -MF "$d" -c "$c" -o "$o" && printf . >> "$PROG"' "$GEN" || { [ -n "$barpid" ] && kill $barpid 2>/dev/null; echo >&2; exit 1; }
if [ -n "$barpid" ]; then wait $barpid 2>/dev/null; echo " done in $(( $(date +%s) - t0 ))s" >&2; fi
$WCC $ldflags -o "$out" "$gen"/*.o "$@"
echo "[link] $out"
