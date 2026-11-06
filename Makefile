# wintomac: Windows x86-64 PE -> native macOS arm64 static recompiler.
#
#   make test                  cross-compile, recompile, link and run every program in tests/src
#   make recomp EXE=game.exe   recompile a game into build/<name>.gen/ and link build/<name>.bin
#
# Tests named free_*.c are freestanding (no CRT, kernel32 only); others link the mingw CRT.
PY       := .venv/bin/python
BREW     := $(shell brew --prefix)
VKD3D    := third_party/vkd3d/install
CC       := clang
CFLAGS   := -O2 -g -std=gnu11 -mcpu=apple-m1 -ffp-contract=off -Iruntime -I$(BREW)/include -I$(VKD3D)/include -I$(VKD3D)/include/vkd3d -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable \
            -Wno-parentheses-equality -Wno-unused-value -Wno-tautological-compare -Wno-constant-conversion \
            -Wno-shift-count-overflow -Wno-tautological-constant-out-of-range-compare -Wno-ignored-attributes -Wno-unknown-attributes
LDFLAGS  := -L$(BREW)/lib -lvulkan -lSDL3 -lFAudio -Wl,-rpath,$(BREW)/lib -L$(VKD3D)/lib -lvkd3d-shader -lvkd3d-utils -lvkd3d -Wl,-rpath,$(abspath $(VKD3D))/lib -framework OpenGL
WINCC    := x86_64-w64-mingw32-gcc
RUNTIME  := $(wildcard runtime/*.c) runtime/gen/vulkan_thunks.c runtime/gen/gl_thunks.c
RT_OBJS  := $(patsubst runtime/%.c,build/rt/%.o,$(RUNTIME))
# Objective-C / C++ parts of the runtime: the experimental D3D12-on-Metal backend (runtime/d3d12mtl*, docs/d3d12_metal.md)
RT_OBJS  += $(patsubst runtime/%.m,build/rt/%.o,$(wildcard runtime/*.m)) $(patsubst runtime/%.cpp,build/rt/%.o,$(wildcard runtime/*.cpp))
SPVC     := third_party/spirv-cross/install
SPVC_LIBS := $(SPVC)/lib/libspirv-cross-msl.a $(SPVC)/lib/libspirv-cross-glsl.a $(SPVC)/lib/libspirv-cross-core.a
LDFLAGS  += -framework Metal -framework Foundation -framework AppKit -framework QuartzCore $(SPVC_LIBS) -lc++
TESTS    := $(basename $(notdir $(wildcard tests/src/*.c tests/src/*.cpp)))

.PHONY: regress-copyelim test check recomp clean vkd3d dxvk zydis spirv-cross dx12mtl-bench interp-fuzz steam-gen dx12-sample jit-a64-selftest link package

# vkd3d-shader (HLSL compiler for the native d3dcompiler + test shaders): make vkd3d
vkd3d: $(VKD3D)/lib/libvkd3d-shader.dylib
$(VKD3D)/lib/libvkd3d-shader.dylib:
	./tools/build_vkd3d.sh
build/rt/d3dcompiler.o build/rt/d3dreflect.o: $(VKD3D)/lib/libvkd3d-shader.dylib
.SECONDARY:

# Zydis (MIT) x86-64 decoder for the runtime interpreter (runtime/interp*.c): make zydis
ZYDIS     := third_party/zydis
ZYDIS_LIB := $(ZYDIS)/libzydis.a
RT_OBJS   += $(ZYDIS_LIB)
zydis: $(ZYDIS_LIB)
$(ZYDIS_LIB):
	./tools/build_zydis.sh
build/rt/interp.o build/rt/interp_%.o build/rt/jit.o build/rt/jit_%.o: CFLAGS += -I$(ZYDIS)/include -I$(ZYDIS)/zycore-include -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD
$(patsubst runtime/%.c,build/rt/%.o,$(wildcard runtime/interp*.c runtime/jit*.c)): $(ZYDIS_LIB)

build/rt/%.o: runtime/%.c runtime/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
build/rt/%.o: runtime/%.m runtime/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-objc-arc -Wno-deprecated-declarations -c $< -o $@
build/rt/%.o: runtime/%.cpp runtime/*.h $(SPVC_LIBS)
	@mkdir -p $(dir $@)
	clang++ -std=c++17 -O2 -g -mcpu=apple-m1 -Wall -Iruntime -I$(SPVC)/include/spirv_cross -I$(VKD3D)/include/vkd3d -c $< -o $@
# SPIRV-Cross (Apache-2.0): SPIR-V -> MSL for the Metal backend. make spirv-cross
spirv-cross: $(SPVC_LIBS)
$(SPVC_LIBS):
	./tools/build_spirv_cross.sh
build/rt/d3d12mtl.o: $(VKD3D)/lib/libvkd3d-shader.dylib

# SEH tests: clang (MSVC-style __try/__except scope tables) compiles, mingw links.
build/seh_%.exe: tests/src/seh_%.c
	@mkdir -p build
	clang --target=x86_64-w64-windows-gnu -fms-extensions -O1 -isystem $(MINGW_SYSROOT)/include -c $< -o build/seh_$*.o
	$(WINCC) -o $@ build/seh_$*.o
MINGW_SYSROOT := $(shell x86_64-w64-mingw32-gcc -print-sysroot)/x86_64-w64-mingw32
# Itanium C++ ABI with DWARF CFI (.eh_frame + LSDA, no SEH), like AnyPS5's libraries: clang -fdwarf-exceptions; the test
# brings its own unwinder. .eh_frame is renamed .ehfram (an 8-byte section name the program can find), as AnyPS5 does.
build/dwarf_%.exe: tests/src/dwarf_%.cpp
	@mkdir -p build
	clang --target=x86_64-w64-windows-gnu -fdwarf-exceptions -O1 -isystem $(MINGW_SYSROOT)/include -c $< -o build/dwarf_$*.o
	$(WINCC) -o $@ build/dwarf_$*.o
	x86_64-w64-mingw32-objcopy --rename-section .eh_frame=.ehfram $@

INPUT_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/input_*.c))
$(INPUT_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -lxinput1_4

NET_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/net_*.c))
$(NET_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -lws2_32

# Win32 API coverage tests (tools/api_coverage.py): expected output in tests/expected (what Windows prints).
API_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/api_*.c))
$(API_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -ladvapi32 -lshlwapi -lbcrypt -lcrypt32 -lshell32 -lole32 -lsynchronization

AUDIO_TESTS :=$(patsubst tests/src/%.c,%,$(wildcard tests/src/audio_*.c))
$(AUDIO_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -lxaudio2_9 -lole32 -ldsound -lwinmm -lmmdevapi -lksuser

build/free_%.exe: tests/src/free_%.c
	@mkdir -p build
	$(WINCC) -O2 -nostdlib -ffreestanding -e entry -o $@ $< -lkernel32

AVXFLAGS := -O3 -mavx2 -mfma -mbmi -mbmi2 -mf16c -mlzcnt -mpopcnt -msse4.2

build/avx_%.exe: tests/src/avx_%.c
	@mkdir -p build
	$(WINCC) $(AVXFLAGS) -o $@ $<

# Vulkan tests: Khronos headers, a mingw import lib for vulkan-1.dll, and SPIR-V shaders compiled by glslang.
runtime/gen/vulkan_thunks.c tools/vulkan-1.def: tools/gen_vulkan.py
	python3 tools/gen_vulkan.py $(BREW)/share/vulkan/registry/vk.xml
build/vkinc/vulkan:
	@mkdir -p build/vkinc && ln -sf $(BREW)/include/vulkan build/vkinc/vulkan && ln -sf $(BREW)/include/vk_video build/vkinc/vk_video
build/libvulkan-1.a: tools/vulkan-1.def
	x86_64-w64-mingw32-dlltool -d $< -l $@
SHADERS := $(patsubst tests/shaders/%.vert,build/shaders/%_vert.h,$(wildcard tests/shaders/*.vert)) \
           $(patsubst tests/shaders/%.frag,build/shaders/%_frag.h,$(wildcard tests/shaders/*.frag))
$(filter %_vert.h,$(SHADERS)): build/shaders/%_vert.h: tests/shaders/%.vert
	@mkdir -p build/shaders && glslangValidator -V --vn $*_vert -o $@ $< >/dev/null
$(filter %_frag.h,$(SHADERS)): build/shaders/%_frag.h: tests/shaders/%.frag
	@mkdir -p build/shaders && glslangValidator -V --vn $*_frag -o $@ $< >/dev/null
VKDEPS := build/vkinc/vulkan build/libvulkan-1.a $(SHADERS)
VK_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/vk_*.c))
# static pattern rules: always win over the generic build/%.exe rule (make 3.81 rule-selection quirk)
$(VK_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c $(VKDEPS)
	@mkdir -p build
	$(WINCC) -O2 -Ibuild/vkinc -Ibuild/shaders -o $@ $< build/libvulkan-1.a
$(VK_TESTS:%=build/%.native): build/%.native: tests/src/%.c $(VKDEPS)
	clang -O2 -w -I$(BREW)/include -Ibuild/shaders -o $@ $< -L$(BREW)/lib -lvulkan -Wl,-rpath,$(BREW)/lib

# OpenGL: thunks generated from the Khronos registry; tests are built for Windows (opengl32/gdi32) and natively
# for macOS (CGL, same GL calls) and must produce identical pixels.
runtime/gen/gl_thunks.c: tools/gen_opengl.py third_party/opengl-registry/gl.xml
	python3 tools/gen_opengl.py third_party/opengl-registry/gl.xml
GL_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/gl_*.c))
$(GL_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -lopengl32 -lgdi32
$(GL_TESTS:%=build/%.native): build/%.native: tests/src/%.c
	clang -O2 -w -DGL_SILENCE_DEPRECATION -o $@ $< -framework OpenGL

# Windowed demos (need a display; not part of `make check`): make demo-vk_window
DEMOS := $(patsubst tests/demo/%.c,demo_%,$(wildcard tests/demo/*.c))
$(DEMOS:%=build/%.exe): build/demo_%.exe: tests/demo/%.c $(VKDEPS)
	@mkdir -p build
	$(WINCC) -O2 -Ibuild/vkinc -Ibuild/shaders -o $@ $< build/libvulkan-1.a -lgdi32 -ld3d9 -ld3d11 -ldxgi -ld3dcompiler_47 -ldxguid
DLLS_demo_dx9_window = $(DXVK)/d3d9.dll
DLLS_demo_dx11_window = $(DXVK)/d3d11.dll $(DXVK)/dxgi.dll
demo-%: build/demo_%.bin
	cd build && ./demo_$*.bin demo_$*.exe $(FRAMES)

# Linked like old MinGW programs: fixed base below 4 GB and no .reloc (the recompiler must move it).
build/reloc_lowbase.exe: tests/src/reloc_lowbase.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -Wl,--disable-dynamicbase,--disable-reloc-section,--image-base,0x400000

build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -lsynchronization

# Test DLLs. mathlib is linked at the exe's own base on purpose, so the recompiler must rebase it.
build/mathlib.dll: tests/dll/mathlib.c
	@mkdir -p build
	$(WINCC) -O2 -shared -Wl,--image-base,0x140000000 -o $@ $<
build/strlib.dll: tests/dll/strlib.c build/mathlib.dll
	$(WINCC) -O2 -shared -o $@ $< build/mathlib.dll
build/dll_test.exe: tests/src/dll_test.c build/mathlib.dll build/strlib.dll
	$(WINCC) -O2 -o $@ $< build/mathlib.dll
DLLS_dll_test := build/mathlib.dll build/strlib.dll

# Direct3D tests run on DXVK-macOS (Gcenx), recompiled from its Windows x64 DLLs. Fetch with `make dxvk`.
DXVK := third_party/dxvk-macos/dxvk-macOS-async-v1.10.3-20230507/x64
dxvk:
	@test -d $(DXVK) || (mkdir -p third_party/dxvk-macos && cd third_party/dxvk-macos && \
	  gh release download v1.10.3-20230507 -R Gcenx/DXVK-macOS -p 'dxvk-macOS-async-v1.10.3-20230507.tar.gz' && tar xzf *.tar.gz)
DX9_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/dx9_*.c))
$(DX9_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< -ld3d9
DLLS_dx9_triangle := $(DXVK)/d3d9.dll

# D3D11: HLSL -> DXBC at build time with vkd3d-compiler (precompiled path) + D3DCompile at runtime (native d3dcompiler)
VKC := DYLD_LIBRARY_PATH=$(VKD3D)/lib $(VKD3D)/bin/vkd3d-compiler
build/shaders/tri11_vs.h: tests/shaders/tri11.hlsl $(VKD3D)/lib/libvkd3d-shader.dylib
	@mkdir -p build/shaders
	$(VKC) -x hlsl -b dxbc-tpf -p vs_4_0 -e vs_main -o build/shaders/tri11_vs.dxbc $<
	python3 tools/bin2h.py build/shaders/tri11_vs.dxbc $@ tri11_vs
build/shaders/tri11_ps.h: tests/shaders/tri11.hlsl $(VKD3D)/lib/libvkd3d-shader.dylib
	@mkdir -p build/shaders
	$(VKC) -x hlsl -b dxbc-tpf -p ps_4_0 -e ps_main -o build/shaders/tri11_ps.dxbc $<
	python3 tools/bin2h.py build/shaders/tri11_ps.dxbc $@ tri11_ps
DX11_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/dx11_*.c))
$(DX11_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c build/shaders/tri11_vs.h build/shaders/tri11_ps.h
	@mkdir -p build
	$(WINCC) -O2 -Ibuild/shaders -o $@ $< -ld3d11 -ld3dcompiler_47 -ldxgi
DLLS_dx11_triangle = $(DXVK)/d3d11.dll $(DXVK)/dxgi.dll
DLLS_dx11_instanced = $(DXVK)/d3d11.dll $(DXVK)/dxgi.dll

# Direct3D 12: d3d12.dll is runtime/d3d12.c over Wine's libvkd3d (native); COM thunks generated from vkd3d's
# headers. Tests are also built natively against libvkd3d (same API, no thunks) as the pixel reference.
runtime/gen/d3d12_thunks.h: tools/gen_d3d12.py $(VKD3D)/lib/libvkd3d-shader.dylib
	python3 -I tools/gen_d3d12.py $(VKD3D)/include/vkd3d $@
build/rt/d3d12.o: runtime/gen/d3d12_thunks.h $(VKD3D)/lib/libvkd3d-shader.dylib
build/shaders/tri12_%.h: tests/shaders/tri11.hlsl $(VKD3D)/lib/libvkd3d-shader.dylib
	@mkdir -p build/shaders
	$(VKC) -x hlsl -b dxbc-tpf -p $*_5_0 -e $*_main -o build/shaders/tri12_$*.dxbc $<
	python3 tools/bin2h.py build/shaders/tri12_$*.dxbc $@ tri12_$*
# dx12mtl_*: the same programs on the experimental Metal backend (WINTOMAC_D3D12_BACKEND=metal, set by `make check`);
# their reference is still the native libvkd3d build, so pixels must match the Vulkan path exactly.
DX12_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/dx12_*.c tests/src/dx12mtl_*.c))
$(DX12_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c build/shaders/tri12_vs.h build/shaders/tri12_ps.h
	@mkdir -p build
	$(WINCC) -O2 -Ibuild/shaders -Itests/shaders -o $@ $< -ld3d12 -ldxgi
DLLS_dx12_present = $(DXVK)/dxgi.dll   # adapters/outputs/factory from DXVK; the D3D12 swapchain is runtime/d3d12_swapchain.c
DLLS_dx12_async_present = $(DXVK)/dxgi.dll
# A real D3D12 program: Microsoft's DirectX-Graphics-Samples HelloTriangle (MIT), built with mingw by
# tools/build_dx_samples.py (hidden window, 60 frames, prints the last frame's pixels): make dx12-sample
build/samples/HelloTriangle/D3D12HelloTriangle.exe: tools/build_dx_samples.py
	python3 -I tools/build_dx_samples.py
dx12-sample: build/samples/HelloTriangle/D3D12HelloTriangle.exe $(RT_OBJS)
	$(PY) -m recomp $< $(DXVK)/dxgi.dll -o build/hellotriangle.gen
	@./tools/link.sh build/hellotriangle.gen build/hellotriangle.bin "$(CC) $(CFLAGS)" "$(LDFLAGS)" $(RT_OBJS)
	cd build/samples/HelloTriangle && SDL_AUDIO_DRIVER=dummy DXVK_LOG_LEVEL=none ../../hellotriangle.bin D3D12HelloTriangle.exe
DLLS_dx12_breadth = $(DXVK)/dxgi.dll
# Draw-overhead micro-benchmark, vkd3d path vs Metal backend (tools/d3d12mtl/dx12_bench.c): make dx12mtl-bench [DRAWS=2000]
build/dx12mtl_bench.exe: tools/d3d12mtl/dx12_bench.c build/shaders/tri12_vs.h build/shaders/tri12_ps.h
	$(WINCC) -O2 -Ibuild/shaders -o $@ $< -ld3d12 -ldxgi
dx12mtl-bench: build/dx12mtl_bench.bin
	@cd build && for be in vkd3d metal; do echo "== $$be"; SDL_AUDIO_DRIVER=dummy WINTOMAC_D3D12_BACKEND=$$be ./dx12mtl_bench.bin dx12mtl_bench.exe $(DRAWS); done
$(DX12_TESTS:%=build/%.native): build/%.native: tests/src/%.c build/shaders/tri12_vs.h build/shaders/tri12_ps.h $(VKD3D)/lib/libvkd3d-shader.dylib
	clang -O2 -w -I$(BREW)/include -I$(VKD3D)/include/vkd3d -Ibuild/shaders -Itests/shaders -o $@ $< -L$(BREW)/lib -lvulkan -L$(VKD3D)/lib -lvkd3d -lvkd3d-utils \
	  -Wl,-rpath,$(BREW)/lib -Wl,-rpath,$(abspath $(VKD3D))/lib

# Steamworks: steam_api64.dll is runtime/steam.c, bridging to the macOS libsteam_api.dylib (docs/steam.md).
# ABI tables are generated from the interface headers Proton vendors (`make steam-gen`; needs tools/fetch_proton_steam.sh).
# Tests import steam_api64.dll via a mingw import library and run against a mock libsteam_api (WINTOMAC_STEAM_API).
steam-gen:
	python3 -I tools/gen_steam.py third_party/proton-lsteamclient/src/lsteamclient runtime/gen/steam_ifaces.h
build/rt/steam.o: runtime/gen/steam_ifaces.h runtime/steam_abi.h
build/libsteam_api64.a: tools/steam_api64.def
	@mkdir -p build
	x86_64-w64-mingw32-dlltool -d $< -l $@
build/libmock_steam_api.dylib: tests/mock/mock_steam_api.c runtime/steam_abi.h runtime/gen/steam_ifaces.h
	@mkdir -p build
	$(CC) -O1 -g -dynamiclib -Iruntime -o $@ $<
STEAM_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/steam_*.c))
$(STEAM_TESTS:%=build/%.exe): build/%.exe: tests/src/%.c build/libsteam_api64.a
	@mkdir -p build
	$(WINCC) -O2 -o $@ $< build/libsteam_api64.a
$(STEAM_TESTS:%=build/%.bin): build/libmock_steam_api.dylib

.SECONDEXPANSION:
build/%.gen/report.json: build/%.exe $$(DLLS_$$*) recomp/*.py
	$(PY) -m recomp $< $(DLLS_$*) -o build/$*.gen

build/%.bin: build/%.gen/report.json $(RT_OBJS)
	@./tools/link.sh build/$*.gen $@ "$(CC) $(CFLAGS)" "$(LDFLAGS)" $(RT_OBJS)

test: $(foreach t,$(TESTS),build/$(t).bin)
	@for t in $(TESTS); do echo "=== $$t"; ./build/$$t.bin build/$$t.exe; echo "[exit=$$?]"; done

recomp: $(RT_OBJS)
	$(PY) -m recomp $(EXE) -o build/$(notdir $(basename $(EXE))).gen
	./tools/link.sh build/$(notdir $(basename $(EXE))).gen build/$(notdir $(basename $(EXE))).bin "$(CC) $(CFLAGS)" "$(LDFLAGS)" $(RT_OBJS)

# Link an already recompiled game against the current runtime (used by tools/package_app.py):
#   make link GEN=build/<name>.gen OUT=build/<name>.bin
link: $(RT_OBJS)
	@test -n "$(GEN)" -a -n "$(OUT)" || (echo "usage: make link GEN=build/x.gen OUT=build/x.bin"; false)
	@./tools/link.sh $(GEN) $(OUT) "$(CC) $(CFLAGS)" "$(LDFLAGS)" $(RT_OBJS)

# Package a game as a self-contained macOS .app (docs/packaging.md):
#   make package GAME=dir EXE=game.exe NAME="Game" [DLLS="a.dll b.dll"] [ARGS="..."]
package:
	$(PY) tools/package_app.py "$(GAME)" "$(EXE)" $(DLLS) --name "$(NAME)" $(if $(ARGS),--args "$(ARGS)")

clean:
	rm -rf build

build/%.exe: tests/src/%.cpp
	@mkdir -p build
	x86_64-w64-mingw32-g++ -O2 -static $(if $(findstring exceptions,$*),,-fno-exceptions) -o $@ $<

# Ground truth: build the same source natively for macOS and diff outputs (free_* tests use tests/expected).
# avx_* reference: x86_64 macOS build of the same source, executed by Rosetta 2.
build/avx_%.native: tests/src/avx_%.c
	clang -arch x86_64 $(AVXFLAGS) -w -o $@ $<
# jit_* (runtime-generated / unrecompiled x86 code, run by the interpreter): x86_64 reference under Rosetta 2.
JIT_TESTS := $(patsubst tests/src/%.c,%,$(wildcard tests/src/jit_*.c))
$(JIT_TESTS:%=build/%.native): build/%.native: tests/src/%.c
	clang -arch x86_64 -O2 -w -o $@ $<
build/%.native: tests/src/%.c
	clang -O2 -w -o $@ $<
# JIT benchmark (tools/bench_jit.sh): same machine code under wintomac (interpreter / JIT) and Rosetta 2
build/jit_bench.exe: tests/jit/jit_bench.c tests/src/jit_hot_code.h
	@mkdir -p build
	$(WINCC) -O2 -o $@ $<
build/jit_bench.rosetta: tests/jit/jit_bench.c tests/src/jit_hot_code.h
	clang -arch x86_64 -O2 -w -o $@ $<
# LLVM AArch64 copy-elimination miscompile (tests/regress/copyelim_miscompile.c): generated code must come out the
# same at -O0 and with tools/link.sh's flags. Also reports whether this clang still has the bug without them.
LINKSH_WCOPT := $(shell grep -o -- '-mllvm -aarch64-enable-copyelim=false' tools/link.sh | head -1)
CE_CC := $(CC) -std=gnu11 -mcpu=apple-m1 -ffp-contract=off -Iruntime -w
regress-copyelim:
	@mkdir -p build/regress
	@$(CE_CC) -O0 tests/regress/copyelim_miscompile.c -o build/regress/ce_O0
	@$(CE_CC) -O1 tests/regress/copyelim_miscompile.c -o build/regress/ce_bug
	@$(CE_CC) -O1 $(LINKSH_WCOPT) tests/regress/copyelim_miscompile.c -o build/regress/ce_O1
	@$(CE_CC) -O2 $(LINKSH_WCOPT) tests/regress/copyelim_miscompile.c -o build/regress/ce_O2
	@ref=$$(./build/regress/ce_O0 2>/dev/null); bug=$$(./build/regress/ce_bug 2>/dev/null); \
	 if [ "$$ref" != "$$bug" ]; then echo "regress-copyelim: this clang miscompiles without the workaround ($$bug vs $$ref)"; \
	 else echo "regress-copyelim: this clang no longer shows the bug"; fi; \
	 ok=1; for o in O1 O2; do r=$$(./build/regress/ce_$$o 2>/dev/null); [ "$$r" = "$$ref" ] || { echo "FAIL regress-copyelim -$$o: $$r (want $$ref)"; ok=0; }; done; \
	 [ $$ok = 1 ] && echo "PASS regress-copyelim (link.sh flags: $(LINKSH_WCOPT))"; [ $$ok = 1 ]
# AArch64 encoder self-test (runtime/jit_a64.h vs clang's assembler)
jit-a64-selftest:
	tests/jit/a64_selftest.sh
# Interpreter differential test: ~1300 instruction snippets x 12 random states, interpreter vs x86_64 under Rosetta 2.
#   make interp-fuzz            interpreter only (WINTOMAC_JIT=0)
#   make interp-fuzz JIT=1      every block JIT-translated on first execution (WINTOMAC_JIT_THRESHOLD=0)
IFZ := build/interp_fuzz
IFZ_OBJS := build/rt/interp.o build/rt/interp_sse.o build/rt/interp_x87.o build/rt/jit.o build/rt/jit_x86.o build/rt/jit_sse.o
IFZ_ENV := $(if $(filter 1,$(JIT)),WINTOMAC_JIT_THRESHOLD=0,WINTOMAC_JIT=0)
IFZ_OUT := $(if $(filter 1,$(JIT)),jit,interp)
interp-fuzz: $(ZYDIS_LIB) $(IFZ_OBJS)
	@mkdir -p $(IFZ)
	python3 tests/interp_fuzz/gen.py $(IFZ)
	clang -arch x86_64 -O1 -w -o $(IFZ)/x86_runner tests/interp_fuzz/x86_runner.c
	$(CC) $(CFLAGS) -I$(ZYDIS)/include -I$(ZYDIS)/zycore-include -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -w -o $(IFZ)/interp_runner \
	  tests/interp_fuzz/interp_runner.c $(IFZ_OBJS) $(ZYDIS_LIB)
	./$(IFZ)/x86_runner $(IFZ)/cases.txt > $(IFZ)/x86.out
	$(IFZ_ENV) ./$(IFZ)/interp_runner $(IFZ)/cases.txt > $(IFZ)/$(IFZ_OUT).out
	@if diff $(IFZ)/x86.out $(IFZ)/$(IFZ_OUT).out > $(IFZ)/diff_$(IFZ_OUT).txt; then echo "interp-fuzz ($(IFZ_OUT)): $$(wc -l < $(IFZ)/x86.out) states match"; \
	 else echo "interp-fuzz ($(IFZ_OUT)): $$(grep -c '^<' $(IFZ)/diff_$(IFZ_OUT).txt) mismatching states (see $(IFZ)/diff_$(IFZ_OUT).txt)"; false; fi
build/%.native: tests/src/%.cpp
	clang++ -std=c++17 -O2 -w -o $@ $<

check: $(foreach t,$(TESTS),build/$(t).bin)
	@pass=0; fail=0; for t in $(TESTS); do \
	  case $$t in dx12mtl_*) be=metal;; *) be=;; esac; \
	  (cd build && DXVK_LOG_LEVEL=none WINTOMAC_D3D12_BACKEND=$$be WINTOMAC_STEAM_API=$(CURDIR)/build/libmock_steam_api.dylib ./$$t.bin $$t.exe > $$t.out 2>&1; echo "[exit=$$?]" >> $$t.out); \
	  if [ -f tests/expected/$$t.out ]; then exp=tests/expected/$$t.out; \
	  else $(MAKE) -s build/$$t.native && (./build/$$t.native; echo "[exit=$$?]") > build/$$t.expected 2>&1; exp=build/$$t.expected; fi; \
	  if diff -u $$exp build/$$t.out > build/$$t.diff; then echo "PASS $$t"; pass=$$((pass+1)); else echo "FAIL $$t"; cat build/$$t.diff; fail=$$((fail+1)); fi; \
	done; echo "$$pass passed, $$fail failed"; [ $$fail = 0 ]

# Godot 4.7.2 editor (third_party/games/godot, ~33M instructions; docs/godot.md). Lifting needs ~12 GB peak
# across workers at -j 4 and compiling 4.8 GB of C takes ~45 min cold, so the binary is built on demand:
#   make godot            recompile + link build/godot.bin (GODOT_JOBS=4 lifting workers)
#   make godot-smoke      headless checks: --version, and a GDScript project's printed values (tests/godot)
GODOT_DIR  := third_party/games/godot/dist
GODOT_EXE  := Godot_v4.7.2-stable_win64.exe
GODOT_JOBS ?= 4
GODOT_RUN  := cd $(GODOT_DIR) && WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy perl -e 'alarm shift; exec @ARGV'
.PHONY: godot godot-smoke
godot: $(RT_OBJS)
	$(PY) -m recomp $(GODOT_DIR)/$(GODOT_EXE) -o build/godot.gen -j $(GODOT_JOBS) $(if $(wildcard tests/godot/seeds.txt),--seeds-file tests/godot/seeds.txt)
	./tools/link.sh build/godot.gen build/godot.bin "$(CC) $(CFLAGS) -w" "$(LDFLAGS)" $(RT_OBJS)
godot-smoke:
	@test -x build/godot.bin || (echo "build/godot.bin missing: run 'make godot' first"; false)
	@$(GODOT_RUN) 300 $(CURDIR)/build/godot.bin ./$(GODOT_EXE) --headless --version > $(CURDIR)/build/godot_version.out 2>&1; \
	  if grep -q '^4\.7\.2\.stable\.official' build/godot_version.out; then echo "PASS godot --version"; \
	  else echo "FAIL godot --version"; cat build/godot_version.out; false; fi
	@$(GODOT_RUN) 600 $(CURDIR)/build/godot.bin ./$(GODOT_EXE) --headless --path $(CURDIR)/tests/godot/smoke --quit-after 5 > $(CURDIR)/build/godot_smoke.out 2>&1; \
	  grep -v '^Godot Engine v' build/godot_smoke.out | grep -F -f tests/godot/smoke/expected.txt -x > build/godot_smoke.got || true; \
	  if diff -u tests/godot/smoke/expected.txt build/godot_smoke.got > build/godot_smoke.diff; then echo "PASS godot GDScript smoke"; \
	  else echo "FAIL godot GDScript smoke"; cat build/godot_smoke.diff; tail -20 build/godot_smoke.out; false; fi

# PS5 compatibility regression tests (docs/ps5_compat.md): build with ps5-payload-sdk, relink with AnyPS5, recompile, run
# headless, diff vs tests/ps5/expected. Skips if tools/ps5_setup.sh --sdk was not run. ~4 min per program.
#   make ps5-check            all programs in tests/ps5/tests.txt      make ps5-check PS5=thr_mutex   just one
.PHONY: ps5-check
ps5-check:
	@tools/ps5_check.sh $(PS5)

# PS5 graphics: tests/ps5/gfx_flip.c through AnyPS5 VideoOut/AgcDriver -> Vulkan -> MoltenVK, headless, frames read back
# and checked (docs/ps5_graphics.md). Skips without the AnyPS5 libraries.   make ps5-draw [PS5DRAW=agc_draw]
.PHONY: ps5-draw
ps5-draw: $(RT_OBJS)
	@tools/ps5_gfx_check.sh $(PS5DRAW)
