#!/bin/bash
# Cross-compile AnyPS5's Windows PS5 system libraries (libs/*.prx + runtime DLLs) on macOS, from an AnyPS5 checkout,
# into the layout of AnyPS5's prx-windows release package (docs/ps5.md, "AnyPS5 from main").
#
#   tools/anyps5_build_win.sh <AnyPS5 checkout with submodules> <out dir>      (normally run by tools/ps5_setup.sh --main)
#
# Result: <out>/prx/libs/{*.prx, libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll, mimalloc*.dll, licenses/}
# Work dirs: <out>/build-host-tools (native nid_patcher, pe_import_first, glslang), <out>/build-win (the cross build).
# Needs: brew install mingw-w64 cmake; python3 (a private venv gets the ninja build tool); the patched GCC compilers
# of tools/gcc_sysv_seh_build.sh.
# Jobs: $ANYPS5_JOBS (default 4).
#
# Why this is not a plain `cmake -DCMAKE_TOOLCHAIN_FILE=...` (AnyPS5 only builds its Windows libraries on Windows):
#  1. the build runs its own programs (nid_patcher, pe_import_first, glslang-standalone, SQLite tools), which a cross build
#     compiles for Windows: tools/anyps5_cross/cross_overrides.cmake makes those targets copy native builds instead;
#  2. it needs GCC with DWARF exceptions (WinLibs) or System V ABI SEH support (MSYS2's GCC patch): Homebrew's
#     GCC 16.2 has neither, so its cc1/cc1plus are replaced by ones rebuilt with that patch (tools/gcc_sysv_seh_build.sh,
#     tools/mingw-w64-x86_64.cmake); clang cannot replace GCC (no System V varargs on Windows targets);
#  3. libjpeg-turbo is built as an ExternalProject without the toolchain file: passed through the environment;
#  4. with Makefiles on a non-Windows host CMake links MinGW DLLs through an objects.a archive, which AnyPS5's
#     import-library rule rejects ("Expected one shared library link command"): Ninja is used, as AnyPS5's CI does.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "$1" && pwd)"; OUT="$(mkdir -p "$2" && cd "$2" && pwd)"
JOBS="${ANYPS5_JOBS:-4}"
MINGW="$(brew --prefix mingw-w64 2>/dev/null || echo /opt/homebrew/opt/mingw-w64)/toolchain-x86_64"
[ -x "$MINGW/bin/x86_64-w64-mingw32-gcc" ] || { echo "anyps5_build_win: brew install mingw-w64" >&2; exit 1; }
[ -x "$ROOT/third_party/gcc-mingw-sysv-seh/overlay/cc1plus" ] || "$ROOT/tools/gcc_sysv_seh_build.sh"
[ -f "$SRC/3rdparty/SDL2/CMakeLists.txt" ] || { echo "anyps5_build_win: $SRC has no submodules (git submodule update --init --recursive)" >&2; exit 1; }
log() { echo "[anyps5_build_win] $*"; }

NINJA="$(command -v ninja || true)"
if [ -z "$NINJA" ]; then
    [ -x "$OUT/tools-venv/bin/ninja" ] || { log "installing ninja into $OUT/tools-venv (pip)"; python3 -m venv "$OUT/tools-venv"; "$OUT/tools-venv/bin/pip" -q install ninja; }
    NINJA="$OUT/tools-venv/bin/ninja"
fi

log "native build tools -> $OUT/build-host-tools"
cmake -S "$ROOT/tools/anyps5_cross/host_tools" -B "$OUT/build-host-tools" -DCMAKE_BUILD_TYPE=Release -DANYPS5_SRC="$SRC" \
    > "$OUT/build-host-tools.log" 2>&1
cmake --build "$OUT/build-host-tools" --parallel "$JOBS" >> "$OUT/build-host-tools.log" 2>&1 \
    || { echo "host tools failed: $OUT/build-host-tools.log" >&2; exit 1; }

B="$OUT/build-win"
# jxrlib's strcodec.h has `#include "..\x86\x86.h"` (Windows path separators). GCC on a Mac treats the backslashes
# literally, so a forwarding header with that exact file name sits in an -iquote directory.
SHIM="$OUT/include-shim"; mkdir -p "$SHIM"
printf '#include "%s"\n' "$SRC/3rdparty/jxrlib/image/x86/x86.h" > "$SHIM/..\\x86\\x86.h"
log "configuring the Windows cross build -> $B (log: $OUT/build-win-configure.log)"
cmake -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -S "$SRC" -B "$B" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/mingw-w64-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_PROJECT_Relinker_INCLUDE="$ROOT/tools/anyps5_cross/cross_overrides.cmake" \
    -DANYPS5_HOST_TOOLS_DIR="$OUT/build-host-tools" -DCMAKE_C_FLAGS="-iquote $SHIM" > "$OUT/build-win-configure.log" 2>&1 \
    || { echo "configure failed: $OUT/build-win-configure.log" >&2; tail -20 "$OUT/build-win-configure.log" >&2; exit 1; }
log "building target libs with $JOBS jobs (log: $OUT/build-win.log; a cold build takes about an hour)"
# libjpeg-turbo is an ExternalProject configured by a nested cmake that is given the compiler but not the toolchain
# file (it would then target macOS): CMake reads CMAKE_TOOLCHAIN_FILE from the environment for such nested configures.
CMAKE_TOOLCHAIN_FILE="$ROOT/tools/mingw-w64-x86_64.cmake" \
cmake --build "$B" --target libs --parallel "$JOBS" > "$OUT/build-win.log" 2>&1 \
    || { echo "build failed: $OUT/build-win.log" >&2; grep -m5 -B2 -A8 'error' "$OUT/build-win.log" >&2; exit 1; }

# package like AnyPS5's tools/package_release.py (prx-windows-<tag>): patched .prx of every core/libs/prx/<name>,
# the MinGW runtime DLLs, mimalloc, and the third-party licences
L="$OUT/prx/libs"
rm -rf "$OUT/prx.new"; mkdir -p "$OUT/prx.new/libs/licenses"
n=0; missing=""
for d in "$SRC"/core/libs/prx/*/; do
    name="$(basename "$d").prx"
    if [ -f "$B/core/libs/libs/$name" ]; then cp -p "$B/core/libs/libs/$name" "$OUT/prx.new/libs/"; n=$((n + 1)); else missing="$missing $name"; fi
done
[ -z "$missing" ] || { echo "anyps5_build_win: missing patched libraries:$missing" >&2; exit 1; }
cp -p "$MINGW/x86_64-w64-mingw32/lib/libgcc_s_seh-1.dll" "$MINGW/x86_64-w64-mingw32/lib/libstdc++-6.dll" \
      "$MINGW/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$B/core/libs/libs/mimalloc.dll" \
      "$B/core/libs/libs/mimalloc-redirect.dll" "$OUT/prx.new/libs/"
lic() { [ -f "$SRC/$1" ] && cp -p "$SRC/$1" "$OUT/prx.new/libs/licenses/$2"; }
lic 3rdparty/libfvad/LICENSE libfvad.txt; lic 3rdparty/libfvad/PATENTS libfvad-patents.txt
lic 3rdparty/vgmstream/COPYING vgmstream.txt; lic 3rdparty/ogg/COPYING ogg.txt; lic 3rdparty/vorbis/COPYING vorbis.txt
lic 3rdparty/opus/COPYING opus.txt; lic 3rdparty/sqlite/LICENSE.md sqlite.txt; lic 3rdparty/mimalloc/LICENSE mimalloc.txt
rm -rf "$OUT/prx"; mv "$OUT/prx.new" "$OUT/prx"
log "$n libraries + runtime DLLs in $L"
