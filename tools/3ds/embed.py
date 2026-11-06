"""Writes a binary file as a C header: `static const unsigned char name[]` and `name_size`.

Usage: python tools/3ds/embed.py input output name
"""

import sys
from pathlib import Path


def main():
    src, dst, name = sys.argv[1:4]
    data = Path(src).read_bytes()
    lines = [f"// Generated from {Path(src).name} by tools/3ds/embed.py; do not edit.", "#pragma once", ""]
    lines.append(f"static const unsigned char {name}[] __attribute__((aligned(4))) = {{")
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 16]) + ",")
    lines.append("};")
    lines.append(f"static const unsigned int {name}_size = {len(data)};")
    Path(dst).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
