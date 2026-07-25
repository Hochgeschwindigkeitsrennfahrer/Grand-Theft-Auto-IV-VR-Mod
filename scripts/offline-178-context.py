#!/usr/bin/env python3
"""16-byte context around each call [reg+0x178] (device-nearby vs not)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
DEVICE = 0x17ED8D8
SITES = [
    0x25720, 0x2C738, 0x30D0D, 0x3725E, 0x37BFB, 0x635DB,
    0x29EE34, 0x2A217D, 0x2A25F9, 0x2A3675, 0x9B18B6, 0x9F1D1A,
]
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    imm = struct.pack("<I", DEVICE)
    for rva in SITES:
        o = rva - SKEW
        ctx = data[o - 24 : o + 12]
        near = imm in data[o - 0x40 : o + 0x20]
        print(f"0x{rva:X} deviceNear={near}")
        print(f"  -24..+12: {ctx.hex(' ')}")


if __name__ == "__main__":
    main()
