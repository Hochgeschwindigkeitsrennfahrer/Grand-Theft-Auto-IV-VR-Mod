#!/usr/bin/env python3
"""Disasm sister +0x178 paths at 0x2A217D / 0x2A25F9 vs ReplayDispatch."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00


def e8s(data: bytes, start: int, end: int) -> None:
    for rva in range(start, end - 5):
        o = rva - SKEW
        if data[o] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, o + 1)[0]
        print(f"  E8 @{rva:X} -> {rva + 5 + rel:X}")


def main() -> None:
    data = EXE.read_bytes()
    for label, start, end in (
        ("ReplayDispatch", 0x30CD0, 0x30D30),
        ("cloneA window", 0x2A20E0, 0x2A21B0),
        ("cloneB window", 0x2A2560, 0x2A2630),
    ):
        print(f"=== {label} ===")
        print(data[start - SKEW : start - SKEW + 16].hex(" "))
        e8s(data, start, end)


if __name__ == "__main__":
    main()
