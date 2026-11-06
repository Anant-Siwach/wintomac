#!/usr/bin/env python3
"""Windows API coverage of the wintomac runtime against real PE binaries.

    python3 -I tools/api_coverage.py [--md docs/api_coverage.md] [--top N] PATH...

Each PATH is a PE file or a folder (searched recursively for *.exe / *.dll). Every imported (dll, function) of
every x86-64 image is listed and marked as
  * shim      - a runtime shim with that name exists (imports resolve by name across all shim tables),
  * recompiled- the DLL itself is one of the scanned PE files and exports the name (it gets recompiled with the
                game, e.g. SDL2.dll or DXVK's d3d11.dll), or
  * missing   - neither: calling it would hit "[rt] called import with no shim".
Shim names are read from the runtime sources (shim tables in runtime/*.c and runtime/gen/*), never from objects.

Input files are treated as untrusted data: they are only parsed, never executed. Run with `python3 -I`.
"""
import argparse
import collections
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Microsoft redistributables are DLLs users have locally; they are counted as imports, never as providers.
MS_REDIST = re.compile(r"^(msvcp\d+.*|vcruntime\d+.*|concrt\d+|ucrtbase.*|api-ms-win-.*|vcomp\d+.*|d3dcompiler_\d+|xinput.*|xaudio.*)\.dll$")


# ---------------------------------------------------------------------------------------------- PE parsing
class PEError(Exception):
    pass


def parse_pe(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise PEError("not MZ")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise PEError("no PE signature")
    machine, nsec, _, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    if machine != 0x8664:
        raise PEError(f"machine {machine:#x} (not x86-64)")
    opt = pe_off + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise PEError("not PE32+")
    ndirs = struct.unpack_from("<I", data, opt + 108)[0]
    dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
    secs = []
    so = opt + opt_size
    for i in range(nsec):
        vsize, rva, raw_size, raw_off = struct.unpack_from("<IIII", data, so + 40 * i + 8)
        secs.append((rva, max(vsize, raw_size), raw_off, raw_size))

    def off(rva):
        for s_rva, s_size, s_raw, s_rsize in secs:
            if s_rva <= rva < s_rva + s_size:
                d = rva - s_rva
                if d >= s_rsize:
                    raise PEError("rva in bss")
                return s_raw + d
        raise PEError(f"bad rva {rva:#x}")

    def u32(rva): return struct.unpack_from("<I", data, off(rva))[0]
    def u64(rva): return struct.unpack_from("<Q", data, off(rva))[0]

    def cstr(rva):
        o = off(rva)
        e = data.index(b"\0", o)
        return data[o:e].decode("latin-1")

    def d(i): return dirs[i] if i < len(dirs) else (0, 0)

    imports = []          # (dll, name or None, ordinal or None, delay)

    def thunks(dll, table, delay):
        k = 0
        while k < 65536:
            ent = u64(table + 8 * k)
            if not ent:
                break
            if ent >> 63:
                imports.append((dll, None, ent & 0xFFFF, delay))
            else:
                imports.append((dll, cstr((ent & 0x7FFFFFFF) + 2), None, delay))
            k += 1

    rva, _ = d(1)
    while rva:
        ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", data, off(rva))
        if not name_rva:
            break
        thunks(cstr(name_rva), ilt or iat, False)
        rva += 20
    rva, _ = d(13)                 # delay-load imports (ImgDelayDescr, RVA-based on x64)
    while rva:
        attrs, name_rva, _, _, int_rva = struct.unpack_from("<IIIII", data, off(rva))[:5]
        if not name_rva:
            break
        thunks(cstr(name_rva), int_rva, True)
        rva += 32

    exports = set()               # names, plus "#ordinal" for every exported function
    rva, size = d(0)
    if rva:
        obase = struct.unpack_from("<I", data, off(rva + 16))[0]
        nfuncs, nnames, a_funcs, a_names, a_ords = struct.unpack_from("<IIIII", data, off(rva + 20))
        for i in range(nnames):
            exports.add(cstr(u32(a_names + 4 * i)))
        for i in range(min(nfuncs, 65536)):
            if u32(a_funcs + 4 * i):
                exports.add(f"#{obase + i}")
    return {"imports": imports, "exports": exports, "is_dll": bool(chars & 0x2000)}


def find_pes(paths):
    out = []
    for p in paths:
        if os.path.isfile(p):
            out.append(p)
            continue
        for dp, dn, fn in os.walk(p):
            dn.sort()
            for f in sorted(fn):
                if f.lower().endswith((".exe", ".dll")):
                    out.append(os.path.join(dp, f))
    return out


# ---------------------------------------------------------------------------------------------- shim tables
def shim_table_order(src_dir):
    txt = open(os.path.join(src_dir, "runtime.c")).read()
    m = re.search(r"shim_tables\[\]\s*=\s*\{([^}]*)\}", txt)
    return [t.strip() for t in m.group(1).split(",") if t.strip() and t.strip() != "NULL"] if m else []


def parse_shims(src_dir):
    """{table: [(dll or None, name)]} from every `const Shim shims_x[] = { ... };` in the runtime sources."""
    tables = {}
    files = [os.path.join(src_dir, f) for f in sorted(os.listdir(src_dir)) if f.endswith((".c", ".h"))]
    gen = os.path.join(src_dir, "gen")
    if os.path.isdir(gen):
        files += [os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith((".c", ".h"))]
    lit = re.compile(r'\{\s*(NULL|0|"([^"]*)")\s*,\s*"([^"]+)"\s*,')
    for path in files:
        lines = open(path, errors="replace").read().split("\n")
        i = 0
        while i < len(lines):
            m = re.match(r"\s*(?:static\s+)?const\s+Shim\s+(\w+)\s*\[\s*\]\s*=\s*\{", lines[i])
            if not m:
                i += 1
                continue
            name, ents, macros = m.group(1), [], {}
            i += 1
            while i < len(lines) and not re.match(r"\s*\};", lines[i]):
                ln = lines[i]
                md = re.match(r"\s*#\s*define\s+(\w+)\s*\(\s*(\w+)\s*\)\s*(.*)", ln)
                if md:      # e.g. #define E(n) { NULL, "__wintomac_" #n, sh_##n }
                    mac, arg, body = md.groups()
                    pm = re.search(r'\{\s*(NULL|0|"([^"]*)")\s*,\s*(?:"([^"]*)"\s*)?#\s*' + arg + r"\b", body)
                    if pm:
                        macros[mac] = (pm.group(2), pm.group(3) or "")
                elif not re.match(r"\s*#", ln):
                    for lm in lit.finditer(ln):
                        ents.append((lm.group(2), lm.group(3)))
                    for mac, (dll, pre) in macros.items():
                        for mm in re.finditer(r"(?<![\w#])" + mac + r"\(\s*(\w+)\s*\)", ln):
                            ents.append((dll, pre + mm.group(1)))
                i += 1
            tables[name] = ents
    return tables


def ordinal_names(src_dir):
    """(dll prefix, ordinal) -> name, mirroring runtime.c's ordinal_name()."""
    o = {("xinput", 100): "XInputGetStateEx", ("xinput", 103): "XInputPowerOffController"}
    ws = open(os.path.join(src_dir, "winsock.c")).read()
    for n, nm in re.findall(r'case (\d+): return "(\w+)"', ws):
        o[("ws2_32", int(n))] = o[("wsock32", int(n))] = nm
    ex = open(os.path.join(src_dir, "win_extra.c")).read()
    m = re.search(r"ord_oleaut32\[\]\s*=\s*\{([^}]*)\}", ex)
    if m:
        for n, nm in re.findall(r'\[(\d+)\]\s*=\s*"(\w+)"', m.group(1)):
            o[("oleaut32", int(n))] = nm
    for line in ex.split("\n"):   # if ((!strncasecmp(dll, "ws2_32", 6) || ...) && ord == 51) return "gethostbyaddr";
        m2 = re.search(r'ord == (\d+)\) return "(\w+)"', line)
        if m2:
            for dll in re.findall(r'strncasecmp\(dll, "([\w.]+)"', line):
                o[(dll.lower(), int(m2.group(1)))] = m2.group(2)
    return o


# ---------------------------------------------------------------------------------------------- report
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--runtime", default=os.path.join(ROOT, "runtime"))
    ap.add_argument("--md", help="write a markdown report here")
    ap.add_argument("--top", type=int, default=40, help="missing APIs listed per DLL")
    ap.add_argument("--list", action="store_true", help="print every missing (dll, name) with its importers")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--with-redist", action="store_true",
                    help="count scanned Microsoft redistributables (msvcp140, vcruntime140, ...) as recompiled providers")
    a = ap.parse_args()

    tables = parse_shims(a.runtime)
    order = shim_table_order(a.runtime)
    shim_any, shim_dll, owner, dups = set(), set(), {}, []
    for t in order + [t for t in tables if t not in order]:
        for dll, nm in tables.get(t, []):
            if dll:
                shim_dll.add((dll.lower(), nm))
                continue
            if nm in owner and owner[nm] != t:
                dups.append((nm, owner[nm], t))
            owner.setdefault(nm, t)
            shim_any.add(nm)
    ords = ordinal_names(a.runtime)

    pes, skipped = {}, []
    for p in find_pes(a.paths):
        try:
            pes[p] = parse_pe(p)
        except (PEError, struct.error, ValueError, IndexError, OSError) as e:
            skipped.append((p, str(e)))
    providers = collections.defaultdict(set)     # dll name -> exports of scanned (non-MS) DLLs
    for p, info in pes.items():
        b = os.path.basename(p).lower()
        if info["is_dll"] and (a.with_redist or not MS_REDIST.match(b)):
            providers[b] |= info["exports"]

    def status(dll, name, ordinal):
        dl = dll.lower()
        if name is None and f"#{ordinal}" in providers.get(dl, ()):
            return "recompiled", f"#{ordinal}"
        if name is None:
            name = next((v for (pre, n), v in ords.items() if n == ordinal and dl.startswith(pre)), None)
            if name is None:
                return "missing", f"#{ordinal}"
        if dl in providers and name in providers[dl]:
            return "recompiled", name
        if name in shim_any or (dl, name) in shim_dll:
            return "shim", name
        return "missing", name

    per_dll = collections.defaultdict(lambda: {"shim": set(), "recompiled": set(), "missing": set()})
    importers = collections.defaultdict(set)     # (dll, name) -> binaries
    bin_missing = {}
    for p, info in pes.items():
        miss = 0
        for dll, name, ordinal, delay in info["imports"]:
            st, nm = status(dll, name, ordinal)
            key = dll.lower()
            per_dll[key][st].add(nm)
            if st == "missing":
                importers[(key, nm)].add(p)
                miss += 1
        bin_missing[p] = (miss, len(info["imports"]))

    def pct(x, n): return 100.0 * x / n if n else 100.0
    rows = []
    T = {"shim": 0, "recompiled": 0, "missing": 0}
    for dll, s in per_dll.items():
        n = sum(len(v) for v in s.values())
        for k in T:
            T[k] += len(s[k])
        rows.append((dll, len(s["shim"]), len(s["recompiled"]), len(s["missing"]), n))
    rows.sort(key=lambda r: (-r[3], r[0]))
    total = sum(T.values())
    def project(p):   # third_party/corpus/<name>/..., third_party/games/<name>/..., build/... -> one project each
        r = os.path.relpath(os.path.abspath(p), ROOT).split(os.sep)
        if len(r) > 2 and r[0] == "third_party":
            return r[2] if r[1] in ("corpus", "games") else r[1]
        return r[0] if len(r) > 1 else os.path.dirname(os.path.abspath(p))
    nproj = lambda bins: len({project(b) for b in bins})
    ranked = sorted(importers.items(), key=lambda kv: (-nproj(kv[1]), -len(kv[1]), kv[0]))

    out = []
    w = out.append
    w("# Windows API coverage\n")
    w("Generated by `python3 -I tools/api_coverage.py " + " ".join(os.path.relpath(x, ROOT) if os.path.isabs(x) else x for x in a.paths) + "`.\n")
    w(f"* binaries scanned: **{len(pes)}** x86-64 PE files ({len(skipped)} skipped: not x86-64 PE32+)")
    w(f"* shim names in the runtime: **{len(shim_any)}** (+{len(shim_dll)} DLL-restricted) in {len(tables)} tables")
    w(f"* distinct imported (dll, function) pairs: **{total}** - shimmed **{T['shim']}**, provided by a recompiled DLL "
      f"**{T['recompiled']}**, missing **{T['missing']}**")
    w(f"* coverage (shim + recompiled): **{pct(T['shim'] + T['recompiled'], total):.1f}%**")
    full = sum(1 for p in pes if bin_missing[p][0] == 0)
    w(f"* binaries with every import covered: **{full} / {len(pes)}**\n")
    if dups:
        w("Shim names defined in more than one table (the first table in `shim_tables[]` wins):\n")
        for nm, t1, t2 in dups:
            w(f"* `{nm}`: `{t1}` shadows `{t2}`")
        w("")
    w("## Per-DLL coverage\n")
    w("| DLL | functions | shim | recompiled | missing | coverage |")
    w("|---|---:|---:|---:|---:|---:|")
    for dll, sh, rc, mi, n in rows:
        w(f"| {dll} | {n} | {sh} | {rc} | {mi} | {pct(sh + rc, n):.0f}% |")
    w("\n## Top missing APIs (ranked by projects, then binaries importing them)\n")
    w("| # | API | DLL | projects | binaries |")
    w("|---:|---|---|---:|---:|")
    for i, ((dll, nm), bins) in enumerate(ranked[:a.top * 3]):
        w(f"| {i + 1} | `{nm}` | {dll} | {nproj(bins)} | {len(bins)} |")
    w("\n## Missing APIs by DLL\n")
    by = collections.defaultdict(list)
    for (dll, nm), bins in ranked:
        by[dll].append((nm, len(bins)))
    for dll in sorted(by, key=lambda d: -len(by[d])):
        lst = by[dll]
        more = f" (+{len(lst) - a.top} more)" if len(lst) > a.top else ""
        w(f"* **{dll}** ({len(lst)}): " + ", ".join(f"`{nm}`" + (f"x{n}" if n > 1 else "") for nm, n in lst[:a.top]) + more)
    w("\n## Binaries\n")
    w("| binary | imports | missing |")
    w("|---|---:|---:|")
    for p in sorted(pes, key=lambda p: (-bin_missing[p][0], p)):
        rel = os.path.relpath(p, ROOT) if os.path.abspath(p).startswith(ROOT) else p
        w(f"| {rel} | {bin_missing[p][1]} | {bin_missing[p][0]} |")
    text = "\n".join(out) + "\n"
    if a.md:
        os.makedirs(os.path.dirname(os.path.abspath(a.md)), exist_ok=True)
        open(a.md, "w").write(text)
    if a.list:
        for (dll, nm), bins in ranked:
            print(f"{nproj(bins):3d} {len(bins):4d} {dll}!{nm}")
    elif not a.quiet:
        print(text if not a.md else text.split("## Missing APIs by DLL")[0])


if __name__ == "__main__":
    main()
