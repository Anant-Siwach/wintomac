"""Check a tests/ps5/agc_draw.c run against what tests/ps5/gen_agc_draw.py expects (agc_draw.expect.json):
sample points of frame_000.bmp (AnyPS5's GPU readback of the flipped display buffer; edges of shapes are left out:
coverage on diagonals is not what is tested) and the hashes of checked buffers that the program printed in run.log.
  python3 -I tests/ps5/check_agc_frame.py <run dir> [expect.json]"""
import json
import os
import re
import struct
import sys


def main():
    d = sys.argv[1]
    exp_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "app0", "agc_draw.expect.json")
    exp = json.load(open(exp_path))
    raw = open(os.path.join(d, "frame_000.bmp"), "rb").read()
    off, = struct.unpack_from("<I", raw, 10)
    w, h = struct.unpack_from("<ii", raw, 18)
    rows = [raw[off + r * w * 4: off + (r + 1) * w * 4] for r in range(abs(h))]
    if h > 0:
        rows.reverse()
    px = b"".join(rows)
    hh = 2166136261
    for c in px:
        hh = ((hh ^ c) * 16777619) & 0xFFFFFFFF
    print("frame %dx%d hash %08x" % (w, abs(h), hh))
    bad = 0
    for x, y, want, tol in exp["points"]:
        o = (y * w + x) * 4
        got = (px[o + 2], px[o + 1], px[o])
        ok = all(abs(a - b) <= tol for a, b in zip(got, want))
        bad += not ok
        print("  (%3d,%3d) rgb%s %s" % (x, y, got, "ok" if ok else "expected %s (tolerance %d)" % (tuple(want), tol)))
    log = open(os.path.join(d, "run.log"), errors="replace").read()
    for i, c in enumerate(exp["checks"]):
        m = re.search(r"^check %d: \d+ bytes hash ([0-9a-f]{8})" % i, log, re.M)
        got = m.group(1) if m else "missing"
        ok = got == c["hash"]
        bad += not ok
        print("  check %d (%s): hash %s %s" % (i, c["name"], got, "ok" if ok else "expected " + c["hash"]))
    print("PASS" if bad == 0 else "FAIL (%d)" % bad)
    sys.exit(1 if bad else 0)


main()
