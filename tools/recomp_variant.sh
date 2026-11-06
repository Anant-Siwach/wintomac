#!/bin/sh
# Recompile an already-recompiled program again with different recompiler flags, for A/B measurements.
#
#   tools/recomp_variant.sh <existing gen dir> <new name> [recomp flags...]
#   e.g. tools/recomp_variant.sh build/vkquake.gen vkquake-nocallopt --no-callopt
#
# Takes the image list (exe + DLLs) from <gen dir>/report.json, uses <exe dir>/wintomac_misses.txt as the
# seeds file when there is one, and writes build/<new name>.gen and build/<new name>.bin.
set -e
cd "$(dirname "$0")/.."
gen=$1; name=$2; shift 2
imgs=$(python3 -I -c 'import json,sys; print(" ".join(i["path"] for i in json.load(open(sys.argv[1] + "/report.json"))["images"]))' "$gen")
exe=$(echo "$imgs" | cut -d' ' -f1)
seeds=$(dirname "$exe")/wintomac_misses.txt
[ -f "$seeds" ] && set -- --seeds-file "$seeds" "$@"
.venv/bin/python -m recomp $imgs -o "build/$name.gen" "$@"
DB=$(make -s -p -n build/free_hello.bin 2>/dev/null)
RT_OBJS=$(echo "$DB" | sed -n 's/^RT_OBJS := //p' | head -1)
CFLAGS=$(echo "$DB" | sed -n 's/^CFLAGS := //p' | head -1)
LDFLAGS=$(echo "$DB" | sed -n 's/^LDFLAGS := //p' | head -1)
make -s $RT_OBJS
./tools/link.sh "build/$name.gen" "build/$name.bin" "clang $CFLAGS" "$LDFLAGS" $RT_OBJS
