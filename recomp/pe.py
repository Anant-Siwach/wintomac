"""Minimal PE32+ (x86-64) parser: headers, sections, imports, exports, .pdata, relocations."""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Section:
    name: str
    rva: int
    vsize: int
    raw_off: int
    raw_size: int
    chars: int

    @property
    def executable(self) -> bool:
        return bool(self.chars & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE

    @property
    def writable(self) -> bool:
        return bool(self.chars & 0x80000000)


@dataclass
class UnwindInfo:
    flags: int            # UNW_FLAG_EHANDLER=1, UHANDLER=2, CHAININFO=4
    handler_rva: int      # language-specific handler (0 if none)
    handler_data_rva: int # RVA of the handler-specific data that follows
    chained_begin: int    # for CHAININFO: begin RVA of the primary function


@dataclass
class Import:
    dll: str
    name: str | None      # None when imported by ordinal
    ordinal: int | None
    iat_rva: int          # RVA of the IAT slot the loader fills


@dataclass
class PE:
    data: bytes
    image_base: int
    entry_rva: int
    size_of_image: int
    size_of_headers: int
    subsystem: int
    sections: list[Section] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: dict[int, str] = field(default_factory=dict)          # rva -> name
    runtime_functions: list[tuple[int, int, int]] = field(default_factory=list)  # (begin, end, unwind) RVAs
    relocs: list[int] = field(default_factory=list)                 # RVAs of DIR64 fixups
    synth_relocs: bool = False   # relocs were recovered by synthesize_relocs(), not read from .reloc
    abs_lo: int = 0              # after a synthesized move: the image's original VA range [abs_lo, abs_hi) ...
    abs_hi: int = 0
    abs_delta: int = 0           # ... and how far it moved. Absolute addresses in code are translated by the lifter.
    tls_callbacks: list[int] = field(default_factory=list)          # RVAs
    tls: tuple | None = None   # (raw_start_rva, raw_end_rva, index_rva, zero_fill) from IMAGE_TLS_DIRECTORY64
    unwind: dict = field(default_factory=dict)  # func begin rva -> UnwindInfo (handler/handler data for EH frames)
    timestamp: int = 0
    is_dll: bool = False
    orig_base: int = 0

    # --- address helpers -------------------------------------------------
    def section_for_rva(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.rva <= rva < s.rva + max(s.vsize, s.raw_size):
                return s
        return None

    def read(self, rva: int, n: int) -> bytes:
        """Read n bytes at an RVA as the loader would map them (zero-filled bss)."""
        out = bytearray()
        while n > 0:
            s = self.section_for_rva(rva)
            if s is None:
                if rva < self.size_of_headers:
                    chunk = self.data[rva:rva + min(n, self.size_of_headers - rva)]
                    out += chunk
                    n -= len(chunk); rva += len(chunk)
                    continue
                raise ValueError(f"rva {rva:#x} not mapped")
            off = rva - s.rva
            avail = max(s.vsize, s.raw_size) - off
            take = min(n, avail)
            raw = self.data[s.raw_off + off: s.raw_off + min(off + take, s.raw_size)] if off < s.raw_size else b""
            out += raw + b"\0" * (take - len(raw))
            n -= take; rva += take
        return bytes(out)

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.read(rva, 4))[0]

    def u64(self, rva: int) -> int:
        return struct.unpack_from("<Q", self.read(rva, 8))[0]

    def cstr(self, rva: int) -> str:
        out = bytearray()
        while True:
            b = self.read(rva, 1)[0]
            if b == 0:
                return out.decode("latin-1")
            out.append(b); rva += 1

    def is_code_rva(self, rva: int) -> bool:
        s = self.section_for_rva(rva)
        return s is not None and s.executable

    def file_offset(self, rva: int) -> int | None:
        s = self.section_for_rva(rva)
        if s is None or rva - s.rva >= s.raw_size:
            return None
        return s.raw_off + (rva - s.rva)

    def synthesize_relocs(self) -> int:
        """Recover DIR64 relocations for an image linked without them (old MinGW defaults: fixed base 0x400000).
        Data: every 8-byte-aligned value in a non-executable section that points into the image is taken to be
        a pointer (vtables, string tables, jump tables, TLS/CRT tables). Code holds absolute addresses as 32-bit
        immediates/displacements, which can't be patched to an address above 4 GB; rebase() records the move
        (abs_lo/abs_hi/abs_delta) and the lifter and analysis translate those instead.
        Returns the number of relocations found. False positives need an integer that happens to equal an
        address inside the image: rare, and logged for review in report.json."""
        lo, hi = self.image_base, self.image_base + self.size_of_image
        # Structures holding RVAs, not VAs: with a low base an RVA can look like an address inside the image
        # (base 0x400000, RVA 0x4f5276), so they are excluded from the scan.
        skip = self._rva_tables()
        narrow = self._narrow_accesses()
        found = []
        for sec in self.sections:
            if sec.executable or not sec.raw_size:
                continue
            n = min(sec.raw_size, sec.vsize or sec.raw_size)
            for off in range(0, n - 7, 8):
                rva = sec.rva + off
                if rva % 8 or any(a <= rva < b for a, b in skip):
                    continue
                v = struct.unpack_from("<Q", self.data, sec.raw_off + off)[0]
                if lo <= v < hi and rva not in narrow and rva + 4 not in narrow:
                    found.append(rva)
        # MinGW's linker script puts the constructor/destructor lists (__CTOR_LIST__/__DTOR_LIST__) inside
        # .text: -1, function pointers..., 0. Code bytes aren't scanned in general (they would yield false
        # "pointers" inside instructions), only this exact shape.
        for sec in self.sections:
            if not sec.executable or not sec.raw_size:
                continue
            n = min(sec.raw_size, sec.vsize or sec.raw_size)
            q = lambda off: struct.unpack_from("<Q", self.data, sec.raw_off + off)[0]
            for off in range(0, n - 15, 8):
                if (sec.rva + off) % 8 or q(off) != 0xFFFFFFFFFFFFFFFF:
                    continue
                k, ptrs = off + 8, []
                while k + 8 <= n and lo <= q(k) < hi:
                    ptrs.append(sec.rva + k); k += 8
                if k + 8 <= n and q(k) == 0:
                    found.extend(ptrs)
        self.relocs = sorted(set(found))
        self.synth_relocs = True
        return len(self.relocs)

    def _narrow_accesses(self) -> set[int]:
        """RVAs the code reads or writes directly (rip-relative or absolute) with an operand narrower than
        8 bytes: such a location holds an int, a float or a char, not a pointer. Found by a linear sweep of
        the executable sections; a misdecoded byte here only removes a candidate, never adds one."""
        import iced_x86 as X
        lo, hi = self.image_base, self.image_base + self.size_of_image
        out = set()
        for sec in self.sections:
            if not sec.executable or not sec.raw_size:
                continue
            code = self.data[sec.raw_off:sec.raw_off + min(sec.raw_size, sec.vsize or sec.raw_size)]
            for ins in X.Decoder(64, code, ip=self.image_base + sec.rva):
                if ins.code == X.Code.INVALID or not ins.op_count:
                    continue
                for i in range(ins.op_count):
                    if ins.op_kind(i) != X.OpKind.MEMORY:
                        continue
                    if ins.is_ip_rel_memory_operand:
                        a = ins.ip_rel_memory_address
                    elif ins.memory_base == X.Register.NONE and ins.memory_index == X.Register.NONE:
                        a = ins.memory_displacement & 0xFFFFFFFFFFFFFFFF
                    else:
                        continue
                    size = X.MemorySizeExt.size(ins.memory_size)
                    if lo <= a < hi and 0 < size < 8 and ins.mnemonic != X.Mnemonic.LEA:
                        out.add(a - self.image_base)
        return out

    def _rva_tables(self) -> list[tuple[int, int]]:
        """[start, end) RVA ranges of PE structures whose 8-byte entries are RVAs or ordinals: import and
        delay-import lookup/address tables, plus the resource, exception, debug and IAT directories."""
        lf = struct.unpack_from("<I", self.data, 0x3C)[0]
        ndirs = struct.unpack_from("<I", self.data, lf + 24 + 108)[0]
        dirs = [struct.unpack_from("<II", self.data, lf + 24 + 112 + 8 * i) for i in range(min(ndirs, 16))]
        out = [(r, r + n) for i, (r, n) in enumerate(dirs) if i in (1, 2, 3, 6, 12, 13) and r and n]

        def u32(rva):
            o = self.file_offset(rva)
            return struct.unpack_from("<I", self.data, o)[0] if o is not None else 0

        def thunk_array(rva):   # null-terminated array of 8-byte entries
            if not rva:
                return
            end = rva
            while (o := self.file_offset(end)) is not None and struct.unpack_from("<Q", self.data, o)[0]:
                end += 8
            out.append((rva, end + 8))

        if len(dirs) > 1 and dirs[1][0]:   # import descriptors: OriginalFirstThunk (ILT) and FirstThunk (IAT)
            d = dirs[1][0]
            while u32(d + 12):
                thunk_array(u32(d)); thunk_array(u32(d + 16)); d += 20
        if len(dirs) > 13 and dirs[13][0]:  # delay-load descriptors: IAT at +12, INT at +16
            d = dirs[13][0]
            while u32(d + 4):
                thunk_array(u32(d + 12)); thunk_array(u32(d + 16)); d += 32
        return out

    def rebase(self, new_base: int):
        """Apply this image's DIR64 base relocations to our copy of the bytes, as the Windows loader would
        if it mapped the image at new_base. Everything recompiled afterwards sees final addresses."""
        delta = (new_base - self.image_base) & 0xFFFFFFFFFFFFFFFF
        if not delta:
            return
        if not self.relocs and not self.synth_relocs:   # synthesized with 0 fixups: nothing in data to move
            raise ValueError(f"image has no relocations; cannot move it from {self.image_base:#x} to {new_base:#x}")
        buf = bytearray(self.data)
        for rva in self.relocs:
            off = self.file_offset(rva)
            if off is None:
                continue  # fixup in bss: applied at runtime after the zero-fill (runtime applies all fixups anyway)
            v = struct.unpack_from("<Q", buf, off)[0]
            struct.pack_into("<Q", buf, off, (v + delta) & 0xFFFFFFFFFFFFFFFF)
        self.data = bytes(buf)
        self.tls_callbacks = [r for r in self.tls_callbacks]  # RVAs: unaffected
        self.orig_base = self.image_base
        if self.synth_relocs:
            self.abs_lo, self.abs_hi = self.image_base, self.image_base + self.size_of_image
            self.abs_delta = new_base - self.image_base
        self.image_base = new_base

    @property
    def exec_sections(self) -> list[Section]:
        return [s for s in self.sections if s.executable]


def _unwind_info(pe: "PE", rva: int) -> UnwindInfo:
    vf, _, ncodes, _ = pe.read(rva, 4)
    flags = vf >> 3
    tail = rva + 4 + 2 * ((ncodes + 1) & ~1)
    if flags & 4:  # chained: a RUNTIME_FUNCTION follows
        return UnwindInfo(flags, 0, 0, pe.u32(tail))
    if flags & 3:
        return UnwindInfo(flags, pe.u32(tail), tail + 4, 0)
    return UnwindInfo(flags, 0, 0, 0)


def parse(path: str) -> PE:
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        raise ValueError("not an MZ executable")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, nsec, timestamp, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    if machine != 0x8664:
        raise ValueError(f"unsupported machine {machine:#x} (only x86-64 / AMD64 is supported)")
    opt = pe_off + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise ValueError("not a PE32+ image")
    entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    size_of_image, size_of_headers = struct.unpack_from("<II", data, opt + 56)
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    ndirs = struct.unpack_from("<I", data, opt + 108)[0]
    dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * i) for i in range(ndirs)]

    pe = PE(data, image_base, entry_rva, size_of_image, size_of_headers, subsystem)
    pe.timestamp, pe.is_dll, pe.orig_base = timestamp, bool(chars & 0x2000), image_base
    sec_off = opt + opt_size
    for i in range(nsec):
        o = sec_off + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
        if name.startswith("/") and name[1:].isdigit():   # long name ("/4"): offset into the COFF string table
            symtab, nsyms = struct.unpack_from("<II", data, pe_off + 12)
            so = symtab + 18 * nsyms + int(name[1:])
            if symtab and so < len(data):
                name = data[so:data.index(b"\0", so)].decode("latin-1")
        vsize, rva, raw_size, raw_off = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        pe.sections.append(Section(name, rva, vsize, raw_off, raw_size, chars))

    def d(i):
        return dirs[i] if i < len(dirs) else (0, 0)

    # Exports (dir 0)
    rva, size = d(0)
    if rva:
        nfuncs, nnames, a_funcs, a_names, a_ords = struct.unpack_from("<IIIII", pe.read(rva + 20, 20))
        names = {}
        for i in range(nnames):
            ordi = struct.unpack_from("<H", pe.read(a_ords + 2 * i, 2))[0]
            names[ordi] = pe.cstr(pe.u32(a_names + 4 * i))
        for i in range(nfuncs):
            frva = pe.u32(a_funcs + 4 * i)
            if frva and not (rva <= frva < rva + size):  # skip forwarders
                pe.exports[frva] = names.get(i, f"ord{i}")

    # Imports (dir 1)
    rva, _ = d(1)
    if rva:
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", pe.read(rva, 20))
            if not name_rva:
                break
            dll = pe.cstr(name_rva)
            look = ilt or iat
            k = 0
            while True:
                ent = pe.u64(look + 8 * k)
                if ent == 0:
                    break
                if ent >> 63:
                    pe.imports.append(Import(dll, None, ent & 0xFFFF, iat + 8 * k))
                else:
                    pe.imports.append(Import(dll, pe.cstr((ent & 0x7FFFFFFF) + 2), None, iat + 8 * k))
                k += 1
            rva += 20

    # Exception directory / .pdata (dir 3): gives us function boundaries for free on x64.
    rva, size = d(3)
    if rva:
        for i in range(size // 12):
            b, e, u = struct.unpack_from("<III", pe.read(rva + 12 * i, 12))
            if b:
                pe.runtime_functions.append((b, e, u))
                try:
                    pe.unwind[b] = _unwind_info(pe, u)
                except (ValueError, struct.error):
                    pass

    # Base relocations (dir 5)
    rva, size = d(5)
    end = rva + size
    while rva and rva < end:
        page, blk = struct.unpack_from("<II", pe.read(rva, 8))
        if blk < 8:
            break
        for j in range((blk - 8) // 2):
            ent = struct.unpack_from("<H", pe.read(rva + 8 + 2 * j, 2))[0]
            if ent >> 12 == 10:  # IMAGE_REL_BASED_DIR64
                pe.relocs.append(page + (ent & 0xFFF))
        rva += blk

    # TLS (dir 9): static TLS template + index slot + callbacks
    rva, _ = d(9)
    if rva:
        start_va, end_va, index_va = pe.u64(rva), pe.u64(rva + 8), pe.u64(rva + 16)
        zero_fill = pe.u32(rva + 32)
        if start_va or index_va:
            pe.tls = (start_va - image_base if start_va else 0, end_va - image_base if end_va else 0,
                      index_va - image_base if index_va else 0, zero_fill)
        cb_va = pe.u64(rva + 24)
        if cb_va:
            k = 0
            while True:
                v = pe.u64(cb_va - image_base + 8 * k)
                if not v:
                    break
                pe.tls_callbacks.append(v - image_base); k += 1
    return pe
