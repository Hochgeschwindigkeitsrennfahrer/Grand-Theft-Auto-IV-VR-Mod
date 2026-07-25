#!/usr/bin/env python3
"""Find all PE references to ViewConst gate absolute VA 0x1797694."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
GATE = 0x1797694


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

    needle = struct.pack("<I", GATE)
    hits = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    print(f"imm32 0x{GATE:X} occurrences: {len(hits)}")
    for fo in hits:
        r = f2r(fo)
        ctx = data[max(0, fo - 3) : fo + 5]
        print(f"  file 0x{fo:X} rva={hex(r) if r is not None else None} ctx={ctx.hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
