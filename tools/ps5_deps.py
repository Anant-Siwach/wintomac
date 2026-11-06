#!/usr/bin/env python3
"""List the AnyPS5 libs/ DLLs a relinked PS5 program needs (transitive PE imports that exist in libs/).

usage: tools/ps5_deps.py <libs_dir> <seed.prx|seed.dll|path/to/module.dll ...>
Prints paths (one per line) of every seed plus everything they import from libs_dir, dependencies first.
A seed that is an existing file outside libs_dir (e.g. a relinked *.guest.prx module) is not printed itself;
the libraries it imports are.
Seeds are the PS5 libraries the ELF names in DT_NEEDED (libkernel_web.sprx -> libkernel_web.prx) plus libc.prx and
libkernel.prx, which AnyPS5's Windows startup always loads. System DLLs (KERNEL32, api-ms-win-crt-*, ...) are shims.
"""
import os, struct, sys

def pe_imports(path):
    with open(path, 'rb') as f:
        d = f.read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    imp_rva = struct.unpack_from('<I', d, opt + 112 + 8)[0]
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsz + 40 * i
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, s + 8)
        secs.append((va, max(vsz, rsz), rptr))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
        raise ValueError(hex(rva))
    out = []
    if not imp_rva:
        return out
    p = off(imp_rva)
    while True:
        ilt, _, _, name, iat = struct.unpack_from('<IIIII', d, p)
        if not name and not iat:
            break
        n = off(name)
        out.append(d[n:d.index(b'\0', n)].decode())
        p += 20
    return out

def main():
    libs = sys.argv[1]
    present = {f.lower(): f for f in os.listdir(libs)}
    order, seen = [], set()
    def visit(name):
        key = name.lower()
        if key.endswith('.sprx'):
            key = key[:-5] + '.prx'
        if key in seen or key not in present:
            return
        seen.add(key)
        path = os.path.join(libs, present[key])
        for dep in pe_imports(path):
            visit(dep)
        order.append(path)
    for s in sys.argv[2:]:
        if os.path.isfile(s) and os.path.dirname(os.path.realpath(s)) != os.path.realpath(libs):
            for dep in pe_imports(s):
                visit(dep)
        else:
            visit(os.path.basename(s))
    print('\n'.join(order))

if __name__ == '__main__':
    main()
