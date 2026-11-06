#!/bin/bash
# make ps5-check: build + run the programs listed in tests/ps5/tests.txt through the PS5 chain
# (SDK build -> AnyPS5 relinker -> wintomac recompile -> headless run) and diff against tests/ps5/expected/<name>.out.
# Optional args: names to run (default all). Skips (exit 0) if the AnyPS5 libraries/relinker/SDK are not set up.
cd "$(dirname "$0")/.." || exit 1
[ -d third_party/anyps5-win/prx/libs ] && [ -x third_party/anyps5-win/build-relinker/core/relinker/relinker ] \
  && [ -x third_party/ps5-payload-sdk/sdk/ps5-payload-sdk/bin/prospero-clang ] \
  || { echo "ps5-check: skipped (run tools/ps5_setup.sh --sdk first)"; exit 0; }
want() { [ $# -eq 0 ] && return 0; local n="$1"; shift; printf '%s\n' "$@" | grep -qx "$n"; }
fail=0; total=0; suites=$(grep -v '^#' tests/ps5/tests.txt | awk 'NF>=3{print $1}' | grep -vx - | sort -u)
for s in $suites; do
  args=(); while read -r su name src; do [ "$su" = "$s" ] && want "$name" "$@" && args+=("$name=$src"); done < <(grep -v '^#' tests/ps5/tests.txt)
  [ ${#args[@]} -gt 0 ] || continue
  out=$(tools/ps5_suite.sh "$s" "${args[@]}"); rc=$?; echo "$out"; total=$((total + ${#args[@]}))
  [ $rc = 0 ] || fail=$((fail + $(echo "$out" | grep -cE "^(FAIL|BUILD-FAIL)") ))
done
while read -r su name src; do
  [ "$su" = "-" ] && want "$name" "$@" || continue
  total=$((total + 1)); tools/ps5_compat.sh "$src" "$name" || fail=$((fail + 1))
done < <(grep -v '^#' tests/ps5/tests.txt)
echo "ps5-check: $((total - fail)) of $total passed"; [ $fail = 0 ]
