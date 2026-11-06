# Windows API coverage

How much of the Win32 surface real Windows games import is implemented by the wintomac runtime, measured with
`tools/api_coverage.py` (pure-Python PE parser; inputs are treated as untrusted data and never executed).

```
python3 -I tools/api_coverage.py [--md out.md] [--list] [--with-redist] PATH...
```

For every x86-64 PE file under the given paths, each imported (dll, function) pair (normal and delay-load imports,
ordinals mapped like the loader does) is classified as **shim** (a runtime shim of that name exists: imports resolve
by name across all shim tables), **recompiled** (the imported DLL is one of the scanned files and exports it, so it
is recompiled with the game) or **missing** (calling it would stop with `called import with no shim`). Shim names
come from the `const Shim shims_*[]` tables in `runtime/*.c` and `runtime/gen/*` (macro tables such as `E(name)` and
literal `{ NULL, "name", fn }` entries are both understood); names defined in two tables are reported, because the
first table in `shim_tables[]` wins. `--with-redist` also counts scanned Microsoft redistributables (msvcp140,
vcruntime140, concrt140) as recompiled providers (see the C++ runtime decision below).

## Corpus

| set | binaries | toolchain | source |
|---|---|---|---|
| `third_party/games/vkquake/dist`, `quakespasm/dist` | vkQuake, QuakeSpasm (+ SDL, SDL2, SDL3, codec DLLs) | MinGW | official releases |
| `third_party/dxvk-macos` | DXVK-macOS d3d9/d3d10core/d3d11/dxgi | MinGW | Gcenx/DXVK-macOS |
| `build/*.exe`, `build/*.dll` | the test programs | MinGW (UCRT) | `tests/src` |
| `third_party/corpus/godot` | Godot 4.7.2 editor (GUI + console) | MinGW/LLVM | godotengine/godot |
| `third_party/corpus/sdl3`, `sdl2` | SDL 3.4.20, SDL 2.32.10 | MSVC (no CRT) | libsdl-org/SDL |
| `third_party/corpus/gzdoom` | GZDoom 4.14.2 + zmusic, OpenAL Soft, libsndfile | MSVC (static CRT) | ZDoom/gzdoom |
| `third_party/corpus/luanti` | Luanti 5.18.0 + 19 DLLs | llvm-mingw (libc++) | luanti-org/luanti |
| `third_party/corpus/raylib` | raylib 6.2 `raylib.dll` | MSVC | raysan5/raylib |
| `third_party/corpus/openmw` | OpenMW 0.51.0: engine, tools, OSG, Qt 6, Boost, FFmpeg, ICU, MyGUI (112 PE files) | MSVC (dynamic CRT: msvcp140 / vcruntime140 / api-ms-win-crt-*) | OpenMW/openmw |

Only x86-64 PE files were extracted; Microsoft redistributable DLLs (msvcp140, vcruntime140, ucrtbase,
api-ms-win-*, concrt140) were skipped. Sources and licenses: `third_party/corpus/SOURCES.md`.
Yamagi Quake II 8.70a (its Windows build is 32-bit) and dhewm3 1.5.5 (win32 only) were checked and dropped.
204 x86-64 binaries in total (4 x86 DXVK DLLs skipped).

## Before / after

|  | before | after |
|---|---:|---:|
| shim names in the runtime | 5269 | 6776 |
| distinct imported (dll, function) pairs | 20979 | 20979 |
| missing | 1197 | 413 |
| coverage (shim + recompiled) | 94.3% | 98.0% |
| binaries with every import covered | 62 / 202 | 122 / 204 |
| ... with the MS C++ redistributables recompiled (`--with-redist`, 7 MS DLLs added) | | **200 / 211, 99.7%, 71 missing** |

Of the 413 still missing, 342 are `msvcp140*.dll` / `vcruntime140_1.dll` imports, which are meant to be satisfied by
recompiling Microsoft's DLLs (below); the other 71 are TLS/LDAP/certificate stacks (secur32, ncrypt, crypt32 Cert*,
wldap32, authz), UI Automation, a few unknown ordinals, and two D3D12 ordinals owned by the D3D12 work.

Per-DLL change (DLLs that had or have missing imports):

| DLL | functions imported | coverage before | coverage after | missing before | missing after |
|---|---:|---:|---:|---:|---:|
| msvcp140.dll | 337 | 0% | 0% | 337 | 337 |
| kernel32.dll | 422 | 62% | 100% | 160 | 0 |
| user32.dll | 298 | 73% | 100% | 80 | 0 |
| api-ms-win-crt-math-l1-1-0.dll | 93 | 53% | 100% | 44 | 0 |
| msvcrt.dll | 206 | 79% | 100% | 44 | 0 |
| gdi32.dll | 76 | 45% | 100% | 42 | 0 |
| advapi32.dll | 52 | 23% | 100% | 40 | 0 |
| api-ms-win-crt-stdio-l1-1-0.dll | 86 | 56% | 100% | 38 | 0 |
| api-ms-win-crt-string-l1-1-0.dll | 71 | 52% | 100% | 34 | 0 |
| winmm.dll | 55 | 38% | 100% | 34 | 0 |
| crypt32.dll | 25 | 0% | 32% | 25 | 17 |
| api-ms-win-crt-filesystem-l1-1-0.dll | 27 | 22% | 100% | 21 | 0 |
| uxtheme.dll | 21 | 0% | 95% | 21 | 1 |
| wldap32.dll | 18 | 0% | 39% | 18 | 11 |
| api-ms-win-crt-convert-l1-1-0.dll | 29 | 41% | 100% | 17 | 0 |
| shell32.dll | 29 | 41% | 97% | 17 | 1 |
| ws2_32.dll | 56 | 70% | 100% | 17 | 0 |
| api-ms-win-crt-runtime-l1-1-0.dll | 45 | 64% | 100% | 16 | 0 |
| ole32.dll | 29 | 52% | 100% | 14 | 0 |
| vcruntime140.dll | 27 | 48% | 100% | 14 | 0 |
| bcrypt.dll | 13 | 0% | 100% | 13 | 0 |
| secur32.dll | 13 | 0% | 0% | 13 | 13 |
| api-ms-win-crt-time-l1-1-0.dll | 15 | 33% | 100% | 10 | 0 |
| oleaut32.dll | 13 | 23% | 77% | 10 | 3 |
| iphlpapi.dll | 9 | 0% | 100% | 9 | 0 |
| shlwapi.dll | 9 | 0% | 100% | 9 | 0 |
| dbghelp.dll | 13 | 38% | 100% | 8 | 0 |
| ncrypt.dll | 8 | 0% | 0% | 8 | 8 |
| uiautomationcore.dll | 7 | 0% | 0% | 7 | 7 |
| authz.dll | 6 | 0% | 0% | 6 | 6 |
| api-ms-win-crt-environment-l1-1-0.dll | 9 | 44% | 100% | 5 | 0 |
| winhttp.dll | 5 | 0% | 100% | 5 | 0 |
| hid.dll | 4 | 0% | 100% | 4 | 0 |
| api-ms-win-core-registry-l1-1-0.dll | 7 | 57% | 100% | 3 | 0 |
| api-ms-win-crt-locale-l1-1-0.dll | 8 | 62% | 100% | 3 | 0 |
| api-ms-win-security-base-l1-1-0.dll | 4 | 25% | 100% | 3 | 0 |
| avrt.dll | 3 | 0% | 100% | 3 | 0 |
| imm32.dll | 16 | 81% | 100% | 3 | 0 |
| ntdll.dll | 6 | 50% | 100% | 3 | 0 |
| setupapi.dll | 15 | 80% | 100% | 3 | 0 |
| api-ms-win-crt-heap-l1-1-0.dll | 10 | 80% | 100% | 2 | 0 |
| api-ms-win-crt-private-l1-1-0.dll | 12 | 83% | 100% | 2 | 0 |
| avicap32.dll | 2 | 0% | 100% | 2 | 0 |
| comctl32.dll | 2 | 0% | 50% | 2 | 1 |
| comdlg32.dll | 2 | 0% | 100% | 2 | 0 |
| d3d12.dll | 5 | 60% | 60% | 2 | 2 |
| dnsapi.dll | 2 | 0% | 100% | 2 | 0 |
| msvcp140_atomic_wait.dll | 2 | 0% | 0% | 2 | 2 |
| netapi32.dll | 2 | 0% | 100% | 2 | 0 |
| wtsapi32.dll | 2 | 0% | 100% | 2 | 0 |
| api-ms-win-core-winrt-error-l1-1-1.dll | 1 | 0% | 100% | 1 | 0 |
| api-ms-win-core-winrt-l1-1-0.dll | 1 | 0% | 100% | 1 | 0 |
| api-ms-win-core-winrt-string-l1-1-0.dll | 1 | 0% | 100% | 1 | 0 |
| api-ms-win-crt-multibyte-l1-1-0.dll | 1 | 0% | 100% | 1 | 0 |
| api-ms-win-crt-utility-l1-1-0.dll | 8 | 88% | 100% | 1 | 0 |
| api-ms-win-security-lsalookup-l2-1-0.dll | 1 | 0% | 100% | 1 | 0 |
| dinput8.dll | 1 | 0% | 100% | 1 | 0 |
| dwmapi.dll | 5 | 80% | 100% | 1 | 0 |
| dwrite.dll | 1 | 0% | 100% | 1 | 0 |
| icudt78.dll | 1 | 0% | 0% | 1 | 1 |
| mpr.dll | 1 | 0% | 100% | 1 | 0 |
| msvcp140_1.dll | 1 | 0% | 0% | 1 | 1 |
| msvcp140_2.dll | 1 | 0% | 0% | 1 | 1 |
| psapi.dll | 1 | 0% | 100% | 1 | 0 |
| userenv.dll | 1 | 0% | 100% | 1 | 0 |
| vcruntime140_1.dll | 1 | 0% | 0% | 1 | 1 |


## Decision: the MSVC C++ runtime (vcruntime140 / msvcp140) is recompiled, not shimmed

* **msvcp140.dll** (iostreams, locale facets, `std::thread`/mutex helpers, `_Xlength_error`...) is a C++ ABI:
  inline code compiled into the game reads and writes the objects' private fields (`basic_streambuf` pointers,
  `ios_base` state, facet reference counts, `locale::id` counters) and calls virtual functions through
  Microsoft's vtable layout. It also exports data (`std::cout`, `locale::id` statics), which a shim cannot
  provide. A faithful shim would mean re-implementing Microsoft's STL layout bug-for-bug: 337 distinct exports in
  this corpus alone.
* **vcruntime140.dll / vcruntime140_1.dll** contain the C++ exception machinery (`__CxxFrameHandler3/4` with the
  compressed FH4 tables, `_CxxThrowException`, catch-object copying, RTTI). These run as ordinary guest code on
  top of our SEH dispatcher (`runtime/seh.c` already dispatches language handlers as guest code for MinGW).
* Microsoft's redistributables are therefore **recompiled from the copy the game ships** (app-local deployment is
  common: OpenMW ships `vc_redist`, many games ship the DLLs next to the exe) or from the user's own install of
  the Visual C++ Redistributable. **wintomac never ships them.** They are passed to the recompiler like any other
  DLL: `python -m recomp game.exe vcruntime140.dll vcruntime140_1.dll msvcp140.dll ...`; the loader binds the
  game's imports to the recompiled DLL first and only falls back to shims when the DLL is absent.
* Checked (the DLLs were taken from OpenMW's bundled `vc_redist.x64.exe`, inspected locally and not kept):
  the recompiler lifts vcruntime140 (325 functions), vcruntime140_1 (111) and msvcp140 (1683) without errors.
  Two instructions fall back to traps: `RDSSPQ` (CET shadow stack) and `FNCLEX`. Their own imports
  (ucrt api-ms-win-crt-*, kernel32 incl. thread-pool work items, `_Getdays`/`_Gettnames`/`_Strftime` for the time
  facets, `_lock_locales`, `__strncnt`) are now all shimmed: with `--with-redist` the corpus is at 99.7%.
* **ucrt (api-ms-win-crt-*, ucrtbase)** stays shimmed: it is a C ABI whose job (FILE*, paths, heap, environment,
  time) has to be mapped onto macOS anyway.
* **Fallbacks** for a game that ships without vcruntime140: the C helpers are shimmed in `runtime/crt2.c`
  (`__std_exception_copy/destroy`, `__std_type_info_*`, `_purecall`, `__std_terminate`, `__RTtypeid`,
  `__RTDynamicCast` on the x64 RTTI layout, `__DestructExceptionObject`, `_CxxThrowException` raising the
  standard 0xE06D7363 exception). `__CxxFrameHandler4` is deliberately not shimmed: C++ catch blocks need the real
  handler, so such a game must be given vcruntime140_1.dll.

## What was implemented (this pass)

| file | area |
|---|---|
| `runtime/advapi32.c` | **Registry**: in-memory tree seeded with what games query (Windows 10 22H2 version keys, CPU description, BIOS, DirectX version, MachineGuid, shell folders, theme/locale settings); HKCU persisted to `~/Library/Application Support/wintomac/user.reg` (`WINTOMAC_REGISTRY=<file>` or `none`); Reg{Open,Create,Query,Get,Set,Delete,Enum,QueryInfo}* A/W with Windows buffer/type rules; SHGetValue & co. **Crypto**: RtlGenRandom, CryptGenRandom, BCryptGenRandom, MD5/SHA-1/SHA-2 and HMAC through CryptoAPI and CNG (CommonCrypto). User name, token elevation, SIDs, security descriptors (accepted). |
| `runtime/kernel32x.c` | GetFullPathName and a tracked current directory (SetCurrentDirectory/GetCurrentDirectory, `Z:` = host root), temp names, drive/volume/disk-space queries, file mappings (file-backed and named pagefile sections), GetFileInformationByHandleEx / SetFileInformationByHandle, GetFinalPathNameByHandle, file locks, file times, moves/replace/links, date/time/number/currency formatting with picture strings, locale names and LCIDs, geo info, ordinal/linguistic compares, lstr*, ExpandEnvironmentStrings, computer name, processor topology (GetLogicalProcessorInformation[Ex]), psapi module queries, InitOnce*, heaps, Virtual*Ex, I/O completion ports (as packet queues), thread-pool work/timers/waits, console odds and ends, RtlNtStatusToDosError. |
| `runtime/crt2.c` | C99 math (+ `_dclass`, `_fpclass`, `_controlfp`), `_s` string functions with `_TRUNCATE`/ERANGE rules, wide string and classification functions, multibyte/wide conversions, wide number parsing, `_itoa` family, wide/secure printf variants, `_wopen`/`_sopen`/`fopen_s`/`freopen`, `_stat*`/`_wstat*` with Windows mode bits, `_access`/`_mkdir`/`_unlink`/`_chdir`/`_getcwd`/`_fullpath`/`_splitpath_s`/`_makepath_s`, `_findfirst*`, wide argv/environment, `_get_stream_buffer_pointers` (msvcp140 filebuf), heap extras, `qsort_s`, time zone and `_s` time functions, locale-time data for msvcp140, and the vcruntime140 C helpers. |
| `runtime/win32x.c` | user32 extras (window hierarchy queries, cursors, carets, menus, sys colours, DPI helpers, char-case helpers), gdi32 palettes/regions/no-op drawing, winmm multimedia timers (`timeSetEvent`, real) and empty MIDI/MCI/joystick device lists, shell32/shlwapi (`SHFileOperationW`, `Path*`, `Str*`, `QISearch`, `SHGetSpecialFolderPath`), ole32/oleaut32 (`StringFromCLSID`, BSTR and VARIANT helpers), uxtheme (unthemed), crypt32 base64/hex, dbghelp PE-image helpers, ws2_32 (`WSASend`/`WSARecv`/`WSASendTo`/`WSARecvFrom`, WSA events), avrt, hid, iphlpapi, winhttp, wtsapi32, userenv, comdlg32, comctl32, WinRT activation (fails cleanly). |
| `runtime/ucrt.c` | msvcrt data exports (`_acmdln`, `_fmode`, `_commode`, `__initenv`, `_environ`, `__argc`, `__argv`, `_iob`, `_timezone`, `_daylight`, `_tzname`): `rt_bind_data_imports()` points the module's IAT slots at real variables during CRT start-up; `__set_app_type`, `__lconv_init`. |

Unsupported features return the documented failure value and set the last error (or return an HRESULT/status);
`WINTOMAC_STUBS=1` logs each of them once. Tests: `tests/src/api_registry.c`, `api_paths.c`, `api_crt2.c` with
`tests/expected/*.out` written from documented Windows behaviour.

## Still missing (after)

Generated by `python3 -I tools/api_coverage.py third_party/games/quakespasm/dist third_party/games/vkquake/dist third_party/dxvk-macos build/audio_dsound.exe build/audio_xaudio2.exe build/avx_kernels.exe build/avx_strops.exe build/cpp_exceptions.exe build/cpp_game.exe build/crt_basic.exe build/demo_dx11_window.exe build/demo_dx9_window.exe build/display_fullscreen.exe build/dll_test.exe build/dx11_instanced.exe build/dx11_triangle.exe build/dx12_dxil.exe build/dx12_offscreen.exe build/dx12_present.exe build/dx9_triangle.exe build/files_enum.exe build/free_hello.exe build/gl_offscreen.exe build/input_rawmouse.exe build/input_xinput.exe build/jit_codegen.exe build/jit_missed.exe build/mathlib.dll build/net_winsock.exe build/reloc_lowbase.exe build/seh_basic.exe build/strlib.dll build/threads_atomic.exe build/threads_sync.exe build/vk_offscreen.exe third_party/corpus`.

* binaries scanned: **204** x86-64 PE files (4 skipped: not x86-64 PE32+)
* shim names in the runtime: **6776** (+0 DLL-restricted) in 28 tables
* distinct imported (dll, function) pairs: **20979** - shimmed **2119**, provided by a recompiled DLL **18447**, missing **413**
* coverage (shim + recompiled): **98.0%**
* binaries with every import covered: **122 / 204**

Shim names defined in more than one table (the first table in `shim_tables[]` wins; the Vulkan ones are deliberate hand-written overrides of generated thunks):

* `vkAcquireNextImageKHR`: `shims_vulkan` shadows `shims_vulkan_gen`
* `vkCreateSwapchainKHR`: `shims_vulkan` shadows `shims_vulkan_gen`
* `vkGetPhysicalDeviceSurfaceCapabilities2KHR`: `shims_vulkan` shadows `shims_vulkan_gen`
* `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`: `shims_vulkan` shadows `shims_vulkan_gen`
* `vkQueuePresentKHR`: `shims_vulkan` shadows `shims_vulkan_gen`
* `vkWaitForPresent2KHR`: `shims_vulkan` shadows `shims_vulkan_gen`

## Per-DLL coverage

| DLL | functions | shim | recompiled | missing | coverage |
|---|---:|---:|---:|---:|---:|
| msvcp140.dll | 337 | 0 | 0 | 337 | 0% |
| crypt32.dll | 25 | 8 | 0 | 17 | 32% |
| secur32.dll | 13 | 0 | 0 | 13 | 0% |
| wldap32.dll | 18 | 7 | 0 | 11 | 39% |
| ncrypt.dll | 8 | 0 | 0 | 8 | 0% |
| uiautomationcore.dll | 7 | 0 | 0 | 7 | 0% |
| authz.dll | 6 | 0 | 0 | 6 | 0% |
| oleaut32.dll | 13 | 10 | 0 | 3 | 77% |
| d3d12.dll | 5 | 3 | 0 | 2 | 60% |
| msvcp140_atomic_wait.dll | 2 | 0 | 0 | 2 | 0% |
| comctl32.dll | 2 | 1 | 0 | 1 | 50% |
| icudt78.dll | 1 | 0 | 0 | 1 | 0% |
| msvcp140_1.dll | 1 | 0 | 0 | 1 | 0% |
| msvcp140_2.dll | 1 | 0 | 0 | 1 | 0% |
| shell32.dll | 29 | 28 | 0 | 1 | 97% |
| uxtheme.dll | 21 | 20 | 0 | 1 | 95% |
| vcruntime140_1.dll | 1 | 0 | 0 | 1 | 0% |
| advapi32.dll | 52 | 52 | 0 | 0 | 100% |
| api-ms-win-core-processthreads-l1-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-core-registry-l1-1-0.dll | 7 | 7 | 0 | 0 | 100% |
| api-ms-win-core-synch-l1-2-0.dll | 8 | 8 | 0 | 0 | 100% |
| api-ms-win-core-version-l1-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-core-version-l1-1-1.dll | 2 | 2 | 0 | 0 | 100% |
| api-ms-win-core-winrt-error-l1-1-1.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-core-winrt-l1-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-core-winrt-string-l1-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-crt-convert-l1-1-0.dll | 29 | 29 | 0 | 0 | 100% |
| api-ms-win-crt-environment-l1-1-0.dll | 9 | 9 | 0 | 0 | 100% |
| api-ms-win-crt-filesystem-l1-1-0.dll | 27 | 27 | 0 | 0 | 100% |
| api-ms-win-crt-heap-l1-1-0.dll | 10 | 10 | 0 | 0 | 100% |
| api-ms-win-crt-locale-l1-1-0.dll | 8 | 8 | 0 | 0 | 100% |
| api-ms-win-crt-math-l1-1-0.dll | 93 | 93 | 0 | 0 | 100% |
| api-ms-win-crt-multibyte-l1-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-crt-private-l1-1-0.dll | 12 | 12 | 0 | 0 | 100% |
| api-ms-win-crt-runtime-l1-1-0.dll | 45 | 45 | 0 | 0 | 100% |
| api-ms-win-crt-stdio-l1-1-0.dll | 86 | 86 | 0 | 0 | 100% |
| api-ms-win-crt-string-l1-1-0.dll | 71 | 71 | 0 | 0 | 100% |
| api-ms-win-crt-time-l1-1-0.dll | 15 | 15 | 0 | 0 | 100% |
| api-ms-win-crt-utility-l1-1-0.dll | 8 | 8 | 0 | 0 | 100% |
| api-ms-win-security-base-l1-1-0.dll | 4 | 4 | 0 | 0 | 100% |
| api-ms-win-security-lsalookup-l2-1-0.dll | 1 | 1 | 0 | 0 | 100% |
| api-ms-win-shcore-scaling-l1-1-1.dll | 1 | 1 | 0 | 0 | 100% |
| avcodec-62.dll | 103 | 0 | 103 | 0 | 100% |
| avfilter-11.dll | 22 | 0 | 22 | 0 | 100% |
| avformat-62.dll | 26 | 0 | 26 | 0 | 100% |
| avicap32.dll | 2 | 2 | 0 | 0 | 100% |
| avrt.dll | 3 | 3 | 0 | 0 | 100% |
| avutil-60.dll | 445 | 0 | 445 | 0 | 100% |
| bcrypt.dll | 13 | 13 | 0 | 0 | 100% |
| boost_filesystem-vc143-mt-x64-1_90.dll | 12 | 0 | 12 | 0 | 100% |
| boost_program_options-vc143-mt-x64-1_90.dll | 84 | 0 | 84 | 0 | 100% |
| boost_serialization-vc143-mt-x64-1_90.dll | 30 | 0 | 30 | 0 | 100% |
| brotlicommon.dll | 10 | 0 | 10 | 0 | 100% |
| brotlidec.dll | 1 | 0 | 1 | 0 | 100% |
| bz2.dll | 6 | 0 | 6 | 0 | 100% |
| collada-dom2.5-dp-vc140-mt.dll | 211 | 0 | 211 | 0 | 100% |
| comdlg32.dll | 2 | 2 | 0 | 0 | 100% |
| d3d11.dll | 3 | 0 | 3 | 0 | 100% |
| d3d9.dll | 1 | 0 | 1 | 0 | 100% |
| d3dcompiler_47.dll | 2 | 2 | 0 | 0 | 100% |
| dbghelp.dll | 13 | 13 | 0 | 0 | 100% |
| dinput8.dll | 1 | 1 | 0 | 0 | 100% |
| dnsapi.dll | 2 | 2 | 0 | 0 | 100% |
| dsound.dll | 2 | 2 | 0 | 0 | 100% |
| dwmapi.dll | 5 | 5 | 0 | 0 | 100% |
| dwrite.dll | 1 | 1 | 0 | 0 | 100% |
| dxgi.dll | 2 | 0 | 2 | 0 | 100% |
| fmt.dll | 6 | 0 | 6 | 0 | 100% |
| freetype.dll | 22 | 0 | 22 | 0 | 100% |
| gdi32.dll | 76 | 76 | 0 | 0 | 100% |
| hid.dll | 4 | 4 | 0 | 0 | 100% |
| iconv-2.dll | 3 | 0 | 3 | 0 | 100% |
| icuin78.dll | 46 | 0 | 46 | 0 | 100% |
| icuuc78.dll | 880 | 0 | 880 | 0 | 100% |
| imm32.dll | 16 | 16 | 0 | 0 | 100% |
| iphlpapi.dll | 9 | 9 | 0 | 0 | 100% |
| jpeg62.dll | 24 | 0 | 24 | 0 | 100% |
| kernel32.dll | 422 | 422 | 0 | 0 | 100% |
| libc++.dll | 372 | 0 | 372 | 0 | 100% |
| libcurl.dll | 28 | 0 | 28 | 0 | 100% |
| libflac-8.dll | 8 | 0 | 8 | 0 | 100% |
| libfreetype.dll | 9 | 0 | 9 | 0 | 100% |
| libiconv-2.dll | 3 | 0 | 3 | 0 | 100% |
| libintl-8.dll | 5 | 0 | 5 | 0 | 100% |
| libjpeg-62.dll | 15 | 0 | 15 | 0 | 100% |
| libleveldb.dll | 3 | 0 | 3 | 0 | 100% |
| liblzma.dll | 8 | 0 | 8 | 0 | 100% |
| libmad-0.dll | 12 | 0 | 12 | 0 | 100% |
| libmpg123-0.dll | 13 | 0 | 13 | 0 | 100% |
| libogg-0.dll | 31 | 0 | 31 | 0 | 100% |
| libopus-0.dll | 6 | 0 | 6 | 0 | 100% |
| libopusfile-0.dll | 6 | 0 | 6 | 0 | 100% |
| libpng16.dll | 48 | 0 | 48 | 0 | 100% |
| libsqlite3-0.dll | 24 | 0 | 24 | 0 | 100% |
| libunwind.dll | 9 | 0 | 9 | 0 | 100% |
| libvorbis-0.dll | 22 | 0 | 22 | 0 | 100% |
| libvorbis.dll | 22 | 0 | 22 | 0 | 100% |
| libvorbisfile-3.dll | 7 | 0 | 7 | 0 | 100% |
| libvorbisfile.dll | 11 | 0 | 11 | 0 | 100% |
| libxml2.dll | 33 | 0 | 33 | 0 | 100% |
| libxmp.dll | 10 | 0 | 10 | 0 | 100% |
| libzip.dll | 12 | 0 | 12 | 0 | 100% |
| libzstd.dll | 12 | 0 | 12 | 0 | 100% |
| lua51.dll | 75 | 0 | 75 | 0 | 100% |
| lz4.dll | 8 | 0 | 8 | 0 | 100% |
| mathlib.dll | 6 | 0 | 6 | 0 | 100% |
| minizip.dll | 14 | 0 | 14 | 0 | 100% |
| mpr.dll | 1 | 1 | 0 | 0 | 100% |
| msvcrt.dll | 206 | 206 | 0 | 0 | 100% |
| myguiengine.dll | 579 | 0 | 579 | 0 | 100% |
| netapi32.dll | 2 | 2 | 0 | 0 | 100% |
| ntdll.dll | 6 | 6 | 0 | 0 | 100% |
| ogg.dll | 29 | 0 | 29 | 0 | 100% |
| ole32.dll | 29 | 29 | 0 | 0 | 100% |
| openal32.dll | 46 | 0 | 46 | 0 | 100% |
| opengl32.dll | 135 | 135 | 0 | 0 | 100% |
| openthreads.dll | 35 | 0 | 35 | 0 | 100% |
| osg.dll | 3967 | 0 | 3967 | 0 | 100% |
| osganimation.dll | 383 | 0 | 383 | 0 | 100% |
| osgdb.dll | 341 | 0 | 341 | 0 | 100% |
| osgfx.dll | 132 | 0 | 132 | 0 | 100% |
| osgga.dll | 198 | 0 | 198 | 0 | 100% |
| osgmanipulator.dll | 218 | 0 | 218 | 0 | 100% |
| osgparticle.dll | 366 | 0 | 366 | 0 | 100% |
| osgshadow.dll | 226 | 0 | 226 | 0 | 100% |
| osgsim.dll | 304 | 0 | 304 | 0 | 100% |
| osgterrain.dll | 271 | 0 | 271 | 0 | 100% |
| osgtext.dll | 144 | 0 | 144 | 0 | 100% |
| osgui.dll | 266 | 0 | 266 | 0 | 100% |
| osgutil.dll | 288 | 0 | 288 | 0 | 100% |
| osgviewer.dll | 244 | 0 | 244 | 0 | 100% |
| osgvolume.dll | 386 | 0 | 386 | 0 | 100% |
| psapi.dll | 1 | 1 | 0 | 0 | 100% |
| qt6core.dll | 2283 | 0 | 2283 | 0 | 100% |
| qt6gui.dll | 2878 | 0 | 2878 | 0 | 100% |
| qt6network.dll | 31 | 0 | 31 | 0 | 100% |
| qt6opengl.dll | 25 | 0 | 25 | 0 | 100% |
| qt6openglwidgets.dll | 13 | 0 | 13 | 0 | 100% |
| qt6svg.dll | 11 | 0 | 11 | 0 | 100% |
| qt6widgets.dll | 1551 | 0 | 1551 | 0 | 100% |
| sdl.dll | 50 | 0 | 50 | 0 | 100% |
| sdl2.dll | 197 | 0 | 197 | 0 | 100% |
| sdl3.dll | 115 | 0 | 115 | 0 | 100% |
| setupapi.dll | 15 | 15 | 0 | 0 | 100% |
| shcore.dll | 2 | 2 | 0 | 0 | 100% |
| shlwapi.dll | 9 | 9 | 0 | 0 | 100% |
| sndfile.dll | 6 | 0 | 6 | 0 | 100% |
| swresample-6.dll | 12 | 0 | 12 | 0 | 100% |
| swscale-9.dll | 13 | 0 | 13 | 0 | 100% |
| uriparser.dll | 2 | 0 | 2 | 0 | 100% |
| user32.dll | 298 | 298 | 0 | 0 | 100% |
| userenv.dll | 1 | 1 | 0 | 0 | 100% |
| vcruntime140.dll | 27 | 27 | 0 | 0 | 100% |
| version.dll | 6 | 6 | 0 | 0 | 100% |
| vulkan-1.dll | 87 | 87 | 0 | 0 | 100% |
| winhttp.dll | 5 | 5 | 0 | 0 | 100% |
| winmm.dll | 55 | 55 | 0 | 0 | 100% |
| ws2_32.dll | 56 | 56 | 0 | 0 | 100% |
| wsock32.dll | 25 | 25 | 0 | 0 | 100% |
| wtsapi32.dll | 2 | 2 | 0 | 0 | 100% |
| xaudio2_9.dll | 3 | 3 | 0 | 0 | 100% |
| xinput1_4.dll | 4 | 4 | 0 | 0 | 100% |
| zlib1.dll | 22 | 0 | 22 | 0 | 100% |
| zmusic.dll | 34 | 0 | 34 | 0 | 100% |
| zstd.dll | 12 | 0 | 12 | 0 | 100% |

## Top missing APIs (ranked by projects, then binaries importing them)

| # | API | DLL | projects | binaries |
|---:|---|---|---:|---:|
| 1 | `__CxxFrameHandler4` | vcruntime140_1.dll | 1 | 76 |
| 2 | `?_Xlength_error@std@@YAXPEBD@Z` | msvcp140.dll | 1 | 69 |
| 3 | `?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAXXZ` | msvcp140.dll | 1 | 56 |
| 4 | `?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ` | msvcp140.dll | 1 | 56 |
| 5 | `?good@ios_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 56 |
| 6 | `?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z` | msvcp140.dll | 1 | 56 |
| 7 | `?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHD@Z` | msvcp140.dll | 1 | 56 |
| 8 | `?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z` | msvcp140.dll | 1 | 55 |
| 9 | `?put@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@D@Z` | msvcp140.dll | 1 | 53 |
| 10 | `?widen@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADD@Z` | msvcp140.dll | 1 | 53 |
| 11 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z` | msvcp140.dll | 1 | 51 |
| 12 | `??1?$basic_ios@DU?$char_traits@D@std@@@std@@UEAA@XZ` | msvcp140.dll | 1 | 50 |
| 13 | `?rdbuf@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ` | msvcp140.dll | 1 | 48 |
| 14 | `??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ` | msvcp140.dll | 1 | 47 |
| 15 | `??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ` | msvcp140.dll | 1 | 47 |
| 16 | `??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAA@XZ` | msvcp140.dll | 1 | 47 |
| 17 | `?showmanyc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JXZ` | msvcp140.dll | 1 | 47 |
| 18 | `?xsgetn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEAD_J@Z` | msvcp140.dll | 1 | 47 |
| 19 | `?xsputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEBD_J@Z` | msvcp140.dll | 1 | 47 |
| 20 | `?flags@ios_base@std@@QEBAHXZ` | msvcp140.dll | 1 | 46 |
| 21 | `?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ` | msvcp140.dll | 1 | 45 |
| 22 | `?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ` | msvcp140.dll | 1 | 45 |
| 23 | `?_Xbad_alloc@std@@YAXXZ` | msvcp140.dll | 1 | 45 |
| 24 | `?imbue@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAXAEBVlocale@2@@Z` | msvcp140.dll | 1 | 45 |
| 25 | `?setbuf@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAPEAV12@PEAD_J@Z` | msvcp140.dll | 1 | 45 |
| 26 | `?sync@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ` | msvcp140.dll | 1 | 45 |
| 27 | `?uflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ` | msvcp140.dll | 1 | 45 |
| 28 | `?uncaught_exception@std@@YA_NXZ` | msvcp140.dll | 1 | 45 |
| 29 | `?width@ios_base@std@@QEAA_J_J@Z` | msvcp140.dll | 1 | 45 |
| 30 | `?width@ios_base@std@@QEBA_JXZ` | msvcp140.dll | 1 | 45 |
| 31 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@H@Z` | msvcp140.dll | 1 | 44 |
| 32 | `?fill@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADXZ` | msvcp140.dll | 1 | 44 |
| 33 | `?tie@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_ostream@DU?$char_traits@D@std@@@2@XZ` | msvcp140.dll | 1 | 44 |
| 34 | `?eback@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 42 |
| 35 | `?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z` | msvcp140.dll | 1 | 42 |
| 36 | `??0_Lockit@std@@QEAA@H@Z` | msvcp140.dll | 1 | 41 |
| 37 | `??1_Lockit@std@@QEAA@XZ` | msvcp140.dll | 1 | 41 |
| 38 | `?gbump@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXH@Z` | msvcp140.dll | 1 | 41 |
| 39 | `?gptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 41 |
| 40 | `?_Id_cnt@id@locale@std@@0HA` | msvcp140.dll | 1 | 40 |
| 41 | `??0?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@@Z` | msvcp140.dll | 1 | 39 |
| 42 | `??1?$basic_iostream@DU?$char_traits@D@std@@@std@@UEAA@XZ` | msvcp140.dll | 1 | 39 |
| 43 | `?_Getgloballocale@locale@std@@CAPEAV_Locimp@12@XZ` | msvcp140.dll | 1 | 39 |
| 44 | `?egptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 39 |
| 45 | `?epptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 39 |
| 46 | `?pptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 39 |
| 47 | `?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD0@Z` | msvcp140.dll | 1 | 39 |
| 48 | `?_Pninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ` | msvcp140.dll | 1 | 36 |
| 49 | `?setp@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z` | msvcp140.dll | 1 | 36 |
| 50 | `?_Getcat@?$ctype@D@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z` | msvcp140.dll | 1 | 33 |
| 51 | `?id@?$ctype@D@std@@2V0locale@2@A` | msvcp140.dll | 1 | 33 |
| 52 | `?sgetc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ` | msvcp140.dll | 1 | 33 |
| 53 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@I@Z` | msvcp140.dll | 1 | 32 |
| 54 | `?_Ipfx@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA_N_N@Z` | msvcp140.dll | 1 | 32 |
| 55 | `?getloc@ios_base@std@@QEBA?AVlocale@2@XZ` | msvcp140.dll | 1 | 32 |
| 56 | `?_Xout_of_range@std@@YAXPEBD@Z` | msvcp140.dll | 1 | 31 |
| 57 | `?snextc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ` | msvcp140.dll | 1 | 31 |
| 58 | `??0?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z` | msvcp140.dll | 1 | 29 |
| 59 | `??1?$basic_istream@DU?$char_traits@D@std@@@std@@UEAA@XZ` | msvcp140.dll | 1 | 29 |
| 60 | `??5?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@AEAH@Z` | msvcp140.dll | 1 | 26 |
| 61 | `?setf@ios_base@std@@QEAAHHH@Z` | msvcp140.dll | 1 | 25 |
| 62 | `??Bios_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 24 |
| 63 | `?_Init@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXXZ` | msvcp140.dll | 1 | 24 |
| 64 | `?pbase@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ` | msvcp140.dll | 1 | 24 |
| 65 | `?read@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@PEAD_J@Z` | msvcp140.dll | 1 | 24 |
| 66 | `?unshift@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEAD1AEAPEAD@Z` | msvcp140.dll | 1 | 24 |
| 67 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@M@Z` | msvcp140.dll | 1 | 23 |
| 68 | `?fail@ios_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 23 |
| 69 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@N@Z` | msvcp140.dll | 1 | 22 |
| 70 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAVios_base@1@AEAV21@@Z@Z` | msvcp140.dll | 1 | 22 |
| 71 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_K@Z` | msvcp140.dll | 1 | 22 |
| 72 | `?eof@ios_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 22 |
| 73 | `??0?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z` | msvcp140.dll | 1 | 21 |
| 74 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@PEBX@Z` | msvcp140.dll | 1 | 21 |
| 75 | `?getloc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEBA?AVlocale@2@XZ` | msvcp140.dll | 1 | 21 |
| 76 | `?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A` | msvcp140.dll | 1 | 21 |
| 77 | `?sbumpc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ` | msvcp140.dll | 1 | 21 |
| 78 | `?write@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@PEBD_J@Z` | msvcp140.dll | 1 | 21 |
| 79 | `??1?$basic_ostream@DU?$char_traits@D@std@@@std@@UEAA@XZ` | msvcp140.dll | 1 | 20 |
| 80 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_N@Z` | msvcp140.dll | 1 | 20 |
| 81 | `?_Getcat@?$codecvt@DDU_Mbstatet@@@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z` | msvcp140.dll | 1 | 20 |
| 82 | `?always_noconv@codecvt_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 20 |
| 83 | `?in@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEBD1AEAPEBDPEAD3AEAPEAD@Z` | msvcp140.dll | 1 | 20 |
| 84 | `?out@?$codecvt@DDU_Mbstatet@@@std@@QEBAHAEAU_Mbstatet@@PEBD1AEAPEBDPEAD3AEAPEAD@Z` | msvcp140.dll | 1 | 20 |
| 85 | `?tellg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@U_Mbstatet@@@2@XZ` | msvcp140.dll | 1 | 20 |
| 86 | `?seekg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@_JH@Z` | msvcp140.dll | 1 | 19 |
| 87 | `??7ios_base@std@@QEBA_NXZ` | msvcp140.dll | 1 | 17 |
| 88 | `?_Decref@facet@locale@std@@UEAAPEAV_Facet_base@3@XZ` | msvcp140.dll | 1 | 16 |
| 89 | `?_Incref@facet@locale@std@@UEAAXXZ` | msvcp140.dll | 1 | 16 |
| 90 | `?_Init@locale@std@@CAPEAV_Locimp@12@_N@Z` | msvcp140.dll | 1 | 16 |
| 91 | `?_Syserror_map@std@@YAPEBDH@Z` | msvcp140.dll | 1 | 16 |
| 92 | `?clear@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z` | msvcp140.dll | 1 | 16 |
| 93 | `?cout@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A` | msvcp140.dll | 1 | 16 |
| 94 | `?get@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAHXZ` | msvcp140.dll | 1 | 16 |
| 95 | `?unsetf@ios_base@std@@QEAAXH@Z` | msvcp140.dll | 1 | 16 |
| 96 | `_Mtx_lock` | msvcp140.dll | 1 | 16 |
| 97 | `_Mtx_unlock` | msvcp140.dll | 1 | 16 |
| 98 | `?_Throw_Cpp_error@std@@YAXH@Z` | msvcp140.dll | 1 | 15 |
| 99 | `?peek@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAHXZ` | msvcp140.dll | 1 | 15 |
| 100 | `?setf@ios_base@std@@QEAAHH@Z` | msvcp140.dll | 1 | 14 |
| 101 | `?setw@std@@YA?AU?$_Smanip@_J@1@_J@Z` | msvcp140.dll | 1 | 14 |
| 102 | `?uncaught_exceptions@std@@YAHXZ` | msvcp140.dll | 1 | 14 |
| 103 | `?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z` | msvcp140.dll | 1 | 13 |
| 104 | `?_Winerror_map@std@@YAHH@Z` | msvcp140.dll | 1 | 13 |
| 105 | `?ignore@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@_JH@Z` | msvcp140.dll | 1 | 13 |
| 106 | `?overflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHH@Z` | msvcp140.dll | 1 | 13 |
| 107 | `?pbackfail@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHH@Z` | msvcp140.dll | 1 | 13 |
| 108 | `??0_Locinfo@std@@QEAA@PEBD@Z` | msvcp140.dll | 1 | 12 |
| 109 | `??0facet@locale@std@@IEAA@_K@Z` | msvcp140.dll | 1 | 12 |
| 110 | `??1_Locinfo@std@@QEAA@XZ` | msvcp140.dll | 1 | 12 |
| 111 | `??1facet@locale@std@@MEAA@XZ` | msvcp140.dll | 1 | 12 |
| 112 | `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@_J@Z` | msvcp140.dll | 1 | 12 |
| 113 | `?_Getcvt@_Locinfo@std@@QEBA?AU_Cvtvec@@XZ` | msvcp140.dll | 1 | 12 |
| 114 | `?_Getfalse@_Locinfo@std@@QEBAPEBDXZ` | msvcp140.dll | 1 | 12 |
| 115 | `?_Gettrue@_Locinfo@std@@QEBAPEBDXZ` | msvcp140.dll | 1 | 12 |
| 116 | `?cerr@std@@3V?$basic_ostream@DU?$char_traits@D@std@@@1@A` | msvcp140.dll | 1 | 12 |
| 117 | `?exceptions@ios_base@std@@QEAAXH@Z` | msvcp140.dll | 1 | 12 |
| 118 | `?gcount@?$basic_istream@DU?$char_traits@D@std@@@std@@QEBA_JXZ` | msvcp140.dll | 1 | 12 |
| 119 | `?id@?$numpunct@D@std@@2V0locale@2@A` | msvcp140.dll | 1 | 12 |
| 120 | `?underflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ` | msvcp140.dll | 1 | 12 |

## Missing APIs by DLL

* **msvcp140.dll** (337): `?_Xlength_error@std@@YAXPEBD@Z`x69, `?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAXXZ`x56, `?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ`x56, `?good@ios_base@std@@QEBA_NXZ`x56, `?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z`x56, `?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHD@Z`x56, `?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z`x55, `?put@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@D@Z`x53, `?widen@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADD@Z`x53, `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z`x51, `??1?$basic_ios@DU?$char_traits@D@std@@@std@@UEAA@XZ`x50, `?rdbuf@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_streambuf@DU?$char_traits@D@std@@@2@XZ`x48, `??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ`x47, `??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ`x47, `??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAA@XZ`x47, `?showmanyc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JXZ`x47, `?xsgetn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEAD_J@Z`x47, `?xsputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEBD_J@Z`x47, `?flags@ios_base@std@@QEBAHXZ`x46, `?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ`x45, `?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ`x45, `?_Xbad_alloc@std@@YAXXZ`x45, `?imbue@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAXAEBVlocale@2@@Z`x45, `?setbuf@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAPEAV12@PEAD_J@Z`x45, `?sync@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ`x45, `?uflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ`x45, `?uncaught_exception@std@@YA_NXZ`x45, `?width@ios_base@std@@QEAA_J_J@Z`x45, `?width@ios_base@std@@QEBA_JXZ`x45, `??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@H@Z`x44, `?fill@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADXZ`x44, `?tie@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBAPEAV?$basic_ostream@DU?$char_traits@D@std@@@2@XZ`x44, `?eback@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ`x42, `?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z`x42, `??0_Lockit@std@@QEAA@H@Z`x41, `??1_Lockit@std@@QEAA@XZ`x41, `?gbump@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXH@Z`x41, `?gptr@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEBAPEADXZ`x41, `?_Id_cnt@id@locale@std@@0HA`x40, `??0?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@@Z`x39 (+297 more)
* **crypt32.dll** (17): `CertAddCertificateContextToStore`, `CertCreateCertificateChainEngine`, `CertCreateCertificateContext`, `CertCreateSelfSignCertificate`, `CertFindExtension`, `CertFreeCertificateChain`, `CertFreeCertificateChainEngine`, `CertGetCertificateChain`, `CertGetCertificateContextProperty`, `CertGetNameStringA`, `CertSetCertificateContextProperty`, `CertStrToNameA`, `CryptDecodeObjectEx`, `CryptEncodeObjectEx`, `CryptHashCertificate2`, `CryptQueryObject`, `PFXImportCertStore`
* **secur32.dll** (13): `AcceptSecurityContext`, `AcquireCredentialsHandleA`, `ApplyControlToken`, `DecryptMessage`, `DeleteSecurityContext`, `EncryptMessage`, `FreeContextBuffer`, `FreeCredentialsHandle`, `InitSecurityInterfaceA`, `InitSecurityInterfaceW`, `InitializeSecurityContextA`, `QueryContextAttributesA`, `SetContextAttributesA`
* **wldap32.dll** (11): `ber_free`, `ldap_first_attribute`, `ldap_first_entry`, `ldap_get_dn`, `ldap_get_values_len`, `ldap_memfree`, `ldap_msgfree`, `ldap_next_attribute`, `ldap_next_entry`, `ldap_search_s`, `ldap_value_free_len`
* **ncrypt.dll** (8): `NCryptCreatePersistedKey`, `NCryptDeleteKey`, `NCryptExportKey`, `NCryptFinalizeKey`, `NCryptFreeObject`, `NCryptImportKey`, `NCryptOpenStorageProvider`, `NCryptSetProperty`
* **uiautomationcore.dll** (7): `UiaGetReservedMixedAttributeValue`, `UiaGetReservedNotSupportedValue`, `UiaHostProviderFromHwnd`, `UiaLookupId`, `UiaRaiseAutomationEvent`, `UiaRaiseAutomationPropertyChangedEvent`, `UiaReturnRawElementProvider`
* **authz.dll** (6): `AuthzAccessCheck`, `AuthzFreeContext`, `AuthzFreeResourceManager`, `AuthzInitializeContextFromSid`, `AuthzInitializeContextFromToken`, `AuthzInitializeResourceManager`
* **oleaut32.dll** (3): `#26`, `#411`, `#417`
* **d3d12.dll** (2): `#101`, `#102`
* **msvcp140_atomic_wait.dll** (2): `__std_atomic_notify_all_direct`, `__std_atomic_wait_direct`
* **vcruntime140_1.dll** (1): `__CxxFrameHandler4`x76
* **msvcp140_1.dll** (1): `_Aligned_get_default_resource`x3
* **comctl32.dll** (1): `#345`
* **icudt78.dll** (1): `icudt78_dat`
* **msvcp140_2.dll** (1): `__std_smf_hypot3f`
* **shell32.dll** (1): `#727`
* **uxtheme.dll** (1): `#47`

## Binaries

| binary | imports | missing |
|---|---:|---:|
| third_party/corpus/openmw/openmw.exe | 3605 | 211 |
| third_party/corpus/openmw/OpenAL32.dll | 356 | 183 |
| third_party/corpus/openmw/openmw-navmeshtool.exe | 1497 | 172 |
| third_party/corpus/openmw/openmw-cs.exe | 4016 | 170 |
| third_party/corpus/openmw/openmw-bulletobjecttool.exe | 1435 | 166 |
| third_party/corpus/openmw/openmw-launcher.exe | 1787 | 143 |
| third_party/corpus/openmw/openmw-wizard.exe | 1323 | 112 |
| third_party/corpus/openmw/boost_program_options-vc143-mt-x64-1_90.dll | 180 | 110 |
| third_party/corpus/openmw/MyGUIEngine.dll | 212 | 104 |
| third_party/corpus/openmw/osg.dll | 340 | 103 |
| third_party/corpus/openmw/esmtool.exe | 257 | 101 |
| third_party/corpus/openmw/openmw-essimporter.exe | 372 | 101 |
| third_party/corpus/openmw/osgDB.dll | 745 | 101 |
| third_party/corpus/openmw/bsatool.exe | 251 | 98 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_osg.dll | 336 | 95 |
| third_party/corpus/openmw/niftest.exe | 258 | 94 |
| third_party/corpus/openmw/openmw-iniimporter.exe | 224 | 90 |
| third_party/corpus/openmw/osgPresentation.dll | 1281 | 90 |
| third_party/corpus/openmw/collada-dom2.5-dp-vc140-mt.dll | 224 | 82 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_dae.dll | 1208 | 82 |
| third_party/corpus/openmw/boost_serialization-vc143-mt-x64-1_90.dll | 135 | 77 |
| third_party/corpus/openmw/boost_graph-vc143-mt-x64-1_90.dll | 129 | 74 |
| third_party/corpus/openmw/osgViewer.dll | 1372 | 71 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_dds.dll | 251 | 68 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_jpeg.dll | 248 | 66 |
| third_party/corpus/openmw/icutu78.dll | 350 | 64 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osg.dll | 2950 | 64 |
| third_party/corpus/openmw/boost_wserialization-vc143-mt-x64-1_90.dll | 147 | 63 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_ktx.dll | 253 | 63 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_png.dll | 258 | 62 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgtext.dll | 345 | 60 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osganimation.dll | 753 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgga.dll | 345 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgparticle.dll | 605 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgshadow.dll | 421 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgsim.dll | 554 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgterrain.dll | 484 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgui.dll | 472 | 59 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgvolume.dll | 566 | 59 |
| third_party/corpus/openmw/osgAnimation.dll | 749 | 56 |
| third_party/corpus/openmw/osgShadow.dll | 827 | 55 |
| third_party/corpus/openmw/osgText.dll | 638 | 55 |
| third_party/corpus/openmw/osgUtil.dll | 1484 | 55 |
| third_party/corpus/openmw/osgWidget.dll | 631 | 54 |
| third_party/corpus/openmw/osgTerrain.dll | 608 | 51 |
| third_party/corpus/openmw/osgSim.dll | 907 | 50 |
| third_party/corpus/openmw/osgUI.dll | 599 | 49 |
| third_party/corpus/openmw/Qt6Core.dll | 388 | 46 |
| third_party/corpus/openmw/osgFX.dll | 695 | 45 |
| third_party/corpus/openmw/osgGA.dll | 381 | 30 |
| third_party/corpus/openmw/Qt6Widgets.dll | 3068 | 28 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_bmp.dll | 190 | 28 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_freetype.dll | 329 | 26 |
| third_party/corpus/openmw/avformat-62.dll | 551 | 25 |
| third_party/corpus/openmw/fmt.dll | 97 | 24 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libcurl.dll | 194 | 22 |
| third_party/corpus/openmw/boost_filesystem-vc143-mt-x64-1_90.dll | 111 | 22 |
| third_party/corpus/openmw/osgVolume.dll | 760 | 22 |
| third_party/corpus/openmw/osgParticle.dll | 510 | 21 |
| third_party/corpus/openmw/OpenThreads.dll | 87 | 19 |
| third_party/corpus/openmw/icuio78.dll | 122 | 19 |
| third_party/corpus/openmw/osgManipulator.dll | 509 | 18 |
| third_party/corpus/openmw/Qt6Gui.dll | 1490 | 13 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_tga.dll | 169 | 9 |
| third_party/corpus/godot/Godot_v4.7.2-stable_win64.exe | 717 | 8 |
| third_party/corpus/openmw/icuuc78.dll | 105 | 7 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgfx.dll | 269 | 5 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgmanipulator.dll | 368 | 5 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgviewer.dll | 236 | 5 |
| third_party/corpus/openmw/platforms/qwindows.dll | 1645 | 5 |
| third_party/corpus/openmw/Qt6Network.dll | 936 | 3 |
| third_party/corpus/openmw/boost_atomic-vc143-mt-x64-1_90.dll | 46 | 3 |
| third_party/corpus/openmw/boost_iostreams-vc143-mt-x64-1_90.dll | 95 | 3 |
| third_party/corpus/openmw/boost_chrono-vc143-mt-x64-1_90.dll | 52 | 2 |
| third_party/corpus/openmw/boost_random-vc143-mt-x64-1_90.dll | 51 | 2 |
| third_party/corpus/openmw/boost_thread-vc143-mt-x64-1_90.dll | 82 | 2 |
| third_party/corpus/openmw/icuin78.dll | 898 | 2 |
| third_party/corpus/gzdoom/gzdoom.exe | 379 | 1 |
| third_party/corpus/openmw/avdevice-62.dll | 187 | 1 |
| third_party/corpus/openmw/boost_container-vc143-mt-x64-1_90.dll | 54 | 1 |
| third_party/corpus/openmw/osgPlugins-3.6.5/osgdb_serializers_osgutil.dll | 85 | 1 |
| third_party/corpus/openmw/styles/qwindowsvistastyle.dll | 491 | 1 |
| build/audio_dsound.exe | 50 | 0 |
| build/audio_xaudio2.exe | 50 | 0 |
| build/avx_kernels.exe | 42 | 0 |
| build/avx_strops.exe | 42 | 0 |
| build/cpp_exceptions.exe | 115 | 0 |
| build/cpp_game.exe | 117 | 0 |
| build/crt_basic.exe | 43 | 0 |
| build/demo_dx11_window.exe | 59 | 0 |
| build/demo_dx9_window.exe | 59 | 0 |
| build/display_fullscreen.exe | 56 | 0 |
| build/dll_test.exe | 51 | 0 |
| build/dx11_instanced.exe | 44 | 0 |
| build/dx11_triangle.exe | 46 | 0 |
| build/dx12_dxil.exe | 48 | 0 |
| build/dx12_offscreen.exe | 49 | 0 |
| build/dx12_present.exe | 53 | 0 |
| build/dx9_triangle.exe | 49 | 0 |
| build/files_enum.exe | 63 | 0 |
| build/free_hello.exe | 3 | 0 |
| build/gl_offscreen.exe | 64 | 0 |
| build/input_rawmouse.exe | 57 | 0 |
| build/input_xinput.exe | 46 | 0 |
| build/jit_codegen.exe | 45 | 0 |
| build/jit_missed.exe | 45 | 0 |
| build/mathlib.dll | 24 | 0 |
| build/net_winsock.exe | 72 | 0 |
| build/reloc_lowbase.exe | 43 | 0 |
| build/seh_basic.exe | 43 | 0 |
| build/strlib.dll | 24 | 0 |
| build/threads_atomic.exe | 43 | 0 |
| build/threads_sync.exe | 59 | 0 |
| build/vk_offscreen.exe | 90 | 0 |
| third_party/corpus/godot/Godot_v4.7.2-stable_win64_console.exe | 68 | 0 |
| third_party/corpus/gzdoom/openal32.dll | 189 | 0 |
| third_party/corpus/gzdoom/sndfile.dll | 82 | 0 |
| third_party/corpus/gzdoom/zmusic.dll | 137 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/OpenAL32.dll | 350 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/SDL2.dll | 361 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libc++.dll | 183 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libfreetype.dll | 56 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libiconv-2.dll | 56 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libintl-8.dll | 98 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libjpeg-62.dll | 35 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libleveldb.dll | 163 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libpng16.dll | 62 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libsqlite3-0.dll | 125 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libunwind.dll | 43 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libvorbis.dll | 73 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libvorbisfile.dll | 90 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libwinpthread-1.dll | 79 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libzip.dll | 104 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/libzstd.dll | 40 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/luanti.exe | 854 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/ogg.dll | 45 | 0 |
| third_party/corpus/luanti/luanti-5.18.0-win64/bin/zlib1.dll | 37 | 0 |
| third_party/corpus/openmw/Qt6OpenGL.dll | 657 | 0 |
| third_party/corpus/openmw/Qt6OpenGLWidgets.dll | 233 | 0 |
| third_party/corpus/openmw/Qt6Svg.dll | 626 | 0 |
| third_party/corpus/openmw/SDL2.dll | 388 | 0 |
| third_party/corpus/openmw/SDL2_image.dll | 123 | 0 |
| third_party/corpus/openmw/avcodec-62.dll | 388 | 0 |
| third_party/corpus/openmw/avfilter-11.dll | 405 | 0 |
| third_party/corpus/openmw/avutil-60.dll | 140 | 0 |
| third_party/corpus/openmw/boost_date_time-vc143-mt-x64-1_90.dll | 25 | 0 |
| third_party/corpus/openmw/brotlicommon.dll | 27 | 0 |
| third_party/corpus/openmw/brotlidec.dll | 37 | 0 |
| third_party/corpus/openmw/brotlienc.dll | 37 | 0 |
| third_party/corpus/openmw/bz2.dll | 45 | 0 |
| third_party/corpus/openmw/charset-1.dll | 34 | 0 |
| third_party/corpus/openmw/freetype.dll | 85 | 0 |
| third_party/corpus/openmw/iconengines/qsvgicon.dll | 154 | 0 |
| third_party/corpus/openmw/iconv-2.dll | 40 | 0 |
| third_party/corpus/openmw/imageformats/qsvg.dll | 139 | 0 |
| third_party/corpus/openmw/jpeg62.dll | 42 | 0 |
| third_party/corpus/openmw/liblzma.dll | 47 | 0 |
| third_party/corpus/openmw/libpng16.dll | 61 | 0 |
| third_party/corpus/openmw/libxml2.dll | 95 | 0 |
| third_party/corpus/openmw/lua51.dll | 138 | 0 |
| third_party/corpus/openmw/lz4.dll | 31 | 0 |
| third_party/corpus/openmw/minizip.dll | 59 | 0 |
| third_party/corpus/openmw/pkgconf-7.dll | 73 | 0 |
| third_party/corpus/openmw/swresample-6.dll | 69 | 0 |
| third_party/corpus/openmw/swscale-9.dll | 92 | 0 |
| third_party/corpus/openmw/tiff.dll | 104 | 0 |
| third_party/corpus/openmw/turbojpeg.dll | 55 | 0 |
| third_party/corpus/openmw/uriparser.dll | 37 | 0 |
| third_party/corpus/openmw/zlib1.dll | 42 | 0 |
| third_party/corpus/openmw/zstd.dll | 50 | 0 |
| third_party/corpus/raylib/raylib-6.2_win64_msvc16/lib/raylib.dll | 217 | 0 |
| third_party/corpus/sdl2/SDL2.dll | 275 | 0 |
| third_party/corpus/sdl3/SDL3.dll | 360 | 0 |
| third_party/dxvk-macos/dxvk-macOS-async-v1.10.3-20230507/x64/d3d10core.dll | 96 | 0 |
| third_party/dxvk-macos/dxvk-macOS-async-v1.10.3-20230507/x64/d3d11.dll | 170 | 0 |
| third_party/dxvk-macos/dxvk-macOS-async-v1.10.3-20230507/x64/d3d9.dll | 190 | 0 |
| third_party/dxvk-macos/dxvk-macOS-async-v1.10.3-20230507/x64/dxgi.dll | 168 | 0 |
| third_party/games/quakespasm/dist/SDL.dll | 209 | 0 |
| third_party/games/quakespasm/dist/SDL2.dll | 298 | 0 |
| third_party/games/quakespasm/dist/libFLAC-8.dll | 64 | 0 |
| third_party/games/quakespasm/dist/libmad-0.dll | 43 | 0 |
| third_party/games/quakespasm/dist/libmikmod-3.dll | 61 | 0 |
| third_party/games/quakespasm/dist/libmpg123-0.dll | 64 | 0 |
| third_party/games/quakespasm/dist/libogg-0.dll | 45 | 0 |
| third_party/games/quakespasm/dist/libopus-0.dll | 49 | 0 |
| third_party/games/quakespasm/dist/libopusfile-0.dll | 82 | 0 |
| third_party/games/quakespasm/dist/libvorbis-0.dll | 72 | 0 |
| third_party/games/quakespasm/dist/libvorbisfile-3.dll | 90 | 0 |
| third_party/games/quakespasm/dist/libxmp.dll | 75 | 0 |
| third_party/games/quakespasm/dist/quakespasm-reloc.exe | 331 | 0 |
| third_party/games/quakespasm/dist/quakespasm-sdl12.exe | 295 | 0 |
| third_party/games/quakespasm/dist/quakespasm.exe | 317 | 0 |
| third_party/games/vkquake/dist/SDL3.dll | 360 | 0 |
| third_party/games/vkquake/dist/libFLAC-8.dll | 64 | 0 |
| third_party/games/vkquake/dist/libmad-0.dll | 43 | 0 |
| third_party/games/vkquake/dist/libmikmod-3.dll | 61 | 0 |
| third_party/games/vkquake/dist/libmpg123-0.dll | 61 | 0 |
| third_party/games/vkquake/dist/libogg-0.dll | 45 | 0 |
| third_party/games/vkquake/dist/libopus-0.dll | 49 | 0 |
| third_party/games/vkquake/dist/libopusfile-0.dll | 82 | 0 |
| third_party/games/vkquake/dist/libvorbis-0.dll | 72 | 0 |
| third_party/games/vkquake/dist/libvorbisfile-3.dll | 90 | 0 |
| third_party/games/vkquake/dist/libxmp.dll | 50 | 0 |
| third_party/games/vkquake/dist/vkQuake.exe | 465 | 0 |
