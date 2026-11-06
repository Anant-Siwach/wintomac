# wintomac

**Windows games and programs as native Apple Silicon apps.**
No emulator, no Rosetta, no Wine bottle. Code is recompiled ahead of time to arm64 through C and clang,
and a built-in JIT covers what static analysis can't see.

```
Windows game.exe + DLLs ─► recomp (analysis + lifter) ─► C ─► clang ─► game.bin ─► Game.app
                            runtime: Win32 shims, JIT, Vulkan/Metal/OpenGL/D3D, audio, input
```

## What runs today

| | how | status |
|---|---|---|
| **vkQuake** (Vulkan) | recompiled exe + 11 DLLs → MoltenVK → Metal | fully playable; ships as a signed, self-contained `vkQuake.app` |
| **QuakeSpasm** (OpenGL) | recompiled, including the official build linked at 0x400000 without relocations | playable |
| **D3D9/D3D11** | DXVK's own DLLs recompiled → Vulkan bridge → MoltenVK | test programs pixel-exact |
| **D3D12** | libvkd3d on MoltenVK (default), or our own **D3D12 → Metal** backend (`WINTOMAC_D3D12_BACKEND=metal`) | Microsoft's HelloTriangle renders on both, pixel-identical; Metal path ~2x less CPU per draw |
| **Runtime-generated code** | trace JIT, x86-64 → arm64 in `MAP_JIT` memory | 14–150x faster than its interpreter; 0.7–2.4x Rosetta's time on loops |
| **PS5 homebrew** (experimental) | PS5 ELF → [AnyPS5](https://github.com/boykopovar/AnyPS5) relinker → Windows PE → wintomac | test programs run natively; see `docs/ps5.md` |

The test suite (37+ programs) diffs every result against a native arm64 build or Rosetta 2, byte for byte.

## Quick start

```sh
python3 -m venv .venv && .venv/bin/pip install iced-x86==1.21.0
brew install molten-vk vulkan-loader vulkan-headers sdl3 faudio mingw-w64 glslang bison ccache
make vkd3d dxvk zydis                 # build vkd3d from source, fetch DXVK-macOS, build Zydis
make check                            # cross-compile tests → recompile → run → diff
make recomp EXE=path/to/game.exe      # → build/game.bin ; run from the game folder: ./build/game.bin game.exe
tools/package_app.sh <game_dir> <exe> [dlls...] --name "Game"     # → dist/Game.app (bundled, signed)
```

Useful switches: `WINTOMAC_HEADLESS=1` (windows stay hidden), `WINTOMAC_JOBS=N` (parallel jobs), `WINTOMAC_NO_PROGRESS=1` (no progress bars), `WINTOMAC_TRACE=1` (log every Win32 call),
`WINTOMAC_JIT=0`, `WINTOMAC_D3D12_BACKEND=metal`. Code the analysis missed is logged to `wintomac_misses.txt`;
feed it back with `--seeds-file`.

## How it works

* **Ahead of time first.** Each guest function becomes one C function. Registers live in C locals that clang
  keeps in ARM registers, and are synced around calls only as far as interprocedural summaries require
  (`recomp/regsum.py`). Flags are computed eagerly and almost all are removed as dead code.
* **Then a JIT** for code that appears at runtime (script engines, .NET/Mono, self-modifying code) or was missed:
  an interpreter on Zydis plus a trace JIT with chaining, lazy flags and write-protect-based invalidation.
* **Memory is identity-mapped:** guest pointers are host pointers. Images are rebased into the 1 TiB+ region.
  Programs without relocations (old MinGW builds) get them synthesised from code and data analysis.
* **x86 memory ordering** is emulated with LDAPR/STLR on shared accesses and LSE atomics for `lock` operations;
  stack accesses are plain.
* **Exceptions:** a faithful ntdll SEH dispatcher, so GCC and MSVC C++ exception handling run unchanged.
* **Windows API:** about 6,800 shims, covering 98% of the imports in a 204-binary corpus of real games and engines
  (`tools/api_coverage.py`, `docs/api_coverage.md`). A real registry, WASAPI/XAudio2/DirectSound audio,
  XInput/raw input, Winsock, and a bridge from Steamworks to the Mac Steam client.
* **Graphics:**
  * Vulkan: generated thunks into MoltenVK.
  * D3D9 and D3D11: DXVK, recompiled.
  * D3D12: libvkd3d, or our Metal backend.
  * OpenGL: up to 4.1 via Apple's GL.
* **Builds scale:**
  * Parallel lifting (byte-identical to serial).
  * Size-balanced files, ccache and dependency tracking. vkQuake compiles in about 66 s cold and 1 s warm.
  * The Godot editor (33M instructions) analyses in 80 s.

Details: `docs/performance.md`, `docs/d3d12.md`, `docs/d3d12_metal.md`, `docs/steam.md`, `docs/packaging.md`, `docs/ps5.md`.

## Status and contributing

wintomac is young: two games are fully playable and the test suite is solid, but most programs will hit a missing
API or instruction somewhere. Those are usually small, self-contained fixes, and good first contributions.
See [CONTRIBUTING.md](CONTRIBUTING.md). Reports of what you tried and what happened are just as useful.

## Legal

wintomac contains no game code, keys or firmware. You supply your own games. For PS5 that means programs you
legally obtained and decrypted yourself; this project never handles keys or DRM, and doesn't bypass licence checks
or anti-cheat. Some vendors' EULAs forbid disassembly; check before recompiling commercial software.

## Licence

MIT (see [LICENSE](LICENSE)), except the few vendored third-party files listed at the end of that file.

## Credits

Built on and inspired by many projects: [CREDITS.md](CREDITS.md) lists every one, from AnyPS5, DXVK, vkd3d, MoltenVK,
SDL, FAudio and Zydis to N64Recomp, box64, FEX and Wine.
