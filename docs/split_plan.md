# Publishing plan: three repositories

Run the split only when no build is in progress (paths move). Before every push: `tools/publish_audit.sh`.

## 1. `x86-recomp`: the translator core (reusable by any front end)
| moves | notes |
|---|---|
| `recomp/` (analysis, lifter, vector, regsum, pe parser) | PE-specific loader parts stay behind a format interface so ELF/Mach-O front ends can plug in |
| `runtime/interp*`, `runtime/jit*`, `runtime/recomp.h` | the execution engine: interpreter + trace JIT |
| `tools/link.sh`, `tools/bench.sh`, `tools/bench_jit.sh`, `tools/build_zydis.sh` | build pipeline and benchmarks |
| `tests/src/avx_*`, `jit_*`, `callopt_*`, `seh_*`, `reloc_lowbase`, `tests/interp_fuzz/`, `tests/bench/` | instruction-level tests and the Rosetta differential fuzzer |
| `docs/performance.md` | |

## 2. `wintomac`: Windows games → native macOS apps (depends on x86-recomp)
Everything Win32/DirectX: `runtime/` shims (kernel32, user32, ucrt, d3d12*, vulkan, opengl, audio, input, winsock,
steam, wasapi, ...), `tools/gen_*.py`, `tools/package_app*`, `tools/api_coverage.py`, the remaining tests, `docs/`
(d3d12, d3d12_metal, steam, packaging, api_coverage), `CREDITS.md`, `Makefile`.

## 3. `ps5-to-mac` (name TBD): PS5 programs → native macOS apps
Our glue around AnyPS5 (GPL-2.0-only): `tools/ps5_to_mac.sh`, `runtime/ps5*`, `docs/ps5.md`. It *runs* AnyPS5's relinker and
uses its released libraries; nothing of AnyPS5 is copied into this repo, so its licence can be chosen freely,
though GPL-2.0 is the natural choice for a project built around AnyPS5.

## Licences (decide before publishing)
- Our own code: MIT or Apache-2.0 suggested for x86-recomp and wintomac. GPL is also fine.
- Third-party code we vendor and that must keep its licence: miniz (MIT), test shaders from vkd3d-proton (LGPL-2.1), DirectX samples (MIT).
- Never published: game files and data, corpus binaries, Superposition, Valve SDK headers, Apple Metal Shader Converter.
  `.gitignore` excludes them, and `tools/publish_audit.sh` blocks them.

## Order
1. Wait for the PS5 milestone; freeze builds.
2. `git init` each repo from the mapping above (no history to carry over).
3. Rewrite the READMEs around what actually runs: vkQuake.app, QuakeSpasm, the PS5 homebrew demo, D3D12 on Metal, JIT numbers.
4. `tools/publish_audit.sh`, then push.
