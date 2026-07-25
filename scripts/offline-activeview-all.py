#!/usr/bin/env python3
"""Full mapped map of [0x17F583C] activeView reads/writes."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
ACTIVE = 0x17F583C
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def main() -> int:
    data = EXE.read_bytes()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    secs = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, vsz, raw, rsz))

    def f2r(fo: int):
        for va, vsz, raw, rsz in secs:
            if raw <= fo < raw + rsz:
                return va + (fo - raw)
        return None

    imm = struct.pack("<I", ACTIVE)
    writes, reads = [], []
    for fo in range(len(data) - 6):
        b = data[fo]
        if b == 0xA1 and data[fo + 1 : fo + 5] == imm:
            reads.append((f2r(fo), "mov eax,[active]"))
        elif b == 0xA3 and data[fo + 1 : fo + 5] == imm:
            writes.append((f2r(fo), "mov [active],eax"))
        elif b in (0x8B, 0x89) and (data[fo + 1] & 0xC7) == 0x05 and data[fo + 2 : fo + 6] == imm:
            reg = REG32[(data[fo + 1] >> 3) & 7]
            if b == 0x8B:
                reads.append((f2r(fo), f"mov {reg},[active]"))
            else:
                writes.append((f2r(fo), f"mov [active],{reg}"))
        elif b == 0x39 and (data[fo + 1] & 0xC7) == 0x05 and data[fo + 2 : fo + 6] == imm:
            reg = REG32[(data[fo + 1] >> 3) & 7]
            reads.append((f2r(fo), f"cmp [active],{reg}"))
        elif b == 0x3B and (data[fo + 1] & 0xC7) == 0x05 and data[fo + 2 : fo + 6] == imm:
            reg = REG32[(data[fo + 1] >> 3) & 7]
            reads.append((f2r(fo), f"cmp {reg},[active]"))
    print(f"activeView writes={len(writes)} reads={len(reads)}")
    for r, k in writes:
        print(f"  WRITE mapped 0x{r:X}  {k}")
    print("  key reads:")
    for r, k in reads:
        if r and (0x30B00 <= r <= 0x33000 or r in (0x97777A,)):
            print(f"    0x{r:X}  {k}")
    print(f"  total reads shown subset; full count={len(reads)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
