#!/bin/bash
# Build several PS5 C test programs into ONE ELF (one relink + one wintomac recompile + one link instead of one each),
# then run every test headless as `<exe> <name>` and diff with tests/ps5/expected/<name>.out.
#   tools/ps5_suite.sh <suite> <name>=<src.c> ...      (PS5_LDFLAGS from "PS5_LDFLAGS:" comments in the sources)
# Each source's main() is renamed (-Dmain=t_<name>) and dispatched by name from a generated driver. Rebuild with
# PS5_NOBUILD=1 to rerun the tests only. Per-test output: build/ps5/<suite>/<name>.out (diff: .diff).
set -uo pipefail
export WINTOMAC_JOBS="${WINTOMAC_JOBS:-3}"   # keep the shared machine usable
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
SUITE="$1"; shift
SDK="$ROOT/third_party/ps5-payload-sdk/sdk/ps5-payload-sdk"
export LLVM_CONFIG="${LLVM_CONFIG:-$(brew --prefix llvm)/bin/llvm-config}"
W="build/ps5/$SUITE"; mkdir -p "$W/obj"
if [ -z "${PS5_NOBUILD:-}" ]; then
  LDF=""; DRV="$W/driver.c"
  { echo '#include <stdio.h>'; echo '#include <string.h>'; echo '#include <stdint.h>'; } > "$DRV"
  for t in "$@"; do echo "int t_${t%%=*}(int, char **);" >> "$DRV"; done
  echo 'static struct { const char *n; int (*f)(int, char **); } tab[] = {' >> "$DRV"
  for t in "$@"; do echo "  { \"${t%%=*}\", t_${t%%=*} }," >> "$DRV"; done
  echo '};
int main(int argc, char **argv) {
    for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) if (argc > 1 && !strcmp(argv[1], tab[i].n)) return tab[i].f(argc - 1, argv + 1);
    fprintf(stderr, "usage: suite <test>\n"); return 2;
}' >> "$DRV"
  printf '.text\n.globl __ps5_pad\n__ps5_pad: int3\n.p2align 4\n' > "$W/pad.s"
  "$SDK/bin/prospero-clang" -c "$W/pad.s" -o "$W/pad.o"
  "$SDK/bin/prospero-clang" -O2 -c -o "$W/crt0.o" tests/ps5/crt0.c
  OBJS=""
  for t in "$@"; do n="${t%%=*}"; s="${t#*=}"
    f="$(sed -n 's|^.*PS5_LDFLAGS: *||p' "$s" | head -1 | sed 's|\*/.*||')"; LDF="$LDF $f"
    "$SDK/bin/prospero-clang" -O2 -w -Dmain="t_$n" -c -o "$W/obj/$n.o" "$s" || { echo "BUILD-FAIL $n (compile)"; exit 2; }
    OBJS="$OBJS $W/obj/$n.o"
  done
  "$SDK/bin/prospero-clang" -O2 -w -c -o "$W/driver.o" "$DRV"
  # shellcheck disable=SC2086
  "$SDK/bin/prospero-clang" -O2 -nostdlib -nostartfiles -o "$W/suite.elf" "$W/pad.o" "$W/crt0.o" "$W/driver.o" $OBJS \
      -Wl,-e,_start -lSceLibcInternal -lkernel $(echo $LDF | tr ' ' '\n' | sort -u | tr '\n' ' ') > "$W/ld.log" 2>&1 || { echo "BUILD-FAIL $SUITE (link: $W/ld.log)"; tail -3 "$W/ld.log"; exit 2; }
  PS5_ELF="$W/suite.elf" tools/ps5_quick.sh /dev/null "$SUITE" > "$W/build.log" 2>&1 || { echo "BUILD-FAIL $SUITE (see $W/build.log)"; tail -5 "$W/build.log"; exit 2; }
fi
rm -rf "$W/game"; mkdir -p "$W/game/libs" "$W/game/app0"; cp "$W/$SUITE.exe" "$W/game/"; cp -p "$W"/libs/* "$W/game/libs/"
fail=0
for t in "$@"; do n="${t%%=*}"
  rm -rf "$W/game/data"; mkdir -p "$W/game/data"   # /data exists on a console; AnyPS5 maps it to ./data
  ( cd "$W/game" && WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy perl -e 'alarm 60; exec @ARGV' "$ROOT/build/ps5_$SUITE.bin" "$SUITE.exe" "$n" < /dev/null > "../$n.raw" 2>&1; echo "[exit=$?]" >> "../$n.raw" )
  grep -v -e "^Transferring control" -e "^\[time\] scheduler" -e "^\[src/" -e "^\[memory\] " "$W/$n.raw" > "$W/$n.out"
  if [ -n "${PS5_UPDATE:-}" ]; then mkdir -p tests/ps5/expected; cp "$W/$n.out" "tests/ps5/expected/$n.out"; fi
  if diff -u "tests/ps5/expected/$n.out" "$W/$n.out" > "$W/$n.diff" 2>&1; then echo "PASS $n"; else echo "FAIL $n"; head -${PS5_DIFFLINES:-12} "$W/$n.diff"; fail=1; fi
done
exit $fail
