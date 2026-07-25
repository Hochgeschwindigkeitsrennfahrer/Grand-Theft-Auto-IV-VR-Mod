#!/usr/bin/env python3
"""Sketch sibling helper-only fn 0x2A1D50 .. retn 0x10 @0x2A1E0D."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
START, END = 0x2A1D50, 0x2A1E10
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    print(f"0x{START:X}..0x{END:X}")
    print("head:", data[START - SKEW : START - SKEW + 32].hex(" "))
    for rva in range(START, END - 5):
        o = rva - SKEW
        if data[o] == 0xE8:
            rel = struct.unpack_from("<i", data, o + 1)[0]
            print(f"  E8 @{rva:X} -> 0x{rva + 5 + rel:X}")
        if data[o] in (0xC2, 0xC3):
            print(f"  ret @{rva:X} {data[o:o+3].hex(' ')}")


if __name__ == "__main__":
    main()
