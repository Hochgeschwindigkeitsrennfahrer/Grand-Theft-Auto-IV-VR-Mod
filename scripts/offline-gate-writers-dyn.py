#!/usr/bin/env python3
"""Hunt non-imm32 writers for ViewConst gate [0x1797694] (mov/or via lea patterns)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
GATE = 0x1797694
IMM = struct.pack("<I", GATE)


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    print(f"imm32 occurrences of 0x{GATE:X}:")
    total = 0
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        chunk = data[raw : raw + rsize]
        pos = 0
        while True:
            j = chunk.find(IMM, pos)
            if j < 0:
                break
            total += 1
            rva = va + j
            prev = chunk[max(0, j - 3) : j]
            # classify: cmp/mov/etc
            print(f"  {name} rva=0x{rva:X} prev3={prev.hex(' ')}")
            pos = j + 1
    print(f"total={total}")
    # nearby globals ±0x40
    print("\nNearby imm32 density (±0x40 of gate):")
    for delta in range(-0x40, 0x44, 4):
        absva = GATE + delta
        imm = struct.pack("<I", absva)
        n = data.count(imm)
        if n:
            print(f"  [0x{absva:X}] refs={n}")


if __name__ == "__main__":
    main()
