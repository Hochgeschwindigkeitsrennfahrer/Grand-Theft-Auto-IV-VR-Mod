#!/usr/bin/env python3
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00
IB = 0x400000


def main() -> None:
    d = EXE.read_bytes()
    print("CC-pad 83 EC candidates 0x2A2200..0x2A2500:")
    for r in range(0x2A2200, 0x2A2500):
        o = r - SKEW
        if d[o - 1] == 0xCC and d[o] == 0x83 and d[o + 1] == 0xEC:
            print(f"  0x{r:X}: {d[o:o+16].hex(' ')}")
    print("abs ptr density:")
    for rva in range(0x2A2200, 0x2A2600, 0x10):
        imm = struct.pack("<I", IB + rva)
        n = d.count(imm)
        if n:
            print(f"  0x{rva:X}: {n}")


if __name__ == "__main__":
    main()
