#!/bin/bash
# Build a PS5 homebrew test program (tests/ps5/*.c) with the open ps5-payload-sdk (third_party/ps5-payload-sdk).
#   tools/ps5_build.sh tests/ps5/hello.c out.elf [extra -l flags]
# Needs Homebrew llvm + lld (brew install llvm lld). Output: a FreeBSD/PS5 x86-64 PIE ELF the AnyPS5 relinker accepts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/third_party/ps5-payload-sdk/sdk/ps5-payload-sdk"
export LLVM_CONFIG="${LLVM_CONFIG:-$(brew --prefix llvm)/bin/llvm-config}"
SRC="$1"; OUT="$2"; shift 2
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
printf '.text\n.globl __ps5_pad\n__ps5_pad: int3\n.p2align 4\n' > "$TMP/pad.s"
"$SDK/bin/prospero-clang" -c "$TMP/pad.s" -o "$TMP/pad.o"
"$SDK/bin/prospero-clang" -O2 -c -o "$TMP/crt0.o" "$ROOT/tests/ps5/crt0.c"
case "$SRC" in
  *.cpp|*.cc)
    if grep -q "PS5_CXXABI: *hle" "$SRC"; then  # C++ runtime (__cxa_*, _Unwind_*) from AnyPS5's libc.prx, as titles use it
      # libc.prx finds the program's FDEs through PT_GNU_EH_FRAME (titles have one; the SDK's linker script doesn't)
      perl -0pe 's/(PT_DYNAMIC;)/$1\n\tph_ehhdr  PT_GNU_EH_FRAME;/; s/(KEEP\(\*\(\.eh_frame_hdr\)\)[^}]*\}) : ph_rodata/$1 : ph_rodata : ph_ehhdr/; s/(KEEP\(\*\(\.eh_frame\)\)[^}]*\})/$1 : ph_rodata/' \
          "$SDK/target/lib/main.script" > "$TMP/main.script"
      # Titles import the C++ ABI by NID; the SDK stubs import it by name, which in AnyPS5's libc.prx is the Windows-ABI
      # entry its own libraries use. Rename those references to the NIDs (unresolved at link time, found by the relinker).
      "$SDK/bin/prospero-clang++" -O2 -Wall -c -o "$TMP/main.o" "$SRC"
      python3 -I -c 'import base64, hashlib, sys
for n in sys.argv[1:]:
    h = hashlib.sha1(n.encode() + bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")).digest()[:8][::-1]
    print(n, base64.b64encode(h, b"+-").decode()[:11])' __cxa_allocate_exception __cxa_free_exception __cxa_throw __cxa_rethrow \
          __cxa_begin_catch __cxa_end_catch __cxa_get_exception_ptr __cxa_call_terminate _Unwind_Resume __gxx_personality_v0 _ZSt9terminatev > "$TMP/nids.txt"
      "$(dirname "$LLVM_CONFIG")/llvm-objcopy" --redefine-syms="$TMP/nids.txt" "$TMP/main.o"
      "$SDK/bin/prospero-clang++" -O2 -Wall -nostdlib -nostartfiles -o "$OUT" "$TMP/pad.o" "$TMP/crt0.o" "$TMP/main.o" \
          -Wl,-e,_start -Wl,--eh-frame-hdr -Wl,-T,"$TMP/main.script" -Wl,--warn-unresolved-symbols \
          -lSceLibcInternal -lkernel ${PS5_LDFLAGS:-} "$@"; exit; fi
    # C++: libc++ / libc++abi / libunwind from the SDK (static); libc from AnyPS5's HLE libc, SDK libc.a only for leftovers (strtod_l...)
    "$SDK/bin/prospero-clang++" -O2 -Wall -nostdlib -nostartfiles -o "$OUT" "$TMP/pad.o" "$TMP/crt0.o" "$SRC" \
        -Wl,-e,_start -lc++ -lc++abi -lunwind -lSceLibcInternal -lkernel -lc $PS5_LDFLAGS "$@" ;;
  *)
    "$SDK/bin/prospero-clang" -O2 -Wall -nostdlib -nostartfiles -o "$OUT" "$TMP/pad.o" "$TMP/crt0.o" "$SRC" \
        -Wl,-e,_start -lSceLibcInternal -lkernel $PS5_LDFLAGS "$@" ;;
esac
