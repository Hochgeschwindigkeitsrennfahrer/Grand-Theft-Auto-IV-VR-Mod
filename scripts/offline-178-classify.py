#!/usr/bin/env python3
"""Classify each call [reg+0x178]: does nearby code load [0x17ed8d8] device singleton?"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
DEVICE = struct.pack("<I", 0x17ED8D8)
SITES = [
    0x25720,
    0x2C738,
    0x30D0D,
    0x3725E,
    0x37BFB,
    0x635DB,
    0x29EE34,
    0x2A217D,
    0x2A25F9,
    0x2A3675,
    0x9B18B6,
    0x9F1D1A,
]


def main() -> int:
    data = EXE.read_bytes()
    print("site       deviceNearby  bytes")
    for rva in SITES:
        fo = rva - 0xC00
        window = data[fo - 0x40 : fo + 0x20]
        near = DEVICE in window
        print(f"0x{rva:06X}  {'YES' if near else 'no ':3s}          {data[fo:fo+6].hex(' ')}")
    print()
    print("YES = [0x17ed8d8] imm appears within -0x40..+0x20 of the call (likely same device path).")
    print("ReplayDispatch stereo seam = 0x30D0D (expect YES).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
