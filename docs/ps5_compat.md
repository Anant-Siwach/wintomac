# PS5 compatibility sweep

Legal test programs only: homebrew built from source with the open ps5-payload-sdk, and small programs written for
this repository (`tests/ps5/`). Each runs through the whole chain (SDK build, AnyPS5 relinker `--windows`, wintomac
recompile of the program plus the AnyPS5 libraries it needs, native arm64 run) headless with
`WINTOMAC_HEADLESS=1 SDL_AUDIO_DRIVER=dummy` and is compared with a native clang build of the same source (portable
programs) or with hand-written expectations (programs that call `sce*` APIs): `tests/ps5/expected/<name>.out`.
Graphics are covered separately ([ps5_graphics.md](ps5_graphics.md)).

## Run it

```sh
tools/ps5_setup.sh --sdk        # once
make ps5-check                  # all programs in tests/ps5/tests.txt (skips if AnyPS5/SDK are not set up)
make ps5-check PS5="thr_mutex net_udp"   # some of them
tools/ps5_suite.sh c ...        # developer use: PS5_NOBUILD=1 reruns the tests without rebuilding, PS5_UPDATE=1 rewrites expectations
```

C programs of one suite are linked into one ELF with a name dispatcher (`tools/ps5_suite.sh`), so the expensive part
(relink, lifting ~8 M instructions of AnyPS5 libraries, compiling them) happens once per suite instead of once per
program. A first build takes 10 to 40 minutes on a busy machine (`WINTOMAC_JOBS=3` is the default of these scripts);
the tests themselves take seconds. `tools/ps5_compat.sh <src> <name>` builds and runs a single program.
Output lines that AnyPS5 logs itself (`[src/...]`, `[memory] ...`, `[time] ...`) are filtered before comparing.

Console paths: `/app0` and `/data` live in the program's folder (`app0/`, `data/`); the test harness creates `data/`.

## Results

| program | area | result | notes / fix |
|---|---|---|---|
| `hello.c` | stdio, heap, libm, 64-bit/float math, exit code | pass (standalone) | in the shared suite it did not run (see open issue 3) |
| `thr_mutex.c` | pthreads: create/join, mutex (plain, recursive, trylock), condvar, rwlock, TLS key, once, semaphore | pass | AnyPS5 logs `[src/Thread.cpp RunThread]` per thread: filtered |
| `time_test.c` | clock_gettime, nanosleep/usleep, gettimeofday, gmtime/strftime/timegm | pass | |
| `mem_map.c` | malloc/calloc/realloc/posix_memalign, `sceKernelMmap/Mprotect/Munmap`, `sceKernelAllocateDirectMemory/MapDirectMemory/ReleaseDirectMemory`, `sceKernelMapNamedFlexibleMemory` | pass | the SDK's own `mmap`/`mprotect` are raw syscall wrappers; the relinker refuses them (`Forbidden syscall instruction`). Use the `sceKernel*` forms (titles do) |
| `net_loop.c` | BSD sockets over loopback: TCP echo, UDP via sendto/poll/recvfrom, getsockname | pass | **fixed**: UDP failed because AnyPS5 calls `WSAIoctl(SIO_UDP_CONNRESET)` on every UDP socket and `runtime/winsock.c` returned `WSAEOPNOTSUPP`; `WSAIoctl` now accepts `SIO_UDP_CONNRESET`/`NETRESET` (no-op) and `FIONBIO` |
| `net_udp.c` | UDP variants: bound/unbound sender, non-blocking, `MSG_DONTWAIT` | pass | same fix |
| `sce_net.c` | `sceNetInit/PoolCreate/Socket/Bind/Listen/Accept/Connect/Send/Recv` loopback echo | pass | |
| `sce_kernel.c` | `scePthread*`, `sceKernelCreateSema/EventFlag`, `sceKernelUsleep`, process time and TSC frequency | pass | handles (`SceKernelSema`, event flags) are 64-bit pointers: declare them `void *` |
| `audio_out.c` | `sceUserService*`, `sceAudioOutInit/Open/Output/Close` (10 blocks, dummy SDL driver, silent) | pass | |
| `pad_none.c` | `scePadInit/Open/ReadState/Close` with no controller | pass | `scePadOpen` succeeds and `ReadState` reports `connected=0`, no buttons |
| `cxx_throw.cpp` | C++ exceptions through AnyPS5's HLE C++ runtime | see ps5.md (other track) | not owned by this sweep |
| `cxx_std.cpp` | C++ with the SDK's static libc++: exceptions, RTTI, iostream, STL, std::thread | not run | links only with `-lc++ -lc++abi -lunwind -lc` (added to `tools/ps5_build.sh`); not run to completion because of the build-load limit. The static `libc++abi` uses its own unwinder, not AnyPS5's |
| `file_io.c` | fopen/fread/fseek, open/read/write/lseek, stat, rename, unlink, mkdir/rmdir under `/data` | **fail** (open issue 1) | everything passes except `readdir()` returning no entries |
| SDK `hello_stdio`, `hello_argv`, `hello_world` | stdin at EOF, argv, `sceKernelSendNotificationRequest` (AnyPS5 prints `[notification] Hello, world!`) | pass | unchanged sources, suite `sdk` |
| SDK `hello_dlfcn` | `dlopen("libSceRandom.sprx")`, `dlsym` | **fail** (open issue 6) | prints `(null)` three times instead of a random hexdump |
| SDK `hello_kenv` | `kenv()` | fail at relink | `kenv` is a raw `syscall` wrapper: `Forbidden syscall instruction`; by design |
| SDK `list_files`, `ps`, `mntinfo`, `hwinfo`, `test_privileges`, `install_app`, `arbitrary_syscall`, `notify*`, `browser` | kernel exploit / privileged system calls (`ps5/kernel.h`) | not applicable | need a jailbroken console |
| SDK `hello_cxx`, `hello_cxx23`, `hello_so`, `hello_sprx`, `relocatable_linking`, `hello_meson/cmake`, `http2_get` | C++ with SDK libc++, shared objects, build systems, network | not run | multi-file / build-system samples need their own link flags |
| AnyPS5 `core/libs/tests/*.cpp` | host-side unit tests of the Windows libraries, not PS5 ELFs | not applicable | AnyPS5 ships no PS5 test ELFs |

## Open issues for other owners

1. **`readdir()` returns nothing** (`file_io.c`). `opendir("/data/wintomac_fileio")` succeeds, the directory holds two
   files, the first `readdir()` returns NULL. `mkdir`, `fopen`, `open`, `stat`, `rename`, `unlink`, `rmdir` all work. Probably
   the AnyPS5 directory-descriptor path (`libkernel/File/src/DirectoryDescriptor.cpp`) on top of wintomac's
   `FindFirstFile`/`getdirentries` emulation. Not diagnosed further.
2. **Fixed here, for the record**: `std::filesystem::canonical(current_path())` failed with ENOENT, so every file operation
   threw an uncaught C++ exception (`unhandled exception 0x20474343`): the program's Windows-side current directory is
   `C:\wintomac` and `rt_host_path` only mapped `C:\wintomac\<file>`. `runtime/files.c` now maps the bare directory too.
3. **Suite dispatcher oddity.** In the 11-program suite ELF, running `hello` (first entry of the name table) printed
   `usage` or crashed with SIGBUS at `c.exe+0x9efb4`, while every other entry works and the same `hello.c` passes as
   a standalone program. The `.data.rel.ro` relocations of the table look right in the ELF. Not understood; possibly a
   lifter/loader issue with the first table entry, possibly the 8 M-instruction library set. `hello` therefore stays
   standalone in `tests.txt`.
4. **Raw `syscall` wrappers** in SDK libc (`mmap`, `mprotect`, `kenv`, ...) are rejected by the AnyPS5 relinker. Homebrew
   that links the SDK's `libc.a` needs the `sce*` equivalents (or AnyPS5 relinker support); titles are unaffected.
6. **`dlopen` of a PS5 system module fails.** `hello_dlfcn` gets NULL from `dlopen("libSceRandom.sprx", RTLD_LAZY)` and
   `dlerror()` is NULL too. Probably the library is not part of the recompiled set (`--with libSceRandom.prx` in
   `ps5_to_mac.sh`; the suite builder does not offer it) or AnyPS5's dlopen needs a module list. Not diagnosed.
5. **Cost.** Every program recompiles the AnyPS5 libraries (3 to 8 M x86 instructions, 20 to 40 minutes under load).
   A per-library cache of the lifted/compiled libraries (they do not depend on the program) would make every PS5 run
   and test minutes cheaper.
