#!/usr/bin/env python3
"""Print the DT_NEEDED library names of a PS5 ELF (eboot.bin / homebrew ELF), e.g. libkernel.sprx.
usage: tools/ps5_needed.py <elf>   (input is untrusted data: only parsed; run with python3 -I)"""
import struct, sys

def needed(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        raise SystemExit(f"{path}: not a 64-bit ELF")
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads, dyn = [], None
    for i in range(phnum):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if t == 1: loads.append((va, off, fsz))
        elif t == 2: dyn = (off, fsz)
        elif t == 0x61000000 and dyn is None: pass   # PT_SCE_DYNLIBDATA (signed titles): handled below
    if dyn is None:
        return []
    ents = [struct.unpack_from('<qQ', d, dyn[0] + 16 * k) for k in range(dyn[1] // 16)]
    strtab = next((v for t, v in ents if t == 5), None)
    def off(va):
        for lva, lo, fsz in loads:
            if lva <= va < lva + fsz: return va - lva + lo
        return None
    out = []
    if strtab is not None and off(strtab) is not None:
        base = off(strtab)
        for t, v in ents:
            if t == 1:
                s = base + v
                out.append(d[s:d.index(b'\0', s)].decode())
    return out

if __name__ == '__main__':
    print(' '.join(needed(sys.argv[1])))
