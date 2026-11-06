// Runtime fallback: an x86-64 interpreter for guest code the static recompiler didn't translate
// (missed indirect targets, runtime-generated code from JITs/DRM, self-patching code).
// It executes directly on the guest Ctx, so recompiled code, shims and interpreted code interoperate freely.
#pragma once
#include "recomp.h"

// Interpret guest code starting at pc, in the state the generated code leaves at a call boundary
// ([rsp] = return address, or mid-function at a resume label). Returns (host-side) when the guest executes
// the `ret` that pops past the entry stack pointer, exactly like a recompiled function returns.
void rt_interp_run(Ctx *c, u64 pc);
// Same, with the arithmetic flags (RFLAGS layout) passed in and out: for entries where flags may be live
// (mid-block fallbacks) and for differential testing (tests/interp_fuzz).
void rt_interp_run_ex(Ctx *c, u64 pc, u64 *rflags);

// Executable-memory awareness: VirtualAlloc/VirtualProtect with PAGE_EXECUTE* register their ranges.
// Code there is always interpreted (never looked up in the static tables) and decoded afresh, so
// runtime-generated and self-modifying code just works.
void rt_interp_mark_exec(u64 addr, u64 size);
void rt_interp_unmark_exec(u64 addr, u64 size);
int  rt_interp_is_exec(u64 addr);
extern int rt_interp_exec_in_images;   // != 0: an exec range overlaps a recompiled image (checked before lookup)

// Implemented in runtime.c: run `target` natively if it is an import thunk or a recompiled function entry
// (return address already pushed, as for rt_call). Returns 0 (doing nothing) if it is neither.
int rt_try_native(Ctx *c, u64 target);

// Record an address the static tables don't cover (once per address) in wintomac_misses.txt, the
// `--seeds-file` input of the recompiler. label != 0: a mid-function resume target. Runtime-generated code
// (inside an executable region) is not recorded: it can't be recompiled ahead of time.
void rt_note_miss(u64 target, int label);
int rt_interp_disabled(void);
// rt_unimplemented / rt_fell_off hand the rest of a recompiled function to the interpreter; noted once per ip
// (WINTOMAC_TRACE / WINTOMAC_INTERP_TRACE print it).
void rt_note_fallback(u64 ip, const char *what);   // WINTOMAC_NO_INTERP=1: die on misses like before the interpreter existed
