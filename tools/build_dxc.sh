#!/bin/sh
# Fetch and build Microsoft's DirectXShaderCompiler (dxc; LLVM/NCSA + MIT) for macOS arm64 into third_party/dxc/build.
# Only needed to regenerate the SM 6.x test blobs (tools/build_sm6x_blobs.sh); `make check` uses the checked-in header.
# Takes ~25 min on 8 cores.
set -e
TAG=v1.9.2609
D=third_party/dxc
mkdir -p $D && cd $D
[ -d src ] || git clone -q --depth 1 --branch $TAG --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/DirectXShaderCompiler.git src
mkdir -p build && cd build
[ -f CMakeCache.txt ] || cmake ../src -C ../src/cmake/caches/PredefinedParams.cmake -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DHLSL_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64 >cmake.log
make -j"$(( $(sysctl -n hw.ncpu) - 2 ))" dxc >build.log 2>&1
echo "[dxc] $(./bin/dxc --version)"
