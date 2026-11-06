# PS5 programs on Apple Silicon

wintomac runs PS5 programs natively on arm64 macOS by chaining two recompilers:

```
PS5 ELF (eboot.bin / homebrew .elf, already decrypted)
   │  AnyPS5 relinker --windows            (github.com/boykopovar/AnyPS5, GPL-2.0)
   ▼
Windows x86-64 .exe  +  AnyPS5's Windows PS5-system-library DLLs (libs/*.prx: libc, libkernel, libSceVideoOut, ...)
   │  wintomac: python -m recomp exe + DLLs → C → clang → arm64   (this repository)
   ▼
native arm64 Mac binary  →  dist/<Name>.app
```

No emulator and no Rosetta at run time. AnyPS5 reimplements the PS5 system libraries (HLE) and converts the PS5
executable into a Windows program that calls them; wintomac then turns that Windows program and those libraries into
native Apple-silicon code, the same way it runs Windows games.

**Legal note.** wintomac never decrypts anything and never handles keys, firmware or DRM. You supply programs that
are already plain ELF files: homebrew you built from source, or titles you own and dumped/decrypted yourself with
your own console. Never download commercial game files. Encrypted or SELF-wrapped inputs are refused.

## Status (2026-11-06)

* **Works:** console programs built with the open [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) run
  natively, with output identical to a native build of the same C source (`tests/ps5/hello.c`: stdio, heap,
  libm, 64-bit integer and float math, exit codes).
* **Graphics:** in progress in a separate track, see [ps5_graphics.md](ps5_graphics.md) (AnyPS5 renders through
  Vulkan, which wintomac bridges to MoltenVK; some Vulkan features AnyPS5 uses are missing in MoltenVK).
* **Real titles:** untested. Expect missing shims, missed code (use `--seeds`, below) and Vulkan/MoltenVK gaps.
* CPU features PS5 code assumes (AVX2, FMA, F16C, BMI1/2, LZCNT, MOVBE) are supported by the lifter, the
  interpreter and the JIT; `make check TESTS=avx_movbe` checks MOVBE and every F16C rounding mode against Rosetta.

## One-time setup

```sh
tools/ps5_setup.sh          # AnyPS5 v0.1.4: official Windows library release + source; builds the relinker (arm64)
tools/ps5_setup.sh --sdk    # also ps5-payload-sdk, for building homebrew test programs (needs: brew install llvm lld)
tools/ps5_setup.sh --main   # optional: AnyPS5 built from its latest main instead (see "AnyPS5 from main")
```

This puts everything in `third_party/anyps5-win/` (`prx/libs/` = the Windows PS5 libraries, `build-relinker/` =
the relinker) and `third_party/ps5-payload-sdk/`. The relinker and the libraries must come from the **same AnyPS5
release**: their import conventions change between versions.

### AnyPS5 from main (built from source)

AnyPS5 moves fast; its `main` is usually ahead of the last release. To use it:

```sh
tools/ps5_setup.sh --main                 # latest main; or: --commit <sha>
tools/ps5_to_mac.sh <input> --anyps5 main # or PS5_ANYPS5=main tools/ps5_to_mac.sh <input>
```

`--main` clones AnyPS5 (with submodules) into `third_party/anyps5-src`, builds its relinker for arm64 and
**cross-compiles its Windows PS5 libraries on macOS** into `third_party/anyps5-main/` (same layout as
`third_party/anyps5-win`: `prx/libs/`, `build-relinker/`; the built commit is in `third_party/anyps5-main/COMMIT`).
Running it again moves to the newest main (or the given commit) and rebuilds; nothing is rebuilt when the commit is
unchanged. Needs `brew install mingw-w64 cmake` (and Python 3; Ninja comes from a private venv). A cold build takes
about 20 minutes for the patched GCC (once) and roughly an hour for the libraries with `ANYPS5_JOBS=4` (the default);
later builds of new commits are incremental.

How the cross build works (AnyPS5 itself only builds its Windows libraries on Windows; nothing of AnyPS5 is patched):

* `tools/mingw-w64-x86_64.cmake`: CMake toolchain file for Homebrew's MinGW-w64 (GCC 16.2, UCRT, POSIX threads, SEH;
  the configuration of MSYS2's UCRT64 GCC that AnyPS5 supports).
* GCC: AnyPS5's libraries need GCC with DWARF exceptions under `-fno-asynchronous-unwind-tables` (WinLibs) or with
  System V ABI SEH support (MSYS2 patches GCC for that). Stock GCC 16.2 falls back to SJLJ in the first case and fails
  with `.seh_handlerdata used outside of .seh_proc block` in the second, so `tools/gcc_sysv_seh_build.sh` rebuilds
  only `cc1`/`cc1plus` of the same GCC 16.2.0 with MSYS2's patch (GCC commit b0f09b72) into
  `third_party/gcc-mingw-sysv-seh/overlay`, and `tools/anyps5_cross/x86_64-w64-mingw32-g++` runs Homebrew's driver with
  them (`-B`). Clang/llvm-mingw is not an option: clang has no System V varargs on Windows targets
  (`__builtin_sysv_va_list`), which AnyPS5's PS5-ABI variadic exports use.
* Build-time programs: AnyPS5 runs its own `nid_patcher`, `pe_import_first` and `glslang` while building; a cross
  build compiles them for Windows. `tools/anyps5_cross/cross_overrides.cmake` (injected with
  `CMAKE_PROJECT_Relinker_INCLUDE`) replaces the link step of those targets with a copy of native builds
  (`tools/anyps5_cross/host_tools`), so AnyPS5's build graph stays unchanged.
* Generator: Ninja. With Makefiles on a non-Windows host CMake links MinGW DLLs through an `objects.a` archive, which
  AnyPS5's import-library rule rejects.
* FFmpeg comes from AnyPS5's own prebuilt `ffmpeg-mingw-x64` package (downloaded by its configure step).
* `tools/anyps5_build_win.sh` packages `build-win/core/libs/libs/*.prx`, the MinGW runtime DLLs, mimalloc and the
  licences like AnyPS5's `tools/package_release.py`.

Differences from the release libraries: the release is built with WinLibs GCC 15.2 in DWARF mode (host C++
exceptions use AnyPS5's own unwinder over `.ehfram`); these use GCC 16.2 in SEH mode (`.pdata`/`.xdata`, libgcc's
SEH personality, registered with libc.prx through its `.sehgcc` section), which `runtime/seh.c` already dispatches.
Guest (PS5) C++ exceptions are DWARF in both. The PS5 test programs pass with both (`tools/ps5_main_check.sh compat
hello cxx_throw`, `tools/ps5_main_check.sh draw`, which run the usual tests against `third_party/anyps5-main`).
Fallback if the cross build breaks: AnyPS5's CI does not publish per-commit Windows libraries (only tagged
releases), so use the release (`--anyps5 release`) or build on Windows (AnyPS5 `docs/dev/BUILD.md`) and pass
`--libs <folder> --relinker <same commit's relinker>`.

## The one command

```sh
tools/ps5_to_mac.sh <title_dir | eboot.bin | homebrew.elf> [--name "My Game"]
```

`<title_dir>` holds `eboot.bin` and optionally `sce_module/` (or `sce_modules/`, `prx/`), `sce_sys/param.json` and
the title's other files; it becomes the program's `/app0`. The tool prints each stage:

1. **relink**: AnyPS5 relinker `--windows` (log: `build/ps5/<slug>/relink.log`); bundled modules become
   `*.guest.prx` DLLs.
2. **libraries**: which AnyPS5 `libs/*.prx` to recompile: libc, libkernel, libkernel_web, everything the ELF and its
   modules name in `DT_NEEDED`, and their DLL imports (`tools/ps5_deps.py`). A title that loads a library at run
   time with `sceSysmoduleLoadModule` needs `--with libSceXxx.prx` (the run stops with
   `LoadLibrary(...libSceXxx.prx): not recompiled with this program`), or `--all-libs`.
3. **API coverage**: Win32/UCRT imports that have no wintomac shim (`tools/api_coverage.py`).
4. **recompile + link**: `python -m recomp` and `make link` via `tools/package_app.py`.
5. **bundle**: `dist/<Name>.app` (`docs/packaging.md`). It prints the commands to run it.

Options: `--out DIR`, `--libs DIR`, `--relinker PATH`, `--with LIB.prx`, `--all-libs`, `--seeds FILE`,
`--recompile`, `--link-data` (don't copy a big title into the app), `--relink-args "..."` (or `PS5_RELINK_ARGS`; e.g.
`--windows-fixed-base 0x400000` for titles that need a fixed base, see AnyPS5 USAGE; the relinker runs in the input
folder), `--anyps5 main|release` (or `PS5_ANYPS5`), `--no-app` (stop at `build/<slug>.bin`).
With a relinker that has `--module-dir` (AnyPS5 main), `Media/Modules` and `Media/Plugins` (Unity titles' run-time
modules) are relinked automatically when the title folder has them. The writable folders AnyPS5 uses beside the
program (`devlog/app`, `data`, `temp0`) are created.

### Running

```sh
# headless (no window, no sound), output in the terminal:
WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy "dist/My Game.app/Contents/MacOS/My Game"
# windowed, logging to a file:
"dist/My Game.app/Contents/MacOS/My Game" > ~/my_game.log 2>&1
# or double-click the app; its log is ~/Library/Logs/wintomac/My Game.log
```

Without the app (`--no-app`): `cd build/ps5/<slug>/game && ../../../<slug>.bin <slug>.exe`.

### Example: the homebrew test program

```sh
tools/ps5_setup.sh --sdk
tools/ps5_build.sh tests/ps5/hello.c /tmp/hello/eboot.bin     # ps5-payload-sdk + tests/ps5/crt0.c
tools/ps5_to_mac.sh /tmp/hello --name HelloPS5
WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy dist/HelloPS5.app/Contents/MacOS/HelloPS5
```
```
Transferring control to ELF entry point
Hello from PS5 homebrew!
argc=1
fib(50)=12586269025
sum=3192.405439
dot=126.000 strlen=8
```
(exit status 7, as `main` returns.) `tools/ps5_quick.sh <file.c> <name>` is the developer loop: build, relink,
recompile and link into `build/ps5_<name>.bin` without making an app.

Homebrew note: the SDK's own `crt1.o` expects an ELF loader's `payload_args`; programs that AnyPS5 runs are entered
like a title (`rdi` → `{argc, argv}`), so `tools/ps5_build.sh` links `tests/ps5/crt0.c` instead, with `-nostdlib`
against the SDK's `libSceLibcInternal`/`libkernel` stubs: libc then comes from AnyPS5's HLE libc. It also places
`_start` away from address 0 (the relinker treats entry point 0 as "no entry").

C++ exceptions: PS5 code and AnyPS5's libraries use the Itanium C++ ABI with DWARF unwind tables, not Windows SEH
(AnyPS5's libraries keep `.eh_frame` as `.ehfram`; a relinked program gets `.ehmeta` → its ELF `PT_GNU_EH_FRAME`).
The unwinder is AnyPS5's libc.prx (guest code); it resumes by restoring registers and jumping into a landing pad.
wintomac parses those tables (`recomp/analysis.py` `_dwarf_eh`), makes every landing pad a label of a function with a
host frame record, and turns the unwinder's jump into a longjmp to that frame (`runtime/seh.c` `rt_landing_pad`).
Functions that use rbp as a frame pointer keep it in `Ctx` at every call, since the unwinder computes their frame
(CFA) from it.
`tests/ps5/cxx_throw.cpp` (`make ps5-check`) and `tests/src/dwarf_eh.cpp` (`make check`) cover it. Homebrew built with
`// PS5_CXXABI: hle` in its source links the C++ runtime from libc.prx as titles do (and gets a `PT_GNU_EH_FRAME`,
which the SDK's linker script omits); without it the SDK's static LLVM libunwind is linked instead. Its resume
(`pop rcx; jmp *rcx`) takes the same path, but only if the program has `PT_GNU_EH_FRAME` (untested).
`WINTOMAC_SEH_TRACE=1` logs every landing-pad resume.

## Your own titles

You relink **your own** dumped files; nothing is downloaded or decrypted for you. Put the decrypted `eboot.bin`
(a plain ELF, `tools/unwrap_self.py` in AnyPS5 unwraps an already decrypted SELF), its `sce_module/` and the rest of
the title's files in one folder, then run `tools/ps5_to_mac.sh <folder> --name "<Title>"`. Big titles: add
`--link-data` so the app runs the files in place instead of copying them. Some titles need
`sce_sys/param.json` (title ID and name) in that folder, as on AnyPS5.

## Troubleshooting

| symptom | meaning / fix |
|---|---|
| `[rt] called import with no shim: X` | a Win32/UCRT function AnyPS5 calls has no shim yet. Stage 3 lists them all up front (`build/ps5/<slug>/missing_shims.txt`). See *Fixing gaps*. |
| `LoadLibrary(...libSceXxx.prx): not recompiled with this program` | run again with `--with libSceXxx.prx` |
| `wintomac_misses.txt` appears in the game folder | code the static analysis missed ran in the interpreter/JIT (slower). Feed it back: `--seeds <that file>` |
| `[libc] unhandled exception ...` | an exception inside AnyPS5's libraries (its message says what failed); `WINTOMAC_ABORT_STACK=1` prints the guest call stack with library symbols |
| `[rt] SIGSEGV ...` + guest call stack | a crash; the stack names the module and nearest export (`libc.prx+0x...`) |
| nothing printed, exit code fine | AnyPS5 buffers stdout; output appears on exit/`fflush` |
| relinker `FAIL:` | AnyPS5 can't convert the input (see its USAGE.md); not a wintomac problem |

Debug switches: `WINTOMAC_TRACE=1` (every Win32 call), `WINTOMAC_VERBOSE=1` (unresolved imports),
`WINTOMAC_VMEM_LOG=1` (placeholder memory operations), `WINTOMAC_HEADLESS=1` (keep windows hidden),
`SDL_AUDIO_DRIVER=dummy` (no sound). AnyPS5's own variables (`APS5_*`, see its USAGE.md) work as on Windows.

## Fixing gaps

* **Missing shim:** add it to `runtime/ps5_shims.c` (or the runtime file of that DLL) as
  `SHIM(Name) { ... }` plus an `E(Name)` table entry; arguments are `A1..A4`, `AN(5)...`, return with `RET(x)`.
  Follow the Microsoft Learn description of the API. `tools/api_coverage.py` reads the tables, so stage 3 then
  shows it as covered. Rebuild: `make link GEN=build/<slug>.gen OUT=build/<slug>.bin` (no recompile needed).
* **Lifter gap** (`[recomp] unsupported (falls back to trap): ...` lines, or a trap at run time): the interpreter
  runs such instructions; to lift them add a case in `recomp/lifter.py` / `recomp/vector.py` with a test in
  `tests/src/avx_*.c` (compared against Rosetta by `make check`).
* **Report a gap:** include `build/ps5/<slug>/relink.log`, `missing_shims.txt`, the run log, and the last
  `WINTOMAC_TRACE=1` lines before the failure. Never attach game files.

## How it works (what had to change)

* **System V calls through function pointers.** PS5 code and AnyPS5's `sysv_abi` exports pass arguments in
  rdi/rsi, which the Win64 calling convention treats as callee-saved. Calls the recompiler can't resolve now also
  store rsi/rdi/xmm6/xmm7 and reload rsi/rdi (`recomp/regsum.py` `UNK_STORE`/`UNK_LOAD`).
* **Placeholder memory** (`runtime/ps5_vmem.c`). AnyPS5 builds the PS5 address space with Windows 10 placeholders:
  `VirtualAlloc2` (`MEM_RESERVE_PLACEHOLDER`/`MEM_REPLACE_PLACEHOLDER`), `MapViewOfFile3`, `UnmapViewOfFile2`,
  `VirtualFree(MEM_PRESERVE_PLACEHOLDER | MEM_COALESCE_PLACEHOLDERS)`. They are modelled as a region list on top of
  mmap; views of pagefile sections alias the same pages via `mach_vm_remap`. `VirtualQuery` now reports exact region
  extents and `MEM_FREE` gaps (from `mach_vm_region`), with the ASLR-slid `__PAGEZERO` (everything below the main
  executable) reported as reserved. Write watching (`GetWriteWatch`) reports every page as written.
* **Loader semantics.** `GetProcAddress` on a recompiled DLL no longer falls back to a same-named CRT shim (AnyPS5
  probes each library for `puts` first, then for its NID); `LoadLibrary` of a `.prx` that wasn't recompiled fails.
* **UCRT details AnyPS5 relies on:** `_get_osfhandle` returns real handles; `_lock_file` publishes the UCRT stream
  flags (`_IOREAD`/`_IOWRITE` at offset 0x14) in the padding of Apple's `FILE`; `NUL` is `/dev/null`;
  plus the shims in `runtime/ps5_shims.c` (security descriptors, iphlpapi notifications, Bessel functions, thread
  stack limits, ...). Over AnyPS5's 394 Windows DLLs, all Win32 imports but a handful are covered (stage 3 prints any left).
* Images whose relocations were synthesized with zero fixups (the relinked exe) can be moved to the 1 TiB region.

## What's next

* Graphics: [ps5_graphics.md](ps5_graphics.md).
* Audio (libSceAudioOut on SDL2 → our SDL path), pad input, save data, `sceSysmoduleLoadModule`-loaded libraries
  discovered automatically.
* Real write watching (mprotect + fault tracking) if GPU uploads become the bottleneck.
* Performance: AnyPS5's libraries are ~9 MB of x86 code per small program (≈2 min to compile cold, seconds with
  ccache afterwards).

Credits: AnyPS5 (GPL-2.0-only), ps5-payload-sdk (GPL-3.0+), see `CREDITS.md`; sources in
`third_party/anyps5-win/SOURCE.md` and `third_party/ps5-payload-sdk/SOURCE.md`.
