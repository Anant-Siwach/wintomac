#!/bin/bash
# Developer loop for the PS5 chain: build a tests/ps5-style C file with the SDK, relink it --windows, recompile it
# with the AnyPS5 libs it needs and link. Usage: tools/ps5_quick.sh src.c name   -> build/ps5/<name>/<name>.exe + build/ps5_<name>.bin
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
SRC="$1"; NAME="$2"
LIBS=third_party/anyps5-win/prx/libs
RELINKER=third_party/anyps5-win/build-relinker/core/relinker/relinker
W="build/ps5/$NAME"; mkdir -p "$W/in" "$W/libs"
if [ -n "${PS5_ELF:-}" ]; then cp "$PS5_ELF" "$W/in/eboot.bin"; else tools/ps5_build.sh "$SRC" "$W/in/eboot.bin"; fi   # PS5_ELF: prebuilt (tools/ps5_suite.sh)
"$RELINKER" --windows --skip-sce-module "$W/in/eboot.bin" "$W/$NAME.exe" | grep -E "FAIL|complete"
python3 -I tools/ps5_deps.py "$LIBS" libc.prx libkernel.prx libkernel_web.prx $(python3 -I tools/ps5_needed.py "$W/in/eboot.bin") > "$W/deps.txt"
for f in $(cat "$W/deps.txt"); do cp -p "$f" "$W/libs/"; done
.venv/bin/python -m recomp "$W/$NAME.exe" $(sed "s|.*/|$W/libs/|" "$W/deps.txt") -o "build/ps5_$NAME.gen" -j ${WINTOMAC_JOBS:-3} | grep -E "\.exe:|total"
make -s link GEN="build/ps5_$NAME.gen" OUT="build/ps5_$NAME.bin"
