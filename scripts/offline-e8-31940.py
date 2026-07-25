#!/usr/bin/env python3
"""Find all E8 callers of copy+PublishSync helper 0x31940."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    tgt = 0x31940
    va, raw, rsz = 0x1000, 0x400, 0xA72000
    hits = []
    for fo in range(raw, raw + rsz - 5):
        if data[fo] != 0xE8:
            continue
        site = va + (fo - raw)
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        if site + 5 + rel == tgt:
            hits.append(site)
    print(f"E8 -> 0x31940: {len(hits)}")
    for s in hits:
        print(f"  call@0x{s:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
