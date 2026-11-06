"""Patch AnyPS5's Windows libSceAgcDriver.prx for the macOS address space (docs/ps5_graphics.md).

AnyPS5's AGC driver keeps a 2 MiB "driver memory" block at the fixed guest address 0xFE0000000 (inside the PS5's
system-reserved range; it holds a stub shader and descriptors the GPU emulation reads). On arm64 macOS the kernel
reserves [0xFC0000000, 0x7000000000) in every process (max protection none: mmap there fails with EACCES), so the
driver's GuestArenaCommit at that address fails and sceVideoOutOpen throws. This moves the block to 0xFB0000000,
still inside the PS5 system-reserved range [0x7FFFFC000, 0x1000000000) (never handed to the game) but mappable here.

Only `movabs reg, imm64` and `movabs [moffs64], rax` / `movabs rax, [moffs64]` operands in the 2 MiB window are
rewritten (the descriptor constant 0x10000FE0040060 carries extra high bits and is handled the same way). The file is
patched in place; running it again is a no-op.
  python3 -I tools/ps5_mac_patch.py <libs dir or libSceAgcDriver.prx>"""
import os
import struct
import sys

OLD, NEW, SIZE = 0xFE0000000, 0xFB0000000, 0x200000
LOW48 = (1 << 48) - 1


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(n):
        name, vs, va, rs, ra, _, _, _, _, ch = struct.unpack_from("<8sIIIIIIHHI", d, pe + 24 + opt + 40 * i)
        yield name.rstrip(b"\0").decode(), ra, rs, ch


def patch(path):
    d = bytearray(open(path, "rb").read())
    hits = []
    for name, ra, rs, ch in sections(d):
        if not ch & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            continue
        for i in range(ra, ra + rs - 10):
            b0, b1 = d[i], d[i + 1]
            if b0 in (0x48, 0x49) and 0xB8 <= b1 <= 0xBF:      # movabs r64, imm64
                at = i + 2
            elif b0 == 0x48 and b1 in (0xA1, 0xA3):           # movabs rax, [moffs64] / movabs [moffs64], rax
                at = i + 2
            else:
                continue
            v = struct.unpack_from("<Q", d, at)[0]
            lo = v & LOW48
            if OLD <= lo < OLD + SIZE and (v >> 48) <= 0x10:
                hits.append((at, v))
    for at, v in hits:
        struct.pack_into("<Q", d, at, (v & ~LOW48) | ((v & LOW48) - OLD + NEW))
    if hits:
        open(path, "wb").write(d)
    print("ps5_mac_patch: %s: %d driver-memory addresses moved 0x%x -> 0x%x" % (os.path.basename(path), len(hits), OLD, NEW))
    return len(hits)


def main():
    p = sys.argv[1]
    if os.path.isdir(p):
        p = os.path.join(p, "libSceAgcDriver.prx")
    if not os.path.exists(p):
        print("ps5_mac_patch: no %s (nothing to patch)" % p)
        return
    patch(p)


main()
