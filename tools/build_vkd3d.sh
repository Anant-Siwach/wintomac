#!/bin/sh
# Fetch, patch and build vkd3d (Wine project, LGPL-2.1) into third_party/vkd3d/install.
# Used for: d3d12.dll (libvkd3d), the native d3dcompiler (HLSL -> DXBC at runtime) and compiling test shaders.
#
# Our changes are third_party/vkd3d-patches/*.patch (git format-patch output, applied in order). Every game links
# install/lib, so a build never touches it until it has succeeded: the sources are extracted fresh into work/,
# patched, built, installed into work/stage (DESTDIR) and only then copied over install/.
#   tools/build_vkd3d.sh            build, then swap into install/
#   tools/build_vkd3d.sh --stage    build into work/stage only; test with DYLD_LIBRARY_PATH=<stage>/lib, then rerun
set -e
VER=1.19
SHA=034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/third_party/vkd3d
PATCHES=$ROOT/third_party/vkd3d-patches
PREFIX=$D/install
mkdir -p "$D" && cd "$D"
[ -f vkd3d-$VER.tar.xz ] || curl -sSfLO https://dl.winehq.org/vkd3d/source/vkd3d-$VER.tar.xz
echo "$SHA  vkd3d-$VER.tar.xz" | shasum -a 256 -c - >/dev/null
# a changed patch set means a fresh source tree
STAMP=$( (ls "$PATCHES"/*.patch >/dev/null 2>&1 && cat "$PATCHES"/*.patch) | shasum -a 256 | cut -c1-16)
if [ ! -f work/stamp ] || [ "$(cat work/stamp)" != "$STAMP" ]; then
    rm -rf work && mkdir -p work && (cd work && tar xf ../vkd3d-$VER.tar.xz)
    for p in "$PATCHES"/*.patch; do [ -f "$p" ] && patch -s -d work/vkd3d-$VER -p1 < "$p"; done
    echo "$STAMP" > work/stamp
fi
cd work/vkd3d-$VER
export PATH="$(brew --prefix bison)/bin:$PATH"     # needs bison >= 3 (macOS ships 2.3)
B=$(brew --prefix)
[ -f Makefile ] || ./configure --prefix="$PREFIX" --disable-tests --disable-demos --without-ncurses \
    --without-xcb --without-opengl CPPFLAGS="-I$B/include" LDFLAGS="-L$B/lib" >../configure.log 2>&1
make -j"$(sysctl -n hw.ncpu)" >../build.log 2>&1 || { echo "[vkd3d] build failed, see $D/work/build.log"; exit 1; }
rm -rf ../stage && make install DESTDIR="$D/work/stage" >../install.log 2>&1
S=$D/work/stage$PREFIX
[ -f "$S/lib/libvkd3d.1.dylib" ] && [ -f "$S/lib/libvkd3d-shader.1.dylib" ] || { echo "[vkd3d] staged install incomplete"; exit 1; }
if [ "$1" = "--stage" ]; then echo "[vkd3d] staged in $S"; exit 0; fi
# Copy over the live install file by file (copy + rename: running processes keep their mapping).
mkdir -p "$PREFIX"
(cd "$S" && find . -type d -exec mkdir -p "$PREFIX/{}" \; && \
 find . \( -type f -o -type l \) -exec sh -c 'cp -P "$2" "$1/$2.new" && mv -f "$1/$2.new" "$1/$2"' _ "$PREFIX" {} \;)
echo "[vkd3d] installed to $PREFIX ($(ls "$PATCHES"/*.patch 2>/dev/null | wc -l | tr -d ' ') patches)"
