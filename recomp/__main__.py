"""wintomac recompiler CLI.

    python -m recomp game.exe [some.dll ...] -o <outdir> [--seed 0x1400012a0 ...] [--funcs-per-file N]

The first image is the program; any DLLs listed after it are recompiled into the same output, rebased
into a dedicated address range so no two images (or macOS's own mappings) collide. The result, linked
with runtime/, is one native arm64 macOS binary. Original images are read at runtime, not embedded.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import re
import time
import zlib

from . import pe as pe_mod
from .analysis import Program
from . import lifter as lifter_mod
from .lifter import lift_function
from . import lifter as _lifter_mod, regsum as _regsum
from . import progress as _progress

EXE_REGION = 0x1_4000_0000   # where low-based programs move to (the usual x64 exe base)
DLL_REGION = 0x100_0000_0000
DLL_STABLE = 0x110_0000_0000   # stable per-name DLL slots: 4096 x 1 GiB = 4 TiB above the program
DLL_SLOT = 0x4000_0000
DLL_SLOTS = 4096  # 1 TiB: probed free on macOS arm64 (0x10_0000_0000-0x70_0000_0000 is reserved by the system)
ALIGN = 0x100000


def _image_header(chunk_file: str) -> str:
    """recomp_<image>_0003.c -> recomp_funcs_<image>.h"""
    return "recomp_funcs_" + re.sub(r"_\d{4}\.c$", "", chunk_file)[len("recomp_"):] + ".h"


def _write_if_changed(path: str, text: str):
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


def _lift_parallel(progs, chunks, files, out, jobs, bar=None):
    """Lift in `jobs` forked workers. Lifting is two-phase per program (lifter.py): every function is lifted and
    reports compact register facts, the parent computes each program's interprocedural summaries, and the
    workers then finalize and write their own files. Lifter objects never leave their worker; only the facts
    (plain tuples) and the summaries (int dicts) cross processes."""
    import multiprocessing as mp
    ctx = mp.get_context("fork")
    pindex = {id(p): i for i, p in enumerate(progs)}
    groups = [[] for _ in range(jobs)]           # contiguous runs of chunks, balanced by instruction count
    total = sum(len(fn.insns) for ch in chunks for fn, _ in ch)
    acc, w = 0, 0
    for k, ch in enumerate(chunks):
        groups[w].append(k)
        acc += sum(len(fn.insns) for fn, _ in ch)
        if acc >= total * (w + 1) / jobs and w < jobs - 1:
            w += 1
    groups = [g for g in groups if g]

    done_insns = ctx.Value("q", 0)                 # progress: instructions lifted (phase 1) + finalized (phase 2)

    def tick(n):                                   # once per chunk, so the lock costs nothing
        with done_insns.get_lock():
            done_insns.value += n

    def worker(conn, ks):
        lifters, facts = {}, {}
        for k in ks:
            n = 0
            for fn, prog in chunks[k]:
                fl = _lifter_mod._new_lifter(prog, fn)
                ff = fl.facts(); fl.pack()
                lifters[fn.addr] = fl
                facts.setdefault(pindex[id(prog)], {})[fn.addr] = ff
                n += len(fn.insns)
            tick(n)
        conn.send(facts)
        summaries = conn.recv()                    # program index -> (NEED, MOD, MUST)
        uns_all = {}
        for k in ks:
            with open(os.path.join(out, files[k]), "w") as f:
                f.write(f'#include "{_image_header(files[k])}"\n\n')
                for fn, prog in chunks[k]:
                    NEED, MOD, MUST = summaries[pindex[id(prog)]]
                    fl = lifters.pop(fn.addr)
                    lines = fl.finalize(NEED, MOD, MUST, facts[pindex[id(prog)]][fn.addr])
                    for kk, v in fl.unsupported.items():
                        uns_all[kk] = uns_all.get(kk, 0) + v
                    f.write("\n".join(lines))
            tick(sum(len(fn.insns) for fn, _ in chunks[k]))
        conn.send(uns_all)
        conn.close()

    # Workers share the analysis copy-on-write. The cyclic GC writes to the header of every object it scans, so a
    # collection in a worker would copy every page of the (gigabytes of) inherited analysis: freeze it first.
    import gc
    gc.collect()
    gc.freeze()
    import threading
    stop = threading.Event()
    if bar is not None and bar.on:
        def draw():
            while not stop.wait(0.15):
                bar.set(done_insns.value // 2)
        threading.Thread(target=draw, daemon=True).start()
    procs, pipes = [], []
    for ks in groups:
        parent, child = ctx.Pipe()
        pr = ctx.Process(target=worker, args=(child, ks)); pr.start()
        procs.append(pr); pipes.append(parent)
    allfacts = {}
    for c in pipes:
        for pi, d in c.recv().items():
            allfacts.setdefault(pi, {}).update(d)
    # Same visiting order as the serial path (each program's function order): summarize() runs a bounded
    # number of rounds, so its (sound) result depends on order. Identical order = identical output.
    summaries = {}
    for pi, d in allfacts.items():
        ordered = {a: d[a] for a in progs[pi].functions if a in d}
        summaries[pi] = _regsum.summarize(ordered)
    for c in pipes:
        c.send(summaries)
    unsupported = {}
    for c in pipes:
        for k, v in c.recv().items():
            unsupported[k] = unsupported.get(k, 0) + v
    stop.set()
    for pr in procs:
        pr.join()
        if pr.exitcode != 0:
            sys.exit(f"[recomp] a lifting worker failed (exit {pr.exitcode}); retry with --jobs 1 for a traceback")
    return unsupported


def main(argv=None):
    ap = argparse.ArgumentParser(prog="recomp")
    ap.add_argument("images", nargs="+", help="the .exe, then any .dll files to recompile with it")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--seed", action="append", default=[], help="extra function entry (VA), repeatable")
    ap.add_argument("--seeds-file", help="file of extra VAs, one per line (e.g. runtime miss log)")
    ap.add_argument("--funcs-per-file", type=int, default=500)
    ap.add_argument("--insns-per-file", type=int, default=20000,
                    help="cut generated files by size so clang's per-file times stay even (parallel build)")
    ap.add_argument("--sequential-dlls", action="store_true",
                    help="pack DLLs one after another instead of at stable per-name addresses")
    ap.add_argument("--keep-exe-base", action="store_true",
                    help="map the program at its preferred base instead of moving it next to the DLLs (1 TiB+)")
    ap.add_argument("-j", "--jobs", type=int, default=0, help="lifting worker processes (default: all cores)")
    ap.add_argument("--no-tso", action="store_true",
                    help="plain loads/stores instead of x86 (TSO) memory ordering; faster, only safe single-threaded")
    ap.add_argument("--no-callopt", action="store_true",
                    help="sync every register around every call (disables the interprocedural summaries of recomp/regsum.py)")
    a = ap.parse_args(argv)
    if a.no_callopt:
        lifter_mod.CALLOPT = False

    t0 = time.time()
    pes = [pe_mod.parse(p) for p in a.images]
    # address plan: program keeps its preferred base; DLLs are packed into DLL_REGION
    # macOS on arm64 reserves the low 4 GB of every process (__PAGEZERO), so a program linked below that
    # moves up; without a .reloc section its relocations are synthesized first (see PE.synthesize_relocs).
    exe = pes[0]
    # A program with relocations also moves up, next to the DLLs: the low 4 GB after __PAGEZERO is where
    # dyld puts the host's own libraries (MoltenVK, vkd3d, SDL, ...) at randomised addresses, so a fixed
    # 0x140000000 sometimes collides ("could not map ... at 140000000"). Nothing else maps at 1 TiB+.
    nxt = DLL_REGION
    if exe.image_base + exe.size_of_image <= 0x1_0000_0000:
        if not exe.relocs:
            n = exe.synthesize_relocs()
            print(f"[recomp] {os.path.basename(a.images[0])}: no relocations; synthesized {n} pointer fixups")
        exe.rebase(nxt)
        nxt = (nxt + exe.size_of_image + ALIGN - 1) // ALIGN * ALIGN
    elif exe.relocs and not a.keep_exe_base:
        exe.rebase(nxt)
        nxt = (nxt + exe.size_of_image + ALIGN - 1) // ALIGN * ALIGN
    # DLLs get a base derived from their name (1 GiB slots above the program), not "next free": a library's
    # generated code then doesn't depend on which other libraries are in the build, so it is byte-identical
    # across programs and its objects come from ccache after the first build (AnyPS5's PS5 libraries are
    # 3-8 M instructions shared by every title). --sequential-dlls packs them as before.
    used = []
    def free(b, size):
        return all(b + size <= u0 or b >= u1 for u0, u1 in used)
    used.append((exe.image_base, exe.image_base + exe.size_of_image))
    for p, path in zip(pes[1:], a.images[1:]):
        if a.sequential_dlls or p.size_of_image > DLL_SLOT:
            while not free(nxt, p.size_of_image):
                nxt += DLL_SLOT
            base = nxt
            nxt = (nxt + p.size_of_image + ALIGN - 1) // ALIGN * ALIGN
        else:
            slot = zlib.crc32(os.path.basename(path).lower().encode()) % DLL_SLOTS
            base = DLL_STABLE + slot * DLL_SLOT
            while not free(base, p.size_of_image):      # rare collision: next slot (deterministic per set)
                slot = (slot + 1) % DLL_SLOTS
                base = DLL_STABLE + slot * DLL_SLOT
        p.rebase(base)
        used.append((base, base + p.size_of_image))
    spans = sorted((p.image_base, p.image_base + p.size_of_image, n) for p, n in zip(pes, a.images))
    for (b0, e0, n0), (b1, e1, n1) in zip(spans, spans[1:]):
        if b1 < e0:
            sys.exit(f"[recomp] images overlap: {n0} and {n1}")

    seeds = [int(s, 0) for s in a.seed]
    labels = []
    if a.seeds_file and os.path.exists(a.seeds_file):
        for l in open(a.seeds_file):
            f = l.split()
            if not f or f[0].startswith("#"):
                continue
            if f[0] == "label":        # resume target the runtime couldn't enter (written by the runtime)
                labels.append(int(f[1], 0))
            else:
                seeds.append(int(f[0], 0))
    progs = []
    abar = _progress.Bar("analysis", sum(p.size_of_image for p in pes))
    for p, path in zip(pes, a.images):
        abar.set(abar.n, note=os.path.basename(path), force=True)
        inside = lambda v: p.image_base <= v < p.image_base + p.size_of_image
        progs.append(Program(p, [s for s in seeds if inside(s)], [l for l in labels if inside(l)]).analyze())
        abar.update(p.size_of_image)
    abar.done(f"{len(pes)} image(s)")
    os.makedirs(a.out, exist_ok=True)

    all_funcs = sorted(((fn, prog) for prog in progs for fn in prog.functions.values()), key=lambda x: x[0].addr)
    unsupported: dict[str, int] = {}
    # Files are cut by instruction count, not function count: clang's time per file grows with its size and a
    # single huge file becomes the build's critical path (a 27 MB chunk took 58 s while the median took 7 s).
    # Files never mix images and are named after their image, so an unchanged library yields identical files
    # (same names, same contents) in every build that includes it: ccache hits across programs.
    img_slug = {id(pr): re.sub(r"[^a-z0-9]+", "_", os.path.splitext(os.path.basename(path))[0].lower()).strip("_") or "img"
                for pr, path in zip(progs, a.images)}
    chunks, names, cur, cur_n, cur_prog, per_img = [], [], [], 0, None, {}
    for item in all_funcs:
        n = len(item[0].insns)
        if cur and (item[1] is not cur_prog or cur_n + n > a.insns_per_file or len(cur) >= a.funcs_per_file):
            k = per_img.get(id(cur_prog), 0); per_img[id(cur_prog)] = k + 1
            chunks.append(cur); names.append(f"recomp_{img_slug[id(cur_prog)]}_{k:04d}.c"); cur, cur_n = [], 0
        cur.append(item); cur_n += n; cur_prog = item[1]
    if cur:
        k = per_img.get(id(cur_prog), 0)
        chunks.append(cur); names.append(f"recomp_{img_slug[id(cur_prog)]}_{k:04d}.c")
    files = names
    ninsn = sum(len(fn.insns) for fn, _ in all_funcs)
    jobs = max(1, min(a.jobs or int(os.environ.get("WINTOMAC_JOBS") or 0) or os.cpu_count() or 1, len(chunks)))
    lbar = _progress.Bar("translate", ninsn)
    if jobs == 1:
        for name, chunk in zip(files, chunks):
            with open(os.path.join(a.out, name), "w") as f:
                f.write(f'#include "{_image_header(name)}"\n\n')
                for fn, prog in chunk:
                    lines, uns = lift_function(prog, fn)
                    for k, v in uns.items():
                        unsupported[k] = unsupported.get(k, 0) + v
                    f.write("\n".join(lines))
                    lbar.update(len(fn.insns))
    else:
        unsupported = _lift_parallel(progs, chunks, files, a.out, jobs, lbar)
    lbar.done(f"{ninsn:,} instructions -> {len(files)} C files")
    # Remove stale chunk files from a previous, larger build
    for old in os.listdir(a.out):
        if old.startswith("recomp_") and old not in ("recomp_table.c", "recomp_table.o", "recomp_table.d") \
                and old.endswith((".c", ".o", ".d")) and old.rsplit(".", 1)[0] + ".c" not in files:
            os.remove(os.path.join(a.out, old))

    # One declarations header per image (a chunk only calls its own image's functions; other images are reached
    # through import thunks), plus recomp_funcs.h including them all for the dispatch table. Written only when
    # the text changes, so unchanged images keep their timestamps and stay cached.
    per_img_decls: dict[str, list[str]] = {}
    for name, chunk in zip(files, chunks):
        per_img_decls.setdefault(_image_header(name), []).extend(f"void f_{fn.addr:x}(Ctx *c);" for fn, _ in chunk)
    prologue = f"#pragma once\n#define RT_TSO {0 if a.no_tso else 1}\n#include \"recomp.h\"\n\n"
    for hdr, decls in per_img_decls.items():
        _write_if_changed(os.path.join(a.out, hdr), prologue + "\n".join(decls) + "\n")
    _write_if_changed(os.path.join(a.out, "recomp_funcs.h"),
                      prologue + "".join(f'#include "{h}"\n' for h in sorted(per_img_decls)))
    for old in os.listdir(a.out):   # headers of images no longer in the build
        if old.startswith("recomp_funcs_") and old.endswith(".h") and old not in per_img_decls:
            os.remove(os.path.join(a.out, old))

    def cstr(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

    with open(os.path.join(a.out, "recomp_table.c"), "w") as f:
        f.write('#include "recomp_funcs.h"\n\n')
        for k, (p, path) in enumerate(zip(pes, a.images)):
            f.write(f"static const RtImport imports_{k}[] = {{\n")
            for imp in p.imports:
                f.write(f"  {{0x{p.image_base + imp.iat_rva:x}ull, {cstr(imp.dll)}, {cstr(imp.name) if imp.name else '0'}, {imp.ordinal or 0}}},\n")
            f.write("  {0, 0, 0, 0}\n};\n")
            sr = ", ".join(hex(r) for r in p.relocs) if p.synth_relocs else "0"
            f.write(f"static const u32 synth_relocs_{k}[] = {{ {sr} }};\n")
            tls = ", ".join(hex(p.image_base + r) for r in p.tls_callbacks) or "0"
            f.write(f"static const u64 tls_{k}[] = {{ {tls} }};\n")
        f.write("const RtImage rt_images[] = {\n")
        for k, (p, path) in enumerate(zip(pes, a.images)):
            f.write(f"  {{ {cstr(os.path.basename(path))}, {cstr(os.path.abspath(path))}, 0x{p.image_base:x}ull, 0x{p.orig_base:x}ull, "
                    f"0x{p.size_of_image:x}ull, 0x{p.image_base + p.entry_rva if p.entry_rva else 0:x}ull, {int(p.is_dll)}, "
                    f"0x{p.timestamp:x}u, imports_{k}, {len(p.imports)}, tls_{k}, {len(p.tls_callbacks)}, "
                    + (f"0x{p.image_base + p.tls[0]:x}ull, 0x{p.image_base + p.tls[1]:x}ull, 0x{p.image_base + p.tls[2]:x}ull, {p.tls[3]}"
                       if p.tls and p.tls[2] else "0, 0, 0, 0")
                    + f", synth_relocs_{k}, {len(p.relocs) if p.synth_relocs else 0}" + " },\n")
        f.write("};\n")
        f.write(f"const u64 rt_nimages = {len(pes)};\n")
        main_pe = pes[0]
        f.write(f"const u64 rt_image_base = 0x{main_pe.image_base:x}ull;\n")
        f.write(f"const u64 rt_image_size = 0x{main_pe.size_of_image:x}ull;\n")
        f.write(f"const u32 rt_subsystem = {main_pe.subsystem};\n")
        f.write("const RtFunc rt_funcs[] = {\n")
        for fn, _ in all_funcs:
            f.write(f"  {{0x{fn.addr:x}ull, f_{fn.addr:x}}},\n")
        f.write("};\n")
        f.write(f"const u64 rt_nfuncs = {len(all_funcs)};\n")

    report = {
        "images": [{"path": os.path.abspath(path), "base": hex(p.image_base), "preferred_base": hex(p.orig_base),
                    "functions": len(prog.functions), "imports": [f"{i.dll}!{i.name or i.ordinal}" for i in p.imports]}
                   for p, path, prog in zip(pes, a.images, progs)],
        "functions": len(all_funcs),
        "instructions": ninsn,
        "jump_tables": sum(len(fn.jump_tables) for fn, _ in all_funcs),
        "eh_functions": sum(fn.eh for fn, _ in all_funcs),
        "resume_labels": sum(len(fn.entries) for fn, _ in all_funcs),
        "eh_handler_kinds": [prog.eh_kinds for prog in progs],
        "unsupported_mnemonics": dict(sorted(unsupported.items(), key=lambda kv: -kv[1])),
        "decode_problems": {hex(fn.addr): fn.bad for fn, _ in all_funcs if fn.bad},
        "seconds": round(time.time() - t0, 2),
        "sources": files,
    }
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
    for p, path, prog in zip(pes, a.images, progs):
        moved = f" (rebased from {p.orig_base:#x})" if p.orig_base != p.image_base else ""
        print(f"[recomp] {os.path.basename(path)}: {len(prog.functions)} functions at {p.image_base:#x}{moved}, {len(p.imports)} imports")
    print(f"[recomp] total {len(all_funcs)} functions, {ninsn} instructions, {report['jump_tables']} jump tables, "
          f"{report['eh_functions']} EH frames, {report['resume_labels']} resume labels in {report['seconds']}s")
    if unsupported:
        top = ", ".join(f"{k}x{v}" for k, v in list(report["unsupported_mnemonics"].items())[:15])
        print(f"[recomp] unsupported (falls back to trap): {top}", file=sys.stderr)


if __name__ == "__main__":
    main()
