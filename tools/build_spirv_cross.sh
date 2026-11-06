#!/bin/sh
# Fetch and build SPIRV-Cross (Khronos, Apache-2.0) into third_party/spirv-cross/install: the SPIR-V -> MSL half of
# the Metal D3D12 backend's shader path (runtime/d3d12mtl_shader.cpp, docs/d3d12_metal.md). Static libraries only.
set -e
TAG=vulkan-sdk-1.4.328.0
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D=$ROOT/third_party/spirv-cross
mkdir -p "$D" && cd "$D"
[ -d src ] || git clone -q --depth 1 --branch $TAG https://github.com/KhronosGroup/SPIRV-Cross.git src
mkdir -p build && cd build
cmake ../src -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
      -DSPIRV_CROSS_CLI=ON -DCMAKE_INSTALL_PREFIX="$D/install" >cmake.log 2>&1
make -j"$(sysctl -n hw.ncpu)" install >make.log 2>&1 || { echo "[spirv-cross] build failed, see $D/build/make.log"; exit 1; }
echo "[spirv-cross] $TAG installed to $D/install"
