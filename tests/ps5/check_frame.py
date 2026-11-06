"""Check frame_<i>.bmp files that AnyPS5 wrote (APS5_DUMP_FRAMES=n APS5_DUMP_SCALE=1) against the pattern
tests/ps5/gfx_flip.c draws. Prints one line per frame: size, FNV-1a hash of the BGRA pixels, and how many pixels
differ from the expected pattern by more than `tol` per channel.
  python3 -I tests/ps5/check_frame.py <dir> <frames> [tol]"""
import struct, sys, os

W, H = 640, 360


def expected(frame):
    px = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            r, g, b = x * 255 // (W - 1), y * 255 // (H - 1), (frame * 80) & 255
            if 100 <= y < 180 and 40 + frame * 40 <= x < 120 + frame * 40:
                r = g = b = 255
            if 300 <= y < 330 and 200 <= x < 440:
                r, g, b = 255, 0, 0
            o = (y * W + x) * 4
            px[o:o + 4] = bytes((b, g, r, 255))
    return px


def read_bmp(path):
    d = open(path, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    bpp, = struct.unpack_from("<H", d, 28)
    assert bpp == 32, bpp
    rows = [d[off + r * w * 4: off + (r + 1) * w * 4] for r in range(abs(h))]
    if h > 0:
        rows.reverse()  # bottom-up
    return w, abs(h), b"".join(rows)


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def main():
    d, n = sys.argv[1], int(sys.argv[2])
    tol = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    ok = True
    for i in range(n):
        p = os.path.join(d, "frame_%03d.bmp" % i)
        if not os.path.exists(p):
            print("frame %d: missing" % i); ok = False; continue
        w, h, px = read_bmp(p)
        if (w, h) != (W, H):
            print("frame %d: %dx%d (expected %dx%d)" % (i, w, h, W, H)); ok = False; continue
        exp = expected(i)
        bad = sum(1 for k in range(0, len(px), 4) if any(abs(px[k + c] - exp[k + c]) > tol for c in range(3)))
        print("frame %d: %dx%d hash %08x, %d pixels off by > %d" % (i, w, h, fnv(px), bad, tol))
        ok &= bad == 0
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


main()
