#!/usr/bin/env python3
"""Sketch fn 0x319B0 (two PublishSync+Proj tails) and 0x317E0 helper."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00


def e8_scan(data: bytes, start: int, end: int) -> None:
    print(f"E8 in 0x{start:X}..0x{end:X}:")
    for rva in range(start, end - 5):
        o = rva - SKEW
        if data[o] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, o + 1)[0]
        tgt = rva + 5 + rel
        print(f"  @{rva:X} -> 0x{tgt:X}")


def main() -> None:
    data = EXE.read_bytes()
    for rva, n in ((0x319B0, 24), (0x317E0, 16), (0x31810, 16), (0x31880, 16), (0x318E0, 16)):
        b = data[rva - SKEW : rva - SKEW + n]
        print(f"0x{rva:X}: {b.hex(' ')}")
    e8_scan(data, 0x319B0, 0x31BA0)
    e8_scan(data, 0x317E0, 0x31940)


if __name__ == "__main__":
    main()
