#!/bin/bash
# Build the patched GCC compilers (cc1, cc1plus) that AnyPS5's Windows libraries need, for use with Homebrew's
# mingw-w64 toolchain (tools/anyps5_cross/mingw-gcc, tools/mingw-w64-x86_64.cmake). One-time, ~20 min with 4 jobs.
#
#   tools/gcc_sysv_seh_build.sh            -> third_party/gcc-mingw-sysv-seh/overlay/{cc1,cc1plus}
#
# The fix is GCC commit b0f09b72 "i386: Keep SEH enabled for Win64 sysv_abi functions" (Oleg Tolmatcev, in GCC
# master), which MSYS2 applies to its GCC 16.2.0-6 (msys2/MINGW-packages mingw-w64-gcc/0002-*.patch). Without it,
# GCC 16.2 rejects catch handlers in __attribute__((sysv_abi)) functions on Win64 (AnyPS5's PS5-ABI exports).
# The compilers are configured exactly like Homebrew's mingw-w64 GCC (same version, prefix and sysroot), so
# Homebrew's driver, headers, libstdc++ and libgcc are used unchanged; only `make all-gcc` is built, nothing is
# installed. Needs: brew install mingw-w64 (and its gmp, mpfr, libmpc, isl, zstd dependencies).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/third_party/gcc-mingw-sysv-seh"
VER=16.2.0
SHA=e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e   # = Homebrew's and MSYS2's gcc-16.2.0.tar.xz
PATCH=0002-i386-Keep-SEH-enabled-for-Win64-sysv_abi-functions.patch
JOBS="${ANYPS5_JOBS:-4}"
[ -x "$D/overlay/cc1plus" ] && { echo "[gcc_sysv_seh] already built: $D/overlay"; exit 0; }
GCC="$(command -v x86_64-w64-mingw32-gcc)" || { echo "brew install mingw-w64" >&2; exit 1; }
have="$("$GCC" -dumpfullversion)"
[ "$have" = "$VER" ] || { echo "[gcc_sysv_seh] Homebrew's mingw-w64 GCC is $have, this script builds $VER: update VER/SHA" >&2; exit 1; }
CONF="$("$GCC" -v 2>&1 | sed -n 's/^Configured with: \.\.\/configure //p')"
TC="$(echo "$CONF" | sed -E 's/.*--prefix=([^ ]+).*/\1/')"
mkdir -p "$D/dl"
if [ ! -f "$D/dl/gcc-$VER.tar.xz" ]; then
    echo "[gcc_sysv_seh] downloading gcc-$VER"
    curl -fL -o "$D/dl/gcc-$VER.tar.xz" "https://ftp.gnu.org/gnu/gcc/gcc-$VER/gcc-$VER.tar.xz"
fi
echo "$SHA  $D/dl/gcc-$VER.tar.xz" | shasum -a 256 -c - >/dev/null || { echo "[gcc_sysv_seh] checksum mismatch" >&2; exit 1; }
if [ ! -f "$D/dl/$PATCH" ]; then
    curl -fL -o "$D/dl/$PATCH" "https://raw.githubusercontent.com/msys2/MINGW-packages/master/mingw-w64-gcc/$PATCH"
fi
if [ ! -d "$D/gcc-$VER" ]; then
    tar xJf "$D/dl/gcc-$VER.tar.xz" -C "$D"
    patch -d "$D/gcc-$VER" -p1 < "$D/dl/$PATCH"
fi
grep -q 'define TARGET_SEH  (TARGET_64BIT && flag_unwind_tables)' "$D/gcc-$VER/gcc/config/i386/cygming.h" \
    || { echo "[gcc_sysv_seh] patch not applied" >&2; exit 1; }
ZSTD="$(brew --prefix zstd 2>/dev/null || echo /opt/homebrew/opt/zstd)"   # Homebrew's build env adds it implicitly
rm -rf "$D/build"; mkdir -p "$D/build"
echo "[gcc_sysv_seh] configuring like Homebrew ($TC), languages c,c++ (log: $D/configure.log)"
# Homebrew's own options, minus --with-bugurl and the extra languages, with zstd's prefix; --disable-bootstrap
# shellcheck disable=SC2086
ARGS="$(echo "$CONF " | sed -E 's/--with-bugurl=[^ ]+//; s/--enable-languages=[^ ]+/--enable-languages=c,c++/' | sed "s|--with-zstd |--with-zstd=$ZSTD |")"
echo "$ARGS" > "$D/configure.args"
(cd "$D/build" && "../gcc-$VER/configure" $ARGS --disable-bootstrap > "$D/configure.log" 2>&1) || { echo "configure failed: $D/configure.log" >&2; exit 1; }
echo "[gcc_sysv_seh] make all-gcc -j$JOBS (log: $D/build.log)"
make -C "$D/build" -j"$JOBS" all-gcc > "$D/build.log" 2>&1 || { echo "build failed: $D/build.log" >&2; exit 1; }
mkdir -p "$D/overlay"
cp -p "$D/build/gcc/cc1" "$D/build/gcc/cc1plus" "$D/overlay/"
echo "[gcc_sysv_seh] $D/overlay: cc1, cc1plus (prefix $TC)"
