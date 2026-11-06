#!/usr/bin/env python3
"""bin2h.py <in> <out.h> <name>: embed a binary file as `static const unsigned char name[]`."""
import sys
data = open(sys.argv[1], "rb").read()
name = sys.argv[3]
rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
open(sys.argv[2], "w").write(f"static const unsigned char {name}[{len(data)}] = {{\n  " + ",\n  ".join(rows) + "\n};\n")
