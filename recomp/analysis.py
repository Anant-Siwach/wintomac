"""Control-flow recovery: function discovery, basic blocks, jump tables."""
from __future__ import annotations

import bisect
import struct
from array import array
from dataclasses import dataclass, field

import iced_x86 as X

from .pe import PE

FC = X.FlowControl
NORETURN_IMPORTS = {
    "ExitProcess", "ExitThread", "TerminateProcess", "FatalExit",   # RaiseException can return (continuable exceptions, SetThreadName)
    "exit", "_exit", "abort", "_cexit", "_amsg_exit", "__fastfail", "longjmp",
    "_invalid_parameter_noinfo_noreturn", "__report_gsfailure", "_CxxThrowException",
    "RtlUnwindEx", "FatalAppExitA", "FatalAppExitW",
}


@dataclass
class JumpTable:
    targets: list[int]          # absolute guest addresses, in table order


class CompactInsns:
    """A finished function's instructions, kept as a sorted array of addresses and decoded again on demand.

    A decoded iced Instruction costs ~200 bytes with its dict slot; Godot's 33M instructions held that way took
    over 10 GB, swapping a 16 GB machine to a standstill. Analysis keeps a plain dict only while it builds a
    function; the lifter materialises one function at a time (Function.materialize)."""
    __slots__ = ("ips", "_dec")

    def __init__(self, d, dec):
        self.ips = array("Q", sorted(d))
        self._dec = dec

    def __len__(self):
        return len(self.ips)

    def __contains__(self, ip):
        ips = self.ips
        i = bisect.bisect_left(ips, ip)
        return i < len(ips) and ips[i] == ip

    def __iter__(self):
        return iter(self.ips)

    def __getitem__(self, ip):
        if ip not in self:
            raise KeyError(ip)
        return self._dec(ip)

    def get(self, ip, default=None):
        return self._dec(ip) if ip in self else default

    def keys(self):
        return iter(self.ips)

    def items(self):
        dec = self._dec
        return ((ip, dec(ip)) for ip in self.ips)

    def values(self):
        dec = self._dec
        return (dec(ip) for ip in self.ips)

    def materialize(self) -> dict:
        dec = self._dec
        return {ip: dec(ip) for ip in self.ips}


@dataclass
class Function:
    addr: int
    insns: dict[int, X.Instruction] = field(default_factory=dict)  # ip -> insn
    leaders: set[int] = field(default_factory=set)                 # block start addresses
    tail_calls: set[int] = field(default_factory=set)              # jmp targets that leave the function
    jump_tables: dict[int, JumpTable] = field(default_factory=dict)  # jmp-insn ip -> table
    calls: set[int] = field(default_factory=set)
    bad: list[str] = field(default_factory=list)
    eh: bool = False                                   # has a language handler: needs a resumable host frame
    entries: set[int] = field(default_factory=set)     # extra entry points (secondary entries, EH resume labels)
    addr_refs: set[int] = field(default_factory=set)   # code/data addresses taken (`lea r,[rip+x]`, `mov r, offset x`)

    def compact(self, dec):
        if isinstance(self.insns, dict):
            self.insns = CompactInsns(self.insns, dec)

    def materialize(self):
        """Decoded instructions back as a dict (lifting); compact() again when done."""
        if isinstance(self.insns, CompactInsns):
            dec = self.insns._dec
            self.insns = self.insns.materialize()
            return dec
        return None

    def blocks(self):
        """Yield (start, [insn, ...]) in address order."""
        cur, start = [], None
        ips = sorted(self.insns)
        # Overlapping instructions (jumps into the middle of another instruction) mean a fallthrough
        # target may not be the next address in sorted order; such targets must start their own block.
        for a, b in zip(ips, ips[1:] + [None]):
            nxt = self.insns[a].next_ip
            if nxt != b and nxt in self.insns:
                self.leaders.add(nxt)
        for ip in ips:
            if ip in self.leaders or (cur and cur[-1].next_ip != ip):
                if cur:
                    yield start, cur
                cur, start = [], ip
            cur.append(self.insns[ip])
            fc = self.insns[ip].flow_control
            if fc in (FC.UNCONDITIONAL_BRANCH, FC.INDIRECT_BRANCH, FC.RETURN, FC.CONDITIONAL_BRANCH,
                      FC.INTERRUPT, FC.EXCEPTION):
                yield start, cur
                cur, start = [], None
        if cur:
            yield start, cur


class _AddrBitmap:
    """Set of addresses inside one image, one bit per byte (Godot: 22 MB instead of a 33M-entry set)."""

    def __init__(self, base: int, size: int):
        self.base, self.size = base, size
        self.bits = bytearray((size + 7) // 8)

    def update(self, addrs):
        bits, base = self.bits, self.base
        for a in addrs:
            o = a - base
            bits[o >> 3] |= 1 << (o & 7)

    def __contains__(self, a):
        o = a - self.base
        return 0 <= o < self.size and bool(self.bits[o >> 3] >> (o & 7) & 1)


class Program:
    def __init__(self, pe: PE, extra_seeds: list[int] | None = None, extra_labels: list[int] | None = None):
        self.pe = pe
        self._reuse = None      # see _decode_at
        self._last_sec = None
        # (va_lo, va_hi_of_file_data, file_offset) for sections with file data: direct slicing in _decode_at
        self._raw_ranges = [(pe.image_base + sc.rva, pe.image_base + sc.rva + min(sc.raw_size, sc.vsize or sc.raw_size), sc.raw_off)
                            for sc in pe.sections if sc.raw_size]
        self.base = pe.image_base
        self.functions: dict[int, Function] = {}
        self.iat: dict[int, tuple[str, str]] = {}  # absolute IAT slot addr -> (dll, name)
        for imp in pe.imports:
            self.iat[self.base + imp.iat_rva] = (imp.dll, imp.name or f"#{imp.ordinal}")
        # .pdata ranges: authoritative function bounds when present
        self.pdata = sorted((self.base + b, self.base + e) for b, e, _ in pe.runtime_functions)
        self._pdata_begins = [b for b, _ in self.pdata]
        # Chained .pdata chunks (UNW_FLAG_CHAININFO) are pieces of one function (clang/MSVC split the unwind info,
        # e.g. for shrink-wrapping). A function = its root chunk + every chunk chained to it.
        self._chunk_root: dict[int, int] = {}
        for b, _ in self.pdata:
            r, seen = b, 0
            while seen < 32:
                ui = pe.unwind.get(r - self.base)
                if not ui or not (ui.flags & 4) or not ui.chained_begin:
                    break
                r = self.base + ui.chained_begin; seen += 1
            self._chunk_root[b] = r
        self._pdata_starts = {b for b in self._pdata_begins if self._chunk_root[b] == b}   # real function starts
        self.seeds: set[int] = set()
        self._seed(pe.entry_rva + self.base)
        for rva in pe.exports:
            self._seed(self.base + rva)
        for rva in pe.tls_callbacks:
            self._seed(self.base + rva)
        for b in self._pdata_starts:
            self._seed(b)
        # Absolute pointers into code that the loader relocates: vtables, callback tables, etc.
        for rva in pe.relocs:
            try:
                v = pe.u64(rva)
            except ValueError:
                continue
            self._seed(v)
        for s in extra_seeds or []:
            self._seed(s)
        # Exception handling: which functions have handlers, and which addresses unwinding can resume at.
        self.eh_funcs: set[int] = set()
        self.labels: dict[int, set[int]] = {}   # function start -> extra labels (landing pads, __except targets, ...)
        self.eh_kinds: dict[str, int] = {}
        for b_rva, ui in pe.unwind.items():
            if ui.flags & 3 and ui.handler_rva:
                fva = self.base + b_rva
                self.eh_funcs.add(fva)
                self._seed(self.base + ui.handler_rva)   # the dispatcher calls it: must be recompiled even if nothing else does
                kind, pads = self._eh_labels(fva, self.base + ui.handler_rva, self.base + ui.handler_data_rva)
                self.eh_kinds[kind] = self.eh_kinds.get(kind, 0) + 1
                for lp in pads:
                    self._add_label(lp)
        for l in extra_labels or []:  # resume targets the runtime couldn't find (wintomac_misses.txt "label" lines)
            self._add_label(l)
        # Itanium C++ ABI code (DWARF CFI in .eh_frame, LSDAs in .gcc_except_table): AnyPS5's libraries (".ehfram")
        # and relinked PS5 executables (".ehmeta" -> the ELF's eh_frame_hdr). The unwinder lives in guest code and
        # resumes by restoring registers and jumping to a landing pad; those pads become labels of EH functions.
        self.dwarf_sites: list[tuple[int, int, int]] = []   # (call-site start, end, landing pad), sorted
        self.dwarf_pads: set[int] = set()
        self._dwarf_eh()

    # ------------------------------------------------------------------
    def _owner(self, va: int) -> int | None:
        """Start of the function (root chunk) containing va: the recompiled function that will own the label."""
        r = self._pdata_range(va)
        return self._chunk_root.get(r[0], r[0]) if r else None

    def _add_label(self, va: int) -> bool:
        o = self._owner(va)
        if o is None or not self._is_code(va):
            return False
        st = self.labels.setdefault(o, set())
        if va in st:
            return False
        st.add(va)
        return True

    def _handler_import(self, handler_va: int) -> str | None:
        ins = self._decode_at(handler_va)
        if ins and ins.flow_control == FC.INDIRECT_BRANCH and ins.is_ip_rel_memory_operand:
            imp = self.iat.get(ins.ip_rel_memory_address)
            return imp[1] if imp else None
        return None

    def _eh_labels(self, func_va: int, handler_va: int, data_va: int):
        """(kind, resume addresses) for one handler: 'cspec' scope tables, 'gcc' LSDA landing pads, else 'other'."""
        name = self._handler_import(handler_va)
        if name in ("__C_specific_handler", "_C_specific_handler"):
            return "cspec", self._scope_table_targets(data_va)
        if name and "CxxFrameHandler" in name:
            return "msvc-cxx", []        # continuations come from catch funclets at runtime: all leaders resumable
        pads = self._gcc_landing_pads(func_va, data_va)
        if pads is not None:
            return "gcc", pads
        return "other", []

    def _scope_table_targets(self, data_va: int):
        try:
            n = self.pe.u32(data_va - self.base)
            if n > 4096:
                return []
            out = []
            for i in range(n):
                b, e, h, j = (self.pe.u32(data_va - self.base + 4 + 16 * i + 4 * k) for k in range(4))
                if j > 1:  # __except block (0 = __finally, whose handler is a separate funclet)
                    out.append(self.base + j)
                if h > 1:  # filter / __finally funclet, called by __C_specific_handler
                    self._seed(self.base + h)
            return out
        except ValueError:
            return []

    # ---- DWARF exception tables ------------------------------------------
    def _dwarf_eh(self):
        pe, base = self.pe, self.base
        frames = []   # (va, bytes) of whole .eh_frame sections, or (None, list of FDE VAs) from an eh_frame_hdr
        for sc in pe.sections:
            if sc.name in (".ehfram", ".eh_fram", ".eh_frame") and sc.raw_size:
                frames.append((base + sc.rva, pe.read(sc.rva, min(sc.vsize or sc.raw_size, sc.raw_size))))
            elif sc.name == ".ehmeta" and sc.raw_size >= 4:
                try:
                    fdes = self._eh_frame_hdr(base + pe.u32(sc.rva))
                except (ValueError, IndexError, struct.error):
                    fdes = None
                if fdes:
                    frames.append((None, fdes))
        if not frames:
            return
        sites = []
        for va, data in frames:
            if va is None:
                fdes = data
            else:
                fdes = self._walk_eh_frame(va, data)
            for fde in fdes:
                try:
                    r = self._parse_fde(fde)
                except (ValueError, IndexError, struct.error):
                    r = None
                if not r or not r[2]:
                    continue
                start, end, lsda = r
                got = self._gcc_lsda_sites(start, lsda)
                if not got:
                    continue
                pads = [lp for _, _, lp in got if self._is_code(lp)]
                if not pads:
                    continue
                # the FDE's start owns its pads (unless they belong to a .cold part reached from another function:
                # _dwarf_owners() assigns those once function bodies are known)
                self.eh_funcs.add(start)
                self.labels.setdefault(start, set()).update(pads)
                sites += [(lo, hi, lp) for lo, hi, lp in got if self._is_code(lp)]
        sites.sort()
        self.dwarf_sites = sites
        self._dwarf_site_starts = [s[0] for s in sites]
        self.dwarf_pads = {s[2] for s in sites}
        self.eh_kinds["dwarf"] = len({s[2] for s in sites})

    def _bytes_at(self, va: int, n: int) -> bytes:
        return self.pe.read(va - self.base, n)

    def _enc(self, va: int, enc: int, data_rel: int = 0):
        """Read a DW_EH_PE-encoded pointer at va: (value, size). Values are VAs of this image."""
        if enc == 0xFF:
            return 0, 0
        fmt, app = enc & 0x0F, enc & 0x70
        if fmt == 0x01 or fmt == 0x09:
            b = self._bytes_at(va, 10)
            v = sh = i = 0
            while True:
                x = b[i]; i += 1
                v |= (x & 0x7F) << sh; sh += 7
                if not x & 0x80:
                    break
            if fmt == 0x09 and x & 0x40:
                v -= 1 << sh
            size = i
        else:
            size = {0x00: 8, 0x02: 2, 0x03: 4, 0x04: 8, 0x0A: 2, 0x0B: 4, 0x0C: 8}.get(fmt)
            if size is None:
                raise ValueError("encoding")
            v = int.from_bytes(self._bytes_at(va, size), "little", signed=fmt >= 0x0A)
        if v and app == 0x10:
            v += va
        elif v and app == 0x30:
            v += data_rel
        elif app not in (0x00,):
            raise ValueError("application")
        if enc & 0x80:
            v = int.from_bytes(self._bytes_at(v, 8), "little")
        return v & 0xFFFFFFFFFFFFFFFF, size

    def _eh_frame_hdr(self, hdr: int):
        """FDE addresses from an ELF .eh_frame_hdr's binary search table (version 1)."""
        b = self._bytes_at(hdr, 4)
        if b[0] != 1 or b[3] == 0xFF:
            return None
        p = hdr + 4
        _, n = self._enc(p, b[1], hdr); p += n
        count, n = self._enc(p, b[2], hdr); p += n
        if count > 2_000_000:
            return None
        out = []
        for _ in range(count):
            _, n = self._enc(p, b[3], hdr); p += n
            f, n = self._enc(p, b[3], hdr); p += n
            out.append(f)
        return out

    def _walk_eh_frame(self, va: int, data: bytes):
        out, p = [], 0
        while p + 8 <= len(data):
            length = struct.unpack_from("<I", data, p)[0]
            if length == 0:
                p += 4
                continue
            if length == 0xFFFFFFFF or p + 4 + length > len(data):
                break
            if struct.unpack_from("<I", data, p + 4)[0]:   # CIE pointer != 0: an FDE
                out.append(va + p)
            p += 4 + length
        return out

    def _cie(self, cie: int):
        cache = self.__dict__.setdefault("_cie_cache", {})
        if cie in cache:
            return cache[cie]
        b = self._bytes_at(cie, 4 + min(self.pe.u32(cie - self.base), 512))
        if struct.unpack_from("<I", b, 4)[0] != 0:
            raise ValueError("not a CIE")
        ver, p = b[8], 9
        aug_end = b.index(0, p)
        aug = b[p:aug_end].decode("latin-1"); p = aug_end + 1

        def uleb():
            nonlocal p
            r = sh = 0
            while True:
                x = b[p]; p += 1
                r |= (x & 0x7F) << sh; sh += 7
                if not x & 0x80:
                    return r
        if ver == 4:
            p += 2
        uleb(); uleb()                       # code / data alignment (data alignment is an SLEB, same length)
        if ver == 1:
            p += 1
        else:
            uleb()
        renc, lenc = 0, 0xFF
        if aug.startswith("z"):
            uleb()
            for ch in aug[1:]:
                if ch == "R": renc = b[p]; p += 1
                elif ch == "L": lenc = b[p]; p += 1
                elif ch == "P":
                    _, n = self._enc(cie + p + 1, b[p] & 0x7F)   # personality pointer: skipped (never dereferenced)
                    p += 1 + n
                elif ch in "SB": pass
                else: raise ValueError("augmentation")
        r = cache[cie] = (aug, renc, lenc)
        return r

    def _parse_fde(self, fde: int):
        """(pc_begin, pc_end, lsda VA or 0) of one FDE."""
        h = self._bytes_at(fde, 8)
        length, cie_off = struct.unpack("<II", h)
        if length in (0, 0xFFFFFFFF) or cie_off == 0:
            return None
        aug, renc, lenc = self._cie(fde + 4 - cie_off)
        p = fde + 8
        start, n = self._enc(p, renc); p += n
        rng, n = self._enc(p, renc & 0x0F); p += n
        lsda = 0
        if aug.startswith("z") and "L" in aug:
            b = self._bytes_at(p, 10)
            i = 0
            while b[i] & 0x80:
                i += 1
            p += i + 1
            lsda, _ = self._enc(p, lenc)
        return start, start + rng, lsda

    def _dwarf_owners(self) -> set[int]:
        """Give every landing pad to the functions whose body holds one of its call sites (a .cold part of a
        function is analysed as part of it). Returns the functions that got new labels."""
        sites, starts = self.dwarf_sites, getattr(self, "_dwarf_site_starts", [])
        if not sites:
            return set()
        lo_all, hi_all = sites[0][0], max(s[1] for s in sites)
        grew = set()
        for fva, fn in self.functions.items():
            have = self.labels.get(fva, set())
            for ip in fn.insns:
                if not (lo_all <= ip < hi_all):
                    continue
                i = bisect.bisect_right(starts, ip) - 1   # call-site ranges don't overlap
                if i >= 0 and ip < sites[i][1] and sites[i][2] not in have:
                    have = self.labels.setdefault(fva, set())
                    have.add(sites[i][2])
                    self.eh_funcs.add(fva)
                    grew.add(fva)
        return grew

    def _gcc_landing_pads(self, func_va: int, data_va: int):
        """Parse a GCC LSDA (handler data of __gxx_personality_seh0) and return its landing pads, or None."""
        sites = self._gcc_lsda_sites(func_va, data_va)
        return None if sites is None else [lp for _, _, lp in sites]

    def _gcc_lsda_sites(self, func_va: int, data_va: int):
        """Parse a GCC LSDA: [(call-site start, end, landing pad)] for entries with a pad, or None."""
        pe, base = self.pe, self.base
        try:
            buf = pe.read(data_va - base, 4096)
        except ValueError:
            try:
                buf = pe.read(data_va - base, 256)
            except ValueError:
                return None
        pos = 0

        def u8():
            nonlocal pos
            v = buf[pos]; pos += 1
            return v

        def uleb():
            nonlocal pos
            r = sh = 0
            while True:
                b = buf[pos]; pos += 1
                r |= (b & 0x7F) << sh; sh += 7
                if not b & 0x80:
                    return r

        def sleb():
            nonlocal pos
            r = sh = 0
            while True:
                b = buf[pos]; pos += 1
                r |= (b & 0x7F) << sh; sh += 7
                if not b & 0x80:
                    if b & 0x40:
                        r -= 1 << sh
                    return r

        def enc(e):
            nonlocal pos
            if e == 0xFF:
                return 0
            fmt = e & 0x0F
            if fmt == 0x01: v = uleb()
            elif fmt == 0x09: v = sleb()
            else:
                size = {0x00: 8, 0x02: 2, 0x03: 4, 0x04: 8, 0x0A: 2, 0x0B: 4, 0x0C: 8}.get(fmt)
                if size is None:
                    raise ValueError("encoding")
                v = int.from_bytes(buf[pos:pos + size], "little", signed=fmt >= 0x0A); pos += size
            return v

        try:
            lp_enc = u8()
            lpstart = func_va if lp_enc == 0xFF else enc(lp_enc)
            tt_enc = u8()
            if tt_enc != 0xFF:
                uleb()
            cs_enc = u8()
            if cs_enc not in (0x01, 0x03, 0x0B, 0x09):
                return None
            cs_len = uleb()
            end = pos + cs_len
            if cs_len > 0x10000 or end > len(buf):
                return None
            sites = []
            while pos < end:
                cs = enc(cs_enc); cl = enc(cs_enc); lp = enc(cs_enc); uleb()
                if lp:
                    sites.append((func_va + cs, func_va + cs + cl, lpstart + lp))
        except (IndexError, ValueError):
            return None
        if any(not (func_va <= p < func_va + 0x400000) or not self._is_code(p) for _, _, p in sites):
            return None
        return sites

    def _is_code(self, va: int) -> bool:
        return self.pe.is_code_rva(va - self.base) if va >= self.base else False

    def _seed(self, va: int):
        if self._is_code(va):
            self.seeds.add(va)

    def _decode_at(self, va: int) -> X.Instruction | None:
        # Re-analysing a function (new secondary entries, re-split) decodes the same bytes again: reuse what the
        # previous analysis of that function decoded. Godot-sized programs re-analyse tens of thousands.
        reuse = self._reuse
        if reuse is not None:
            ins = reuse.get(va)
            if ins is not None:
                return ins
        # Fast path: whole 15-byte window inside one section's file data (almost always); last hit cached.
        sec = self._last_sec
        if sec is None or not (sec[0] <= va and va + 15 <= sec[1]):
            sec = None
            for lo, hi, off in self._raw_ranges:
                if lo <= va and va + 15 <= hi:
                    sec = self._last_sec = (lo, hi, off)
                    break
        if sec is not None:
            o = sec[2] + (va - sec[0])
            buf = self.pe.data[o:o + 15]
        else:
            # Within 15 bytes of the end of a section's file data, or outside it: code only exists in file
            # data, the zero-filled virtual tail of a section is not code (00 00 decodes as `add [rax],al`, and
            # following it walks megabytes of zeros). Decode only if the instruction start has file data.
            if not any(lo <= va < hi for lo, hi, _ in self._raw_ranges):
                return None
            try:
                buf = self.pe.read(va - self.base, 15)
            except ValueError:
                return None
        ins = X.Decoder(64, buf, ip=va).decode()
        return None if ins.code == X.Code.INVALID else ins

    def _pdata_range(self, va: int):
        i = bisect.bisect_right(self._pdata_begins, va) - 1
        if i >= 0 and self.pdata[i][0] <= va < self.pdata[i][1]:
            return self.pdata[i]
        return None

    def _is_noreturn_call(self, ins: X.Instruction) -> bool:
        if ins.is_ip_rel_memory_operand:
            imp = self.iat.get(ins.ip_rel_memory_address)
            return bool(imp and imp[1] in NORETURN_IMPORTS)
        return False

    # ------------------------------------------------------------------
    def _drain(self, work):
        while work:
            va = work.pop()
            if va in self.functions:
                continue
            fn = self._analyze_function(va)
            self.functions[va] = fn
            for t in fn.calls | fn.tail_calls:
                if t not in self.functions and self._is_code(t):
                    self.seeds.add(t)
                    work.append(t)

    def analyze(self):
        self._drain(sorted(self.seeds))
        # DWARF landing pads whose call sites ended up in another function's body (.cold parts): give them to that
        # function, re-analyse it (the pad code is new code with new calls) and analyse what it calls.
        for _ in range(8):
            grew = self._dwarf_owners()
            if not grew:
                break
            work = []
            for o in grew:
                fn = self.functions[o] = self._analyze_function(o)
                work += [t for t in fn.calls | fn.tail_calls if t not in self.functions and self._is_code(t)]
            self.seeds.update(work)
            self._drain(work)
        # Pass 2: a function's internal jump target may have turned out to be another function's
        # entry (gcc tail-call through jmp). Re-split so such jumps become tail calls.
        changed = not self.pdata   # with .pdata, function bounds come from it and this pass never re-splits
        while changed:
            changed = False
            starts = set(self.functions)
            for va, fn in list(self.functions.items()):
                for ip, ins in fn.insns.items():
                    if ins.flow_control in (FC.UNCONDITIONAL_BRANCH, FC.CONDITIONAL_BRANCH):
                        t = ins.near_branch_target
                        if t in starts and t != va and t not in fn.tail_calls and self.pdata == []:
                            self.functions[va] = self._analyze_function(va)
                            changed = True
                            break
        # Code addresses taken in code (`lea rcx,[rip+callback]`): leaf functions have no .pdata and often no data
        # reference, so seed every rip-relative lea target in executable sections that isn't inside a known body
        # (inside a .pdata range only if it is that range's start). Iterate: new functions take new addresses.
        # Linear in program size: each function's address references were collected while decoding it, and the
        # set of known instruction addresses grows incrementally (Godot: 180K functions, ~27M instructions).
        known = _AddrBitmap(self.base, self.pe.size_of_image)   # instruction starts of every known function
        for fn in self.functions.values():
            known.update(fn.insns)
        pending = list(self.functions.values())
        for _ in range(16):
            new_seeds = set()
            for fn in pending:
                for t in fn.addr_refs:
                    if t in self.functions or t in known or not self._is_code(t):
                        continue
                    r = self._pdata_range(t)
                    if r and t not in self._pdata_starts:
                        continue
                    if self._decode_at(t) is None:
                        continue
                    new_seeds.add(t)
            if not new_seeds:
                break
            pending = []
            for t in sorted(new_seeds):
                self.seeds.add(t)
                fn = self._analyze_function(t)
                self.functions[t] = fn
                known.update(fn.insns)
                pending.append(fn)
        # Jumps into the middle of another function (GCC hot/cold partitions jump back into the hot part) become
        # secondary entry points of the function that owns that address; re-analyse owners until stable.
        for _ in range(8):
            grew = set()
            for fn in list(self.functions.values()):
                targets = set(fn.tail_calls)
                for jt in fn.jump_tables.values():
                    targets.update(t for t in jt.targets if t not in fn.insns)
                for t in targets:
                    if t not in self.functions and self._add_label(t):
                        grew.add(self._owner(t))
            grew &= set(self.functions)
            if not grew:
                break
            for o in grew:
                self.functions[o] = self._analyze_function(o)
        return self

    def _analyze_function(self, entry: int) -> Function:
        prev = self.functions.get(entry)
        # reuse what the previous analysis of this function decoded (a compacted one costs a decode anyway)
        self._reuse = prev.insns if prev is not None and isinstance(prev.insns, dict) else None
        try:
            fn = self._analyze_function_body(entry)
        finally:
            self._reuse = None
        fn.compact(self._decode_raw)
        return fn

    def _decode_raw(self, va: int) -> X.Instruction:
        ins = self._decode_at(va)
        if ins is None:
            raise ValueError(f"instruction at {va:#x} no longer decodes")
        return ins

    def _analyze_function_body(self, entry: int) -> Function:
        fn = Function(entry)
        bounds = self._pdata_range(entry)
        work = [entry]
        fn.leaders.add(entry)
        fn.eh = entry in self.eh_funcs
        for l in sorted(self.labels.get(entry, ())):
            fn.leaders.add(l); fn.entries.add(l); work.append(l)

        root = self._owner(entry) if bounds else None
        def internal(t: int) -> bool:
            if t == entry:
                return True
            if bounds:   # anywhere in this function's chunks, except the start of another function
                return self._owner(t) == root and t not in self._pdata_starts
            if t in self.seeds:
                return False
            # A function without .pdata (a leaf, or a seed from a pointer) must not grow into code that .pdata
            # assigns to another function: a jump there is a tail call. Without this, one unbounded seed in a
            # big binary follows jumps and fall-throughs across millions of instructions (Godot editor).
            return not (self.pdata and self._pdata_range(t) is not None)

        cap = 200_000   # safety net: no real function is this large; data decoded as code can be
        zeros = 0
        while work:
            va = work.pop()
            while va not in fn.insns:
                if not bounds and va != entry and self.pdata and self._pdata_range(va) is not None:
                    fn.tail_calls.add(va)   # fell through into another function's .pdata range
                    break
                if len(fn.insns) >= cap:
                    fn.bad.append(f"function exceeds {cap} instructions; truncated @ {va:#x}")
                    break
                ins = self._decode_at(va)
                if ins is None:
                    fn.bad.append(f"undecodable @ {va:#x}")
                    break
                if ins.len == 2 and ins.code == X.Code.ADD_RM8_R8 and ins.op0_kind == X.OpKind.MEMORY \
                        and ins.memory_base == X.Register.RAX and ins.op1_register == X.Register.AL:
                    zeros += 1
                    if zeros >= 8:   # a run of 00 00 bytes: padding or data, not code
                        fn.bad.append(f"zero bytes decoded as code @ {va:#x}")
                        break
                else:
                    zeros = 0
                fn.insns[va] = ins
                if ins.is_ip_rel_memory_operand:
                    if ins.mnemonic == X.Mnemonic.LEA:
                        fn.addr_refs.add(ins.ip_rel_memory_address)
                elif self.pe.abs_delta and ins.mnemonic in (X.Mnemonic.MOV, X.Mnemonic.PUSH) and \
                        ins.op_kind(ins.op_count - 1) in (X.OpKind.IMMEDIATE32, X.OpKind.IMMEDIATE32TO64, X.OpKind.IMMEDIATE64):
                    a = self._abs(ins.immediate(ins.op_count - 1))   # `mov ecx, offset callback` (non-PIC code)
                    if a is not None:
                        fn.addr_refs.add(a)
                fc = ins.flow_control
                if fc == FC.NEXT or fc == FC.XBEGIN_XABORT_XEND:
                    va = ins.next_ip
                    continue
                if fc == FC.CALL:
                    fn.calls.add(ins.near_branch_target)
                    va = ins.next_ip
                    continue
                if fc == FC.INDIRECT_CALL:
                    if self._is_noreturn_call(ins):
                        break
                    va = ins.next_ip
                    continue
                if fc in (FC.UNCONDITIONAL_BRANCH, FC.CONDITIONAL_BRANCH):
                    t = ins.near_branch_target
                    if internal(t):
                        fn.leaders.add(t)
                        work.append(t)
                    else:
                        fn.tail_calls.add(t)
                    if fc == FC.CONDITIONAL_BRANCH:
                        fn.leaders.add(ins.next_ip)
                        va = ins.next_ip
                        continue
                    break
                if fc == FC.INDIRECT_BRANCH:
                    jt = self._jump_table(fn, ins)
                    if jt:
                        fn.jump_tables[ins.ip] = jt
                        for t in jt.targets:
                            if internal(t):
                                fn.leaders.add(t); work.append(t)
                            else:
                                fn.tail_calls.add(t)
                    break
                # RETURN, INTERRUPT (int3), EXCEPTION (ud2)
                break
        return fn

    # ------------------------------------------------------------------
    def _jump_table(self, fn: Function, jmp: X.Instruction) -> JumpTable | None:
        """Recognise the common compiler jump-table idioms by a short backwards walk.

        gcc/clang:  lea rT,[rip+tbl]; movsxd rX,[rT+rI*4]; add rX,rT; jmp rX      (entries relative to tbl)
        MSVC:       lea rB,[__ImageBase]; mov eX,[rB+rI*4+tblrva]; add rX,rB; jmp rX  (entries are RVAs)
        absolute:   jmp [tbl+rI*8]                                                (entries are VAs)
        """
        prev = self._preceding(fn, jmp.ip, 12)
        R = X.Register
        if jmp.op0_kind == X.OpKind.MEMORY:
            if jmp.memory_index_scale == 8 and jmp.memory_base == R.NONE:
                tbl = self._abs(jmp.memory_displacement)
                if tbl is None:
                    return None
                n = self._bound(prev, jmp.memory_index)
                return self._read_table(tbl, n, lambda o: self.pe.u64(o - self.base), 8)
            return None
        reg = jmp.op0_register
        full = X.RegisterExt.full_register
        add = load = None
        for ins in reversed(prev):
            m = ins.mnemonic
            if add is None and m == X.Mnemonic.ADD and ins.op_count == 2 and ins.op0_kind == X.OpKind.REGISTER \
                    and full(ins.op0_register) == full(reg) and ins.op1_kind == X.OpKind.REGISTER:
                add = ins
                continue
            if add is not None and load is None and m in (X.Mnemonic.MOVSXD, X.Mnemonic.MOV) and \
                    ins.op0_kind == X.OpKind.REGISTER and full(ins.op0_register) == full(reg) and \
                    ins.op1_kind == X.OpKind.MEMORY and ins.memory_index_scale == 4:
                load = ins
                break
        if add is None or load is None:
            return None
        base_reg = full(add.op1_register)
        base_val = None
        for ins in reversed(prev):
            if ins.mnemonic == X.Mnemonic.LEA and full(ins.op0_register) == base_reg and ins.is_ip_rel_memory_operand:
                base_val = ins.ip_rel_memory_address
                break
        if base_val is None:
            # Base register defined outside the straight-line window (hoisted out of a loop):
            # walk the CFG backwards and require every reaching definition to be the same rip-relative lea.
            defs = self._reaching_defs(fn, jmp.ip, base_reg)
            if defs and all(d is not None and d.mnemonic == X.Mnemonic.LEA and d.is_ip_rel_memory_operand for d in defs):
                vals = {d.ip_rel_memory_address for d in defs}
                if len(vals) == 1:
                    base_val = vals.pop()
        if base_val is None:
            return None
        load_base = full(load.memory_base)
        if load_base == base_reg:
            tbl = base_val + load.memory_displacement
        else:
            # The table load may use a different register holding the same kind of constant (MSVC/clang often keep
            # __ImageBase in one register for the load and rematerialise it in another for the add).
            lb = self._const_reg(fn, load.ip, load_base, prev)
            if lb is None:
                return None
            tbl = lb + load.memory_displacement
        tbl &= 0xFFFFFFFFFFFFFFFF
        n = self._bound([q for q in prev if q.ip < load.ip], load.memory_index)   # the index as of the table load
        signed = load.mnemonic == X.Mnemonic.MOVSXD
        def entry(o):
            v = self.pe.u32(o - self.base)
            if signed and v & 0x80000000:
                v -= 1 << 32
            return (base_val + v) & 0xFFFFFFFFFFFFFFFF
        return self._read_table(tbl, n, entry, 4)

    def _read_table(self, tbl, n, read, stride) -> JumpTable | None:
        targets = []
        limit = n if n is not None else 1024
        for i in range(limit):
            try:
                t = read(tbl + stride * i)
            except ValueError:
                break
            if not self._is_code(t):
                if n is not None:
                    return None
                break
            targets.append(t)
        return JumpTable(targets) if targets else None

    def _abs(self, v: int) -> int | None:
        """Absolute address as linked -> current address. Identity unless the image moved with synthesized
        relocations (its code still holds the original 32-bit addresses); None if v isn't inside it then."""
        pe = self.pe
        u = v & 0xFFFFFFFFFFFFFFFF
        if not pe.abs_delta:
            return u
        return u + pe.abs_delta if pe.abs_lo <= u < pe.abs_hi else None

    def _const_reg(self, fn, ip, reg, prev):
        """Value of `reg` at `ip` if every reaching definition is the same rip-relative lea, else None."""
        full = X.RegisterExt.full_register
        for ins in reversed([q for q in prev if q.ip < ip]):
            if ins.op_count and ins.op0_kind == X.OpKind.REGISTER and full(ins.op0_register) == reg:
                if ins.mnemonic == X.Mnemonic.LEA and ins.is_ip_rel_memory_operand:
                    return ins.ip_rel_memory_address
                break
        defs = self._reaching_defs(fn, ip, reg)
        if defs and all(d is not None and d.mnemonic == X.Mnemonic.LEA and d.is_ip_rel_memory_operand for d in defs):
            vals = {d.ip_rel_memory_address for d in defs}
            if len(vals) == 1:
                return vals.pop()
        return None

    def _bound(self, prev, idx_reg) -> int | None:
        """Table size from the guarding `cmp idx, N` + `ja` (N+1 entries) or `jae` (N entries).

        Walks backwards following register copies (`movzx eax, dl` means the guard tests dl), stops at any
        other write to the index, and ignores compares that aren't followed by an unsigned range branch
        (e.g. an unrelated `cmp ax, 10Ah; je ...` before the switch)."""
        full = X.RegisterExt.full_register
        idx = full(idx_reg)
        copies = (X.Mnemonic.MOV, X.Mnemonic.MOVZX, X.Mnemonic.MOVSX, X.Mnemonic.MOVSXD)
        for k in range(len(prev) - 1, -1, -1):
            ins = prev[k]
            if ins.mnemonic == X.Mnemonic.CMP and ins.op0_kind == X.OpKind.REGISTER and full(ins.op0_register) == idx \
                    and ins.op1_kind not in (X.OpKind.REGISTER, X.OpKind.MEMORY):
                nxt = prev[k + 1] if k + 1 < len(prev) else None
                if nxt is not None and nxt.mnemonic in (X.Mnemonic.JA, X.Mnemonic.JAE):
                    imm = ins.immediate(1) & ((1 << (8 * X.RegisterExt.size(ins.op0_register))) - 1)
                    return imm + 1 if nxt.mnemonic == X.Mnemonic.JA else imm
                continue
            if ins.op_count and ins.op0_kind == X.OpKind.REGISTER and full(ins.op0_register) == idx:
                if ins.mnemonic in copies and ins.op1_kind == X.OpKind.REGISTER:
                    idx = full(ins.op1_register)  # follow the copy to its source
                    continue
                if ins.mnemonic == X.Mnemonic.CMP:
                    continue
                return None
        return None

    _NONWRITING = None

    def _writes_reg(self, ins: X.Instruction, reg) -> bool:
        if ins.flow_control in (FC.CALL, FC.INDIRECT_CALL):
            return reg not in (X.Register.RBX, X.Register.RBP, X.Register.RDI, X.Register.RSI, X.Register.RSP,
                               X.Register.R12, X.Register.R13, X.Register.R14, X.Register.R15)
        if not ins.op_count or ins.op0_kind != X.OpKind.REGISTER:
            return False
        if X.RegisterExt.full_register(ins.op0_register) != reg:
            return False
        return ins.mnemonic not in (X.Mnemonic.PUSH, X.Mnemonic.CMP, X.Mnemonic.TEST, X.Mnemonic.BT)

    def _reaching_defs(self, fn: Function, ip: int, reg, limit=2000):
        """Definitions of `reg` reaching `ip` over the (partially) decoded CFG. None = reaches entry."""
        preds: dict[int, list[int]] = {}
        for a, ins in fn.insns.items():
            fc = ins.flow_control
            succ = []
            if fc in (FC.NEXT, FC.CALL, FC.INDIRECT_CALL, FC.CONDITIONAL_BRANCH):
                succ.append(ins.next_ip)
            if fc in (FC.CONDITIONAL_BRANCH, FC.UNCONDITIONAL_BRANCH) and ins.op0_kind == X.OpKind.NEAR_BRANCH64:
                succ.append(ins.near_branch_target)
            if a in fn.jump_tables:            # switch edges (tables found so far)
                succ.extend(fn.jump_tables[a].targets)
            for t in succ:
                preds.setdefault(t, []).append(a)
        defs, seen, work = set(), set(), list(preds.get(ip, []))
        while work and len(seen) < limit:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            ins = fn.insns[a]
            if self._writes_reg(ins, reg):
                defs.add(ins)
                continue
            ps = preds.get(a, [])
            if a == fn.addr or not ps:
                defs.add(None)
            work.extend(ps)
        return defs

    def _preceding(self, fn: Function, ip: int, n: int):
        """Instructions linearly preceding ip within the function (crosses the ja/jbe guard)."""
        ips = sorted(a for a in fn.insns if a < ip)
        out = []
        expect = ip
        for a in reversed(ips):
            ins = fn.insns[a]
            if ins.next_ip != expect:
                break
            out.append(ins); expect = a
            if len(out) >= n:
                break
        out.reverse()
        return out
