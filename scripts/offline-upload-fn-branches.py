#!/usr/bin/env python3
"""Find conditional branches between the two MatMul×3 paths inside 0x2A1E10."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00
A, B = 0x2A21B0, 0x2A2580  # between first +1B4 and second MatMul


def main() -> None:
    data = EXE.read_bytes()
    print(f"jcc / jmp in 0x{A:X}..0x{B:X}:")
    rva = A
    while rva < B:
        o = rva - SKEW
        b0 = data[o]
        # short jcc
        if 0x70 <= b0 <= 0x7F:
            rel = struct.unpack_from("<b", data, o + 1)[0]
            print(f"  @{rva:X}: jcc8 -> {rva + 2 + rel:X}")
            rva += 2
            continue
        # near jcc 0F 8x
        if b0 == 0x0F and 0x80 <= data[o + 1] <= 0x8F:
            rel = struct.unpack_from("<i", data, o + 2)[0]
            print(f"  @{rva:X}: jcc32 -> {rva + 6 + rel:X}")
            rva += 6
            continue
        if b0 == 0xE9:
            rel = struct.unpack_from("<i", data, o + 1)[0]
            print(f"  @{rva:X}: jmp -> {rva + 5 + rel:X}")
            rva += 5
            continue
        if b0 == 0xEB:
            rel = struct.unpack_from("<b", data, o + 1)[0]
            print(f"  @{rva:X}: jmp8 -> {rva + 2 + rel:X}")
            rva += 2
            continue
        rva += 1


if __name__ == "__main__":
    main()
