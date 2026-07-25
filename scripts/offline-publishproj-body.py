#!/usr/bin/env python3
"""Sketch PublishProj 0x31BA0 body: flags + E8 targets."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
START = 0x31BA0
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    print("prologue:", data[START - SKEW : START - SKEW + 32].hex(" "))
    print("E8 targets in +0x200:")
    for rva in range(START, START + 0x200):
        o = rva - SKEW
        if data[o] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, o + 1)[0]
        print(f"  @{rva:X} -> {rva + 5 + rel:X}")
        if data[o + 5 : o + 6] == b"\xC3" or data[rva + 5 - SKEW] == 0xC3:
            pass
    # find ret
    for rva in range(START, START + 0x280):
        o = rva - SKEW
        if data[o] == 0xC3 and data[o - 1] != 0xE8:
            # crude: first C3 after some progress
            if rva > START + 0x40:
                print(f"first likely ret @0x{rva:X}")
                break


if __name__ == "__main__":
    main()
