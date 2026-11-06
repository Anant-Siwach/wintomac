#!/bin/bash
# Run the PS5 regression tests against AnyPS5 built from main (third_party/anyps5-main, tools/ps5_setup.sh --main)
# instead of the pinned release (third_party/anyps5-win), without touching the release builds:
#   tools/ps5_main_check.sh compat hello cxx_throw          programs of tests/ps5/tests.txt run by tools/ps5_compat.sh
#   tools/ps5_main_check.sh draw [gfx_flip agc_draw pad_input]   make ps5-draw (tools/ps5_gfx_check.sh)
# The test scripts are used unchanged except for their paths: a copy in build/ps5main/tools/ reads
# third_party/anyps5-main and writes build/ps5main/<name>/ and build/ps5main_<name>.{gen,bin}.
# Uses WINTOMAC_JOBS (default 3).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
export WINTOMAC_JOBS="${WINTOMAC_JOBS:-3}" WINTOMAC_ROOT="$ROOT"
T="$ROOT/build/ps5main/tools"; mkdir -p "$T"
for s in ps5_compat.sh ps5_quick.sh ps5_gfx_check.sh; do
    sed -e 's|third_party/anyps5-win|third_party/anyps5-main|g' \
        -e 's|build/ps5/|build/ps5main/|g; s|build/ps5_|build/ps5main_|g' \
        -e 's|cd "$(dirname "$0")/.."|cd "$WINTOMAC_ROOT"|; s|ROOT="$(cd "$(dirname "$0")/.." \&\& pwd)"|ROOT="$WINTOMAC_ROOT"|' \
        -e "s|tools/ps5_quick.sh|$T/ps5_quick.sh|g" "tools/$s" > "$T/$s"
    chmod +x "$T/$s"
done
[ -f third_party/anyps5-main/prx/libs/libc.prx ] || { echo "ps5_main_check: run tools/ps5_setup.sh --main first"; exit 1; }
what="${1:-}"; shift || true
case "$what" in
    compat)
        fail=0
        for n in "$@"; do
            src="$(awk -v n="$n" '$2 == n {print $3}' tests/ps5/tests.txt)"
            [ -n "$src" ] || { echo "no test $n in tests/ps5/tests.txt"; fail=1; continue; }
            "$T/ps5_compat.sh" "$src" "$n" || fail=1
        done
        exit $fail ;;
    draw) exec "$T/ps5_gfx_check.sh" "$@" ;;
    *) sed -n '2,9p' "$0"; exit 2 ;;
esac
