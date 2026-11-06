#!/bin/bash
# One-time setup for the PS5 chain (docs/ps5.md): fetch AnyPS5's official Windows PS5-library release and its source,
# build the AnyPS5 relinker natively for arm64, and (optionally, for building homebrew tests) fetch ps5-payload-sdk.
#
#   tools/ps5_setup.sh [--version v0.1.4] [--sdk]
#   tools/ps5_setup.sh --main [--commit <sha>] [--sdk]
#
# --main builds AnyPS5 from source instead (third_party/anyps5-main, same layout as third_party/anyps5-win): clones
# github.com/boykopovar/AnyPS5 into third_party/anyps5-src (latest main, or --commit <sha>), builds the relinker for
# arm64 and cross-compiles the Windows libraries with MinGW-w64 (tools/anyps5_build_win.sh; needs
# `brew install mingw-w64 cmake`; first run builds patched GCC compilers, tools/gcc_sysv_seh_build.sh).
# Running it again updates to the newest main (or the given commit) and rebuilds what changed.
# Use the result with tools/ps5_to_mac.sh --anyps5 main (or PS5_ANYPS5=main).
# Needs: gh (GitHub CLI, logged in) or curl, cmake, Xcode command-line tools. --sdk also needs `brew install llvm lld`.
# Nothing here decrypts anything or touches keys: AnyPS5's libraries are HLE re-implementations under GPL-2.0.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="v0.1.4"; SDK=0; MAIN=0; COMMIT=""
while [ $# -gt 0 ]; do case "$1" in --version) VER="$2"; shift 2 ;; --sdk) SDK=1; shift ;; --main) MAIN=1; shift ;;
    --commit) MAIN=1; COMMIT="$2"; shift 2 ;; *) echo "unknown option $1" >&2; exit 1 ;; esac; done
if [ $MAIN = 1 ]; then
    S="$ROOT/third_party/anyps5-src"; M="$ROOT/third_party/anyps5-main"; mkdir -p "$M"
    [ -d "$S/.git" ] || { echo "[ps5_setup] cloning AnyPS5 into $S"; git clone --filter=blob:none https://github.com/boykopovar/AnyPS5.git "$S"; }
    git -C "$S" fetch --quiet origin main
    if [ -n "$COMMIT" ]; then git -C "$S" fetch --quiet origin "$COMMIT" 2>/dev/null || true; WANT="$(git -C "$S" rev-parse "$COMMIT^{commit}")"
    else WANT="$(git -C "$S" rev-parse origin/main)"; fi
    if [ "$(git -C "$S" rev-parse HEAD)" != "$WANT" ]; then
        [ -z "$(git -C "$S" status --porcelain --untracked-files=no)" ] || { echo "[ps5_setup] $S has local changes; not switching" >&2; exit 1; }
        git -C "$S" -c advice.detachedHead=false checkout --quiet "$WANT"
    fi
    echo "[ps5_setup] AnyPS5 $(git -C "$S" log -1 --format='%h %cs %s' HEAD)"
    git -C "$S" -c protocol.file.allow=never submodule update --init --recursive --depth 1 --jobs 4 --quiet
    if [ "$(cat "$M/COMMIT" 2>/dev/null)" != "$WANT" ] || [ ! -x "$M/build-relinker/core/relinker/relinker" ] || [ ! -f "$M/prx/libs/libc.prx" ]; then
        rm -f "$M/COMMIT"
        echo "[ps5_setup] building the relinker (arm64)"
        cmake -S "$S" -B "$M/build-relinker" -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=OFF \
              -DCMAKE_OSX_ARCHITECTURES=arm64 > "$M/build-relinker.log" 2>&1
        cmake --build "$M/build-relinker" --parallel "${ANYPS5_JOBS:-4}" >> "$M/build-relinker.log" 2>&1 \
            || { echo "[ps5_setup] relinker build failed: $M/build-relinker.log" >&2; exit 1; }
        "$ROOT/tools/gcc_sysv_seh_build.sh"
        "$ROOT/tools/anyps5_build_win.sh" "$S" "$M"
        echo "$WANT" > "$M/COMMIT"
    fi
    echo "[ps5_setup] AnyPS5 main $WANT: relinker $M/build-relinker/core/relinker/relinker"
    echo "[ps5_setup] libraries: $M/prx/libs ($(ls "$M/prx/libs" | wc -l | tr -d ' ') files); use: tools/ps5_to_mac.sh --anyps5 main ..."
    [ $SDK = 1 ] || exit 0
else
D="$ROOT/third_party/anyps5-win"
mkdir -p "$D/dl" "$D/prx" "$D/src"
fetch() {   # repo tag asset dest
    if command -v gh >/dev/null; then gh release download "$2" -R "$1" -p "$3" -D "$(dirname "$4")" --clobber
    else curl -fL -o "$4" "https://github.com/$1/releases/download/$2/$3"; fi
}
if [ ! -f "$D/prx/libs/libc.prx" ]; then
    echo "[ps5_setup] downloading AnyPS5 $VER Windows libraries (~95 MB)"
    fetch boykopovar/AnyPS5 "$VER" "prx-windows-$VER.tar.gz" "$D/dl/prx-windows-$VER.tar.gz"
    tar xzf "$D/dl/prx-windows-$VER.tar.gz" -C "$D/prx"
fi
SRC="$D/src/AnyPS5-${VER#v}"
if [ ! -d "$SRC" ]; then
    echo "[ps5_setup] downloading AnyPS5 $VER source"
    curl -fL -o "$D/dl/AnyPS5-${VER#v}.tar.gz" "https://github.com/boykopovar/AnyPS5/archive/refs/tags/$VER.tar.gz"
    tar xzf "$D/dl/AnyPS5-${VER#v}.tar.gz" -C "$D/src"
fi
R="$D/build-relinker/core/relinker/relinker"
if [ ! -x "$R" ]; then
    echo "[ps5_setup] building the relinker (arm64)"
    cmake -S "$SRC" -B "$D/build-relinker" -DCMAKE_BUILD_TYPE=Release -DANYPS5_RELINKER_ONLY=ON -DBUILD_TESTING=OFF \
          -DCMAKE_OSX_ARCHITECTURES=arm64 > "$D/build-relinker.log" 2>&1
    cmake --build "$D/build-relinker" --parallel "$(sysctl -n hw.ncpu)" >> "$D/build-relinker.log" 2>&1 \
        || { echo "[ps5_setup] relinker build failed: $D/build-relinker.log" >&2; exit 1; }
fi
echo "[ps5_setup] relinker: $R"
echo "[ps5_setup] libraries: $D/prx/libs ($(ls "$D/prx/libs" | wc -l | tr -d ' ') files)"
fi   # release
if [ $SDK = 1 ]; then
    S="$ROOT/third_party/ps5-payload-sdk"
    if [ ! -d "$S/sdk/ps5-payload-sdk" ]; then
        mkdir -p "$S/dl" "$S/sdk"
        echo "[ps5_setup] downloading ps5-payload-sdk"
        if command -v gh >/dev/null; then gh release download -R ps5-payload-dev/sdk -p ps5-payload-sdk.zip -D "$S/dl" --clobber
        else curl -fL -o "$S/dl/ps5-payload-sdk.zip" https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip; fi
        (cd "$S/sdk" && unzip -q ../dl/ps5-payload-sdk.zip)
    fi
    command -v ld.lld >/dev/null || [ -x "$(brew --prefix lld 2>/dev/null)/bin/ld.lld" ] || echo "[ps5_setup] note: brew install llvm lld (needed by the SDK)"
    echo "[ps5_setup] SDK: $S/sdk/ps5-payload-sdk"
fi
