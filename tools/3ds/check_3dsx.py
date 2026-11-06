"""Checks a .3dsx for relocations that 3DSX loaders reject.

A relocated word holds a segment-relative address whose top 4 bits select the segment
(0 = code, 1 = rodata, 2 = data). Loaders refuse anything else, which happens when the
program stores a pointer that falls outside every segment (for example &array[-1]).

Usage: python tools/3ds/check_3dsx.py build/3ds/smg.3dsx [build/3ds/smg.elf] [--stamp file]
With the ELF, each bad relocation is reported with the symbol that contains it.
Exits with an error if any are found; --stamp writes a file on success (for ninja).
"""

import bisect
import struct
import subprocess
import sys

# ##############################################################################################
# # The check                                                                                  #
# #                                                                                            #
# # Each relocated word of the .3dsx must point into one of its segments (details in the       #
# # docstring above); a bad one fails the build, named by the ELF symbol it belongs to.        #
# ##############################################################################################
BASE = 0x00100000


def load_symbols(elf):
    nm = r"C:\devkitPro\devkitARM\bin\arm-none-eabi-nm.exe"
    out = subprocess.run([nm, "-C", "-n", "-S", elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[2].lower() in "tdrb":
            addrs.append(int(parts[0], 16))
            names.append(parts[3])
    return addrs, names


def main():
    args = sys.argv[1:]
    stamp = None
    if "--stamp" in args:
        i = args.index("--stamp")
        stamp = args[i + 1]
        del args[i : i + 2]

    data = open(args[0], "rb").read()
    magic, header_size, reloc_hdr_size, _, _, code_size, rodata_size, data_size, bss_size = struct.unpack_from("<4sHHIIIIII", data, 0)
    assert magic == b"3DSX", "not a 3DSX file"

    seg_sizes = [(code_size + 0xFFF) & ~0xFFF, (rodata_size + 0xFFF) & ~0xFFF, (data_size + 0xFFF) & ~0xFFF]
    seg_bases = [BASE, BASE + seg_sizes[0], BASE + seg_sizes[0] + seg_sizes[1]]
    file_sizes = [code_size, rodata_size, data_size - bss_size]

    pos = header_size
    reloc_counts = []
    for _ in range(3):
        reloc_counts.append(struct.unpack_from("<II", data, pos))
        pos += reloc_hdr_size

    seg_offsets = []
    for size in file_sizes:
        seg_offsets.append(pos)
        pos += size

    symbols = load_symbols(args[1]) if len(args) > 1 else None
    bad = 0
    for seg in range(3):
        for kind, count in zip(("absolute", "relative"), reloc_counts[seg]):
            word = 0
            for _ in range(count):
                skip, patch = struct.unpack_from("<HH", data, pos)
                pos += 4
                word += skip
                for _ in range(patch):
                    offset = seg_offsets[seg] + word * 4
                    if word * 4 < file_sizes[seg]:
                        value = struct.unpack_from("<I", data, offset)[0]
                        if value >> 28 >= 3:
                            bad += 1
                            addr = seg_bases[seg] + word * 4
                            where = ""
                            if symbols:
                                i = bisect.bisect_right(symbols[0], addr) - 1
                                if i >= 0:
                                    where = f" in {symbols[1][i]} (+0x{addr - symbols[0][i]:X})"
                            print(f"bad {kind} relocation at 0x{addr:08X}: value 0x{value:08X}{where}")
                    word += 1

    if bad:
        print(f"{bad} bad relocation(s): 3DSX loaders will refuse this file")
        sys.exit(1)

    if stamp:
        open(stamp, "w").close()


if __name__ == "__main__":
    main()
