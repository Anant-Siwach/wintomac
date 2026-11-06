# Performance

How fast recompiled code runs, compared with Rosetta 2 running the same program built for x86_64 macOS
(the competitor) and with a native arm64 build (the ceiling); what it costs and why; and what was done about it.

## Benchmarks (`tools/bench.sh`, `tests/bench/`)

Seven CPU-bound programs shaped like game workloads. Each prints `BENCH <kernel> <ms> <checksum>` per
kernel; the harness builds each three ways **from the same source with the same compiler** (clang -O2,
`-ffp-contract=off`, baseline x86-64 / SSE2) and checks that every build computes the same checksums:

| build | how | runs as |
|---|---|---|
| wintomac | `clang --target=x86_64-w64-windows-gnu` + mingw-w64 link (C++: libstdc++, static) → `python -m recomp` → `tools/link.sh` | native arm64 Mach-O |
| Rosetta 2 | `clang -arch x86_64` (C++: libc++) | x86_64 Mach-O, translated by Rosetta 2 |
| native | `clang -arch arm64 -mcpu=apple-m1` | the ceiling |

| program | kernels | what it stresses |
|---|---|---|
| `int_hash.c` | FNV-1a, Murmur3, open-addressing hash table, quicksort, sieve | integer ALU, small direct calls, compare+branch, byte stores |
| `bytecode_vm.c` | switch-dispatched VM, function-pointer-dispatched VM, recursive tree evaluator | jump tables, indirect calls, recursion |
| `float_simd.c` | 4x4 matrix product, vertex transform, particle physics, n-body (double), SSE-intrinsics normalisation | SSE packed/scalar float, shuffles |
| `mem.c` | large/small memcpy and memset, struct copies, a strcpy loop | CRT imports, load→store streams |
| `cpp_virt.cpp` | virtual update/score calls over 20k entities, `std::sort`, `std::unordered_map<std::string,int>`, `std::function` | C++ indirect calls, EH-frame functions, STL |
| `threads.c` | contended `lock add`, spinlock, SPSC ring buffer, 4-thread job slices | atomics, x86 memory ordering (TSO) |
| `compress.c` | miniz 3.0.2 deflate / inflate / crc32+adler32 (vendored in `tests/bench/miniz/`, MIT) | a real codec |

```sh
tools/bench.sh                               # all benchmarks, 5 runs each, markdown table of medians
tools/bench.sh -r 9 int_hash cpp_virt        # some benchmarks, 9 runs
tools/bench.sh -v ours,notso,rosetta,native  # add a --no-tso build (no x86 memory-ordering emulation)
tools/bench.sh -o results.md                 # also write the table to a file
RECOMP_FLAGS=--no-callopt BENCH_TAG=nocallopt tools/bench.sh -v ours   # A/B a recompiler flag (own build dir)
BASE_DIR=/path/to/older/wintomac/build/bench tools/bench.sh -v base,ours   # before/after against another checkout
```

Runs are interleaved (run 1 of every build, then run 2, ...) so frequency/thermal drift and other load on
the machine hit all builds alike; the table shows medians. The machine these numbers come from (Apple M5,
MacBook Air, passively cooled) was shared with other build jobs, so single numbers move by ±20%; ratios
measured in the same interleaved run are more stable than absolute times.

## Results

Apple M5 (MacBook Air), macOS 26.7, clang 21 (Apple), 5 interleaved runs, medians in ms. **before** is
wintomac as it was before this work (the recompiler and runtime frozen at that point, same benchmark
sources and sizes), **wintomac** is the current tree, **--no-tso** the current tree without x86
memory-ordering emulation (not safe for multithreaded programs: the two racy thread kernels compute
different results without it, which is the point of TSO emulation).

| bench | kernel | before ms | wintomac ms | wintomac --no-tso ms | Rosetta 2 ms | native arm64 ms | before / Rosetta | wintomac / Rosetta | wintomac --no-tso / Rosetta | before / native | wintomac / native | wintomac --no-tso / native |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bytecode_vm | vm_switch | 310.7 | 314.6 | 251.2 | 337.1 | 199.2 | 0.92x | 0.93x | 0.75x | 1.56x | 1.58x | 1.26x |
| bytecode_vm | vm_fnptr | 944.9 | 484.1 | 475.9 | 254.6 | 194.0 | 3.71x | 1.90x | 1.87x | 4.87x | 2.50x | 2.45x |
| bytecode_vm | tree_eval | 470.9 | 305.4 | 301.3 | 198.6 | 145.8 | 2.37x | 1.54x | 1.52x | 3.23x | 2.10x | 2.07x |
| compress | deflate | 1375.9 | 1335.9 | 924.2 | 1238.5 | 898.9 | 1.11x | 1.08x | 0.75x | 1.53x | 1.49x | 1.03x |
| compress | inflate | 327.2 | 344.2 | 282.5 | 328.4 | 268.7 | 1.00x | 1.05x | 0.86x | 1.22x | 1.28x | 1.05x |
| compress | crc32_adler | 470.7 | 469.7 | 412.2 | 461.2 | 430.5 | 1.02x | 1.02x | 0.89x | 1.09x | 1.09x | 0.96x |
| cpp_virt | virtual_calls | 671.0 | 222.3 | 209.1 | 182.8 | 134.8 | 3.67x | 1.22x | 1.14x | 4.98x | 1.65x | 1.55x |
| cpp_virt | stl_sort | 386.2 | 272.2 | 271.0 | 158.4 | 106.4 | 2.44x | 1.72x | 1.71x | 3.63x | 2.56x | 2.55x |
| cpp_virt | stl_unordered_map | 244.4 | 225.7 | 232.3 | 102.3 | 76.0 | 2.39x | 2.21x | 2.27x | 3.22x | 2.97x | 3.06x |
| cpp_virt | std_function | 238.2 | 200.2 | 195.5 | 21.3 | 11.3 | 11.20x | 9.42x | 9.20x | 21.02x | 17.67x | 17.25x |
| float_simd | mat4_mul | 170.0 | 120.3 | 120.2 | 139.5 | 95.0 | 1.22x | 0.86x | 0.86x | 1.79x | 1.27x | 1.27x |
| float_simd | vec_transform | 170.0 | 80.7 | 85.2 | 89.2 | 61.4 | 1.91x | 0.91x | 0.95x | 2.77x | 1.31x | 1.39x |
| float_simd | particles | 192.3 | 91.4 | 91.7 | 78.9 | 72.1 | 2.44x | 1.16x | 1.16x | 2.67x | 1.27x | 1.27x |
| float_simd | nbody | 66.8 | 61.7 | 60.6 | 78.3 | 62.0 | 0.85x | 0.79x | 0.77x | 1.08x | 1.00x | 0.98x |
| float_simd | sse_normalize | 75.6 | 73.3 | 78.5 | 70.0 | 74.5 | 1.08x | 1.05x | 1.12x | 1.02x | 0.98x | 1.05x |
| int_hash | hash_fnv1a | 224.7 | 226.6 | 222.0 | 225.1 | 221.8 | 1.00x | 1.01x | 0.99x | 1.01x | 1.02x | 1.00x |
| int_hash | hash_murmur3 | 182.9 | 191.0 | 183.0 | 236.6 | 179.2 | 0.77x | 0.81x | 0.77x | 1.02x | 1.07x | 1.02x |
| int_hash | hashtable | 288.7 | 164.6 | 168.1 | 182.6 | 149.9 | 1.58x | 0.90x | 0.92x | 1.93x | 1.10x | 1.12x |
| int_hash | quicksort | 514.5 | 440.4 | 428.0 | 391.9 | 366.9 | 1.31x | 1.12x | 1.09x | 1.40x | 1.20x | 1.17x |
| int_hash | sieve | 452.3 | 460.4 | 182.2 | 418.0 | 152.2 | 1.08x | 1.10x | 0.44x | 2.97x | 3.03x | 1.20x |
| mem | memcpy_large | 62.5 | 63.8 | 61.9 | 127.8 | 60.3 | 0.49x | 0.50x | 0.48x | 1.04x | 1.06x | 1.03x |
| mem | memset_large | 75.7 | 73.7 | 72.9 | 180.4 | 79.7 | 0.42x | 0.41x | 0.40x | 0.95x | 0.92x | 0.91x |
| mem | memcpy_small | 175.2 | 132.6 | 120.3 | 94.1 | 47.7 | 1.86x | 1.41x | 1.28x | 3.67x | 2.78x | 2.52x |
| mem | memset_small | 152.9 | 114.9 | 105.4 | 67.2 | 37.8 | 2.28x | 1.71x | 1.57x | 4.05x | 3.04x | 2.79x |
| mem | struct_copy | 217.8 | 234.1 | 192.1 | 195.4 | 196.7 | 1.11x | 1.20x | 0.98x | 1.11x | 1.19x | 0.98x |
| mem | strcpy_loop | 199.6 | 198.1 | 157.7 | 160.6 | 152.2 | 1.24x | 1.23x | 0.98x | 1.31x | 1.30x | 1.04x |
| threads | atomic_fetch_add | 1103.3 | 155.8 | 134.4 | 138.4 | 75.3 | 7.97x | 1.13x | 0.97x | 14.66x | 2.07x | 1.79x |
| threads | spinlock | 74.9 | 87.7 | 48.4 | 52.9 | 187.6 | 1.42x | 1.66x | 0.92x | 0.40x | 0.47x | 0.26x |
| threads | spsc_ring | 228.4 | 284.6 | 18.0 | 34.1 | 622.9 | 6.70x | 8.35x | 0.53x | 0.37x | 0.46x | 0.03x |
| threads | job_slices | 71.1 | 64.5 | 82.8 | 20.6 | 14.2 | 3.45x | 3.13x | 4.02x | 5.02x | 4.56x | 5.85x |
| **geomean** | |  |  |  |  |  | **1.68x** | **1.31x** | **1.08x** | **2.03x** | **1.58x** | **1.30x** |

--no-tso computed different results (expected for racy code without x86 ordering): threads/spinlock, threads/spsc_ring

Geometric mean over the 30 kernels: **1.68x → 1.31x the Rosetta 2 time** and **2.03x → 1.58x the native
time** (an earlier full run on a quieter machine, before the last fix: 1.65x → 1.28x vs Rosetta,
1.90x → 1.47x vs native). The recompiled code is now faster than Rosetta on 8 of the 30 kernels and within 5% on 4 more.

Reading the table:
* Plain integer and float loops (`hash_*`, `sse_normalize`, `nbody`, `crc32_adler`, the large `memcpy`/`memset`
  which are shims onto the host's libc) run at native speed; Rosetta is often slower there.
* Calls are the main remaining gap: `std_function` (a three-instruction callee behind an indirect call) is
  ~9x Rosetta, `tree_eval`/`stl_sort`/`vm_fnptr` 1.5-1.9x. Before this work every call cost a full register
  sync; now direct calls sync what the callee needs and indirect calls the volatile registers.
* x86 memory ordering costs up to 2-3x on store-heavy loops (`sieve`, `deflate`, `struct_copy`,
  `strcpy_loop`: compare the `--no-tso` column), and it is why `spsc_ring` is 8x Rosetta (Rosetta has
  hardware TSO; native arm64 with acquire/release C atomics is slower still).
* `job_slices` is SSE2 integer code (`pmuludq` multiply idiom) that stays 3x behind.

A/B of the call-site sync alone (`--no-callopt`, i.e. the old full sync, against the default; both with all
other changes):

| bench | kernel | --no-callopt ms | default ms | speedup |
|---|---|---|---|---|
| bytecode_vm | vm_switch | 325.1 | 329.7 | 0.99x |
| bytecode_vm | vm_fnptr | 649.2 | 481.5 | 1.35x |
| bytecode_vm | tree_eval | 536.4 | 343.2 | 1.56x |
| cpp_virt | virtual_calls | 321.9 | 264.4 | 1.22x |
| cpp_virt | stl_sort | 347.3 | 341.1 | 1.02x |
| cpp_virt | stl_unordered_map | 280.2 | 266.9 | 1.05x |
| cpp_virt | std_function | 227.8 | 246.6 | 0.92x |
| int_hash | hash_fnv1a | 260.9 | 285.0 | 0.92x |
| int_hash | hash_murmur3 | 246.6 | 239.5 | 1.03x |
| int_hash | hashtable | 279.8 | 188.6 | 1.48x |
| int_hash | quicksort | 570.2 | 518.0 | 1.10x |
| int_hash | sieve | 523.7 | 746.1 | 0.70x |
| mem | memcpy_large | 189.1 | 170.5 | 1.11x |
| mem | memset_large | 178.8 | 127.2 | 1.41x |
| mem | memcpy_small | 211.0 | 184.9 | 1.14x |
| mem | memset_small | 157.3 | 157.5 | 1.00x |
| mem | struct_copy | 323.5 | 296.1 | 1.09x |
| mem | strcpy_loop | 227.6 | 208.6 | 1.09x |
| float_simd | mat4_mul | 215.8 | 222.2 | 0.97x |
| float_simd | vec_transform | 110.3 | 98.4 | 1.12x |
| float_simd | particles | 197.8 | 184.5 | 1.07x |
| float_simd | nbody | 90.8 | 102.3 | 0.89x |
| float_simd | sse_normalize | 138.7 | 120.2 | 1.15x |
| compress | deflate | 1361.5 | 1355.2 | 1.00x |
| compress | inflate | 305.5 | 333.0 | 0.92x |
| compress | crc32_adler | 464.7 | 502.2 | 0.93x |
| **geomean** | | | | **1.07x** |

This A/B ran while other jobs kept the machine's load average at 30-60, so kernels without calls in their
hot loop (`sieve`, `memset_large`, `nbody`, ...) moved ±30% from noise alone; the call-heavy kernels moved
consistently across two runs: `tree_eval` 1.56x, `hashtable` 1.4-1.5x, `vm_fnptr` 1.3x, `virtual_calls`
1.2x, `memcpy_small` 1.15-1.25x (import calls), `quicksort` 1.03-1.1x.

## Where the time went (profiling)

`tools/profile.py -- ./build/bench/cpp_virt.ours.bin build/bench/cpp_virt.exe` records the program with
Instruments' Time Profiler (`xctrace`, headless), and prints self/inclusive time per function, annotating
recompiled `f_<addr>` functions with the guest symbol from the PE (mingw builds keep a symbol table).
Inlined helpers show up under their own names (`LD64`, `rt_icall`, ...) because xctrace symbolicates
inlined frames from the DWARF info. `sample` doesn't work on recompiled binaries (empty call graphs).

What the profiles and the generated code showed, in order of cost:

1. **Register sync around calls.** Every call stored every guest register the caller used into `Ctx` and
   loaded them all back (in `main` of `int_hash`: 14 GPRs + 11 xmm = 50 memory operations, around a 25-
   instruction `tbl_get`). Functions with an EH frame (any C++ function with a destructor or catch) kept that
   full sync for every call.
2. **Indirect calls** went through `rt_call`: a range check, a flag check, then a binary search over all
   functions (`rt_funcs[]`) on every call.
3. **Packed SSE was lifted lane by lane**: `mulps` became four scalar `fmul`s, `shufps`/`unpcklps` became
   element moves. `transform()` had 64 scalar `fmul` where the x86 code had 16 `mulps`.
4. **Locked read-modify-writes** (`lock add/inc/...`, very common: reference counts, statistics, job
   counters) were compare-and-swap loops; under contention they were 5x slower than Rosetta.
5. **Compare + branch**: signed conditions (`jl`, `jge`, ...) were evaluated from flag bits rebuilt with
   `eor/bic/orr` instead of one `cmp`, because clang can't see that `sf != of` of a subtraction is `a < b`.
6. **x86 memory ordering (TSO)**: every non-stack load is `LDAPR` and every store `STLR`. Loads cost about
   the same as plain ones on Apple cores; release stores after acquire loads (a load→store stream: copies,
   read-modify-write of arrays) cost up to 2x. See the `--no-tso` column. (Microbenchmark on the M5: a
   load→store copy loop with LDAPR+STLR takes 1.6-2.3x the time of LDAPR+STR; replacing STLR with
   `DMB ISHST` + STR, the other correct mapping, is 3-60x slower still, so STLR stays.)
7. Import calls (`memcpy` from the CRT is a shim in `runtime/`) wrote two thread-locals per call (each a
   `tlv_get_addr` call on macOS).

## What was changed

| change | where | effect |
|---|---|---|
| Interprocedural register summaries: per function NEED (registers it reads from Ctx), MOD (registers whose Ctx value it may change), MUST (definitely rewritten); a direct call stores NEED ∪ (MOD \ MUST) and reloads MOD; functions only load live-in registers at entry and write back only what they changed at `ret`. | `recomp/regsum.py` (new), `recomp/lifter.py` | direct-call kernels: `tree_eval` 1.56x, `hashtable` 1.4-1.5x, `quicksort` ~1.05x (A/B above); `hashtable` 289 → 165 ms overall |
| Win64 ABI: callee-saved registers saved in the prologue and restored in every epilogue are not in MOD and their saves are not reads the caller must provide; calls the analysis can't see sync only volatile registers; EH-frame functions use the same scheme, storing the callee-saved registers they change before each call. | same | C++: `virtual_calls` 671 → 222 ms (3.0x, with the inline cache), `stl_sort` 386 → 272 ms; `main` of `cpp_virt` was in full-sync mode before (EH frame) |
| Indirect calls: per-call-site inline cache (`rt_icall`), plus a direct-mapped cache in front of the binary search for everything else. | `runtime/recomp.h`, `runtime/runtime.c`, lifter | `vm_fnptr` 945 → 484 ms (2.0x, with the volatile-only sync) |
| SSE lane kernels as whole-vector operations on GCC vector types (one NEON instruction each): add/sub/mul/div/min/max/sqrt ps/pd, logic, integer add/sub/compare/min/max/saturating, shuffles/unpacks/blends, packed compares, shifts, `pmuludq`, `cvtdq2ps`. | `recomp/vector.py`, `runtime/recomp.h` | `vec_transform` 170 → 81 ms (2.1x), `particles` 192 → 91 ms (2.1x), `mat4_mul` 170 → 120 ms (1.4x); `transform()` went from 64 scalar `fmul` to 16 `fmul.4s` |
| `lock add/sub/and/or/xor/inc/dec/not` on memory → one LSE atomic (`ldaddal`, `ldclral`, `ldsetal`, `ldeoral`). | lifter | `atomic_fetch_add` (4 threads, `lock add`) 1103 → 156 ms (7x, now Rosetta's speed) |
| Compare/test fused with the following `jcc`/`setcc`/`cmovcc`: the condition is written on the compared values (`(i32)a < (i32)b`), the flag locals die. | lifter | signed loop conditions become one `cmp` + `b.cond`; part of `quicksort` 515 → 440 ms |
| Zeroing idioms (`xorps x,x`, `pxor`, `psub` with equal operands) lifted as a plain zero, so clang can still split a register used by scalar code into lanes (found as an `nbody` regression of the vector change: 43 → 63 ms, now 62 ms native-equal). | `recomp/vector.py` | fixes a regression |
| Import calls: thread-locals only written when they change; return-address pushes of indirect calls are stack stores (no release). | `runtime/runtime.c`, lifter | small |

Correctness: every change keeps the full headless suite passing, plus four new tests:
`callopt_regs` (IPA-RA-style register reuse across calls, a hand-written x86 function with a private
convention that only works with exact MOD sets, xmm6-15 pressure, callbacks from the runtime, and the
MSVC pattern that broke vkQuake's SDL3.dll at first: `rbx` saved to the home slot and restored on both
exits of a function, one of them a tail call to a function allowed to change `rbx`; the epilogue's restore
had been counted as a "definitely written" (MUST) register although `ret` skips writing it back),
`callopt_exceptions` (C++ throw/rethrow through frames with values in callee-saved registers, cleanup landing
pads, nested try), `seh_callopt` (`__try/__except/__finally` with `RaiseException` and an integer divide by
zero through optimised frames) and `callopt_threads` (8 threads racing on the same indirect call sites).
`python -m recomp --no-callopt` restores the old full sync (for A/B and as an escape hatch);
`recomp/regsum.py` `ABI_SAVES = False` and `recomp/lifter.py` `EH_CONSERVATIVE = True` turn off the ABI-based
parts and the EH-frame part separately.

## What is still slow, and next steps

Measured with the same harness; in rough order of what they cost the benchmarks:

1. **Indirect calls** (virtual calls, `std::function`, function-pointer dispatch) still cost a sync of the
   volatile registers in use plus the inline-cache check: 1.2x (virtual calls) to 3-5x (`std::function`,
   whose callee is three instructions) behind Rosetta. Next: pass the argument registers to recompiled
   functions as C arguments and return `rax`/`xmm0` as the C result, so a call through the inline cache
   needs no `Ctx` traffic at all for leaf callees; speculative devirtualisation of hot sites.
2. **x86 memory ordering**: every heap store is `STLR`. After an acquire load (a load→store stream) that
   costs up to 2x (`sieve`, `deflate`, `vm_switch`, `strcpy_loop`; compare the `--no-tso` column). Next:
   prove more accesses thread-private (frame-pointer frames, `lea r, [rsp+x]` pointers that provably don't
   escape the function); `--no-tso` per game when it is known single-threaded.
3. **Call sync is now proportional to what callees use, but not zero**: every recompiled function still
   checks `c->resume_target` on entry and loads its live-in registers from `Ctx`; a recursive call with two
   arguments is ~3 stores + 4 loads (`tree_eval` 2-3x native). Next: the same register-argument convention
   as (1), keeping `Ctx` only for unknown callers (`rt_call`, the interpreter, exceptions).
4. **SSE integer idioms**: SSE2 has no 32-bit `pmulld`, so compilers emit `pmuludq` + `pshufd` + `punpckldq`
   sequences; lifted one by one they cost 3-4x a native `mul.4s` (`job_slices`). Next: recognise the idiom,
   and make `V128` a plain vector type (the union makes clang move 128-bit values through GPR pairs).
5. **Import calls** (`memcpy`, `memset`, CRT `printf` family): ~2x Rosetta for small calls: guest `call` →
   import stub → `rt_jump` → `rt_call` → thunk → shim. Next: call the shim directly when the IAT slot is
   known at recompile time; the runtime's `printf` (`win_format` in `runtime/ucrt.c`) dominates the
   `stl_unordered_map` kernel.
6. **Recompile time**: the two-phase lift + summaries take ~2.2x longer (vkQuake: 21 s instead of 10 s) and
   ~2x the memory (570 MB); the lifted C is kept zlib-compressed between the phases.
7. **Interpreter** (`runtime/interp*.c`, ~69 M x86 instructions/s): not changed. Pre-decoded basic blocks
   (decode once per block, dispatch through a per-block array of handlers) remain the planned next step; it
   only matters for code the recompiler didn't see (JIT output, missed functions).


## Game timedemos (run these yourself: they open a window)

QuakeSpasm and vkQuake ship with the shareware `demo1`. `tools/timedemo.sh` runs `+timedemo demo1` in a
1280x720 window with vsync off and sound on SDL's dummy driver, stops the game when the result line appears,
saves the full output to `build/timedemo/<label>.<n>.log` and appends `<label> <frames> <seconds> <fps>` to
`build/timedemo/results.txt`. `tools/timedemo_compare.py` prints the median fps per label.

```sh
cd wintomac   # the repository root
# 1. the games with the current recompiler (same images, flags and seeds as the existing builds)
tools/recomp_variant.sh build/vkquake.gen vkquake-new
tools/recomp_variant.sh build/quakespasm.gen quakespasm-new
# 2. A/B: the old call-site sync (everything else identical)
tools/recomp_variant.sh build/vkquake.gen vkquake-nocallopt --no-callopt
tools/recomp_variant.sh build/quakespasm.gen quakespasm-nocallopt --no-callopt
# 3. timedemo, 3 runs each (each opens a window; output only goes to build/timedemo/*.log)
tools/timedemo.sh build/vkquake-new.bin       third_party/games/vkquake/dist/vkQuake.exe              vkq-new 3
tools/timedemo.sh build/vkquake-nocallopt.bin third_party/games/vkquake/dist/vkQuake.exe              vkq-nocallopt 3
tools/timedemo.sh build/quakespasm-new.bin       third_party/games/quakespasm/dist/quakespasm-reloc.exe qs-new 3
tools/timedemo.sh build/quakespasm-nocallopt.bin third_party/games/quakespasm/dist/quakespasm-reloc.exe qs-nocallopt 3
# 4. compare
tools/timedemo_compare.py build/timedemo/results.txt vkq-nocallopt
tools/timedemo_compare.py build/timedemo/results.txt qs-nocallopt
```

`tools/recomp_variant.sh` takes the exe + DLL list from `build/<game>.gen/report.json` and the game's
`wintomac_misses.txt` as seeds, so the variants differ only in the flags given. `--no-tso` can be added the same way to measure what x86 memory ordering costs a real game (vkQuake
is multithreaded: only safe if it survives the demo, and not a supported configuration).
