#!/bin/bash
# Build + relink + recompile + link + run one PS5 test program headless; compare with tests/ps5/expected/<name>.out.
#   tools/ps5_compat.sh src.c name [--update] [--ldflags "..."]   (output in build/ps5/<name>/run.out)
# Exit codes are appended to the output as "[exit=N]". Expected files are produced from a native clang build where the
# program is portable (--native), otherwise hand-written.
set -uo pipefail
export WINTOMAC_JOBS="${WINTOMAC_JOBS:-3}"   # keep the shared machine usable
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
SRC="$1"; NAME="$2"; shift 2
UPDATE=0; LDF=""; export PS5_LDFLAGS=""
while [ $# -gt 0 ]; do case "$1" in --update) UPDATE=1;; --ldflags) export PS5_LDFLAGS="$2"; shift;; esac; shift; done
export PS5_LDFLAGS="${PS5_LDFLAGS:-$(sed -n 's|^.*PS5_LDFLAGS: *||p' "$SRC" | head -1 | sed 's|\*/.*||')}"
W="build/ps5/$NAME"; mkdir -p "$W"
tools/ps5_quick.sh "$SRC" "$NAME" > "$W/build.log" 2>&1 || { echo "BUILD-FAIL $NAME (see $W/build.log)"; tail -5 "$W/build.log"; exit 2; }
rm -rf "$W/game"; mkdir -p "$W/game"; mkdir -p "$W/game/libs" "$W/game/app0" "$W/game/data"; cp "$W/$NAME.exe" "$W/game/"; cp -p "$W"/libs/* "$W/game/libs/"
( cd "$W/game" && WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy perl -e 'alarm 60; exec @ARGV' "$ROOT/build/ps5_$NAME.bin" "$NAME.exe" < /dev/null > ../run.raw 2>&1; echo "[exit=$?]" >> ../run.raw )
grep -v -e "^Transferring control" -e "^\[time\] scheduler" -e "^\[src/" -e "^\[memory\] " "$W/run.raw" > "$W/run.out"
EXP="tests/ps5/expected/$NAME.out"
[ $UPDATE = 1 ] && { mkdir -p tests/ps5/expected; cp "$W/run.out" "$EXP"; }
if diff -u "$EXP" "$W/run.out" > "$W/run.diff"; then echo "PASS $NAME"; else echo "FAIL $NAME"; head -30 "$W/run.diff"; exit 1; fi
