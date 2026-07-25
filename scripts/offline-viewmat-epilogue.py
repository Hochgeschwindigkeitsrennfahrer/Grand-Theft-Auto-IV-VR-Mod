#!/usr/bin/env python3
"""Disassemble ViewMatWriter region through PublishSync@0x31624 + PublishProj."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
TEXT_VA = 0x1000
TEXT_RAW = 0x400
SKEW = 0xC00


def rva_to_off(rva: int) -> int:
    return rva - SKEW


def main() -> None:
    data = EXE.read_bytes()
    start, end = 0x314C0, 0x31640
    fo = rva_to_off(start)
    blob = data[fo : fo + (end - start)]
    print(f"bytes 0x{start:X}..0x{end:X} ({len(blob)}):")
    # hex dump + mark E8 targets
    i = 0
    while i < len(blob):
        rva = start + i
        b = blob[i]
        if b == 0xE8 and i + 5 <= len(blob):
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            tgt = rva + 5 + rel
            print(f"  0x{rva:X}: E8 -> 0x{tgt:X}")
            i += 5
            continue
        if b == 0xE9 and i + 5 <= len(blob):
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            tgt = rva + 5 + rel
            print(f"  0x{rva:X}: E9 -> 0x{tgt:X}")
            i += 5
            continue
        if b == 0xC3:
            print(f"  0x{rva:X}: C3 ret")
            i += 1
            continue
        if b == 0xC2 and i + 3 <= len(blob):
            imm = struct.unpack_from("<H", blob, i + 1)[0]
            print(f"  0x{rva:X}: C2 {imm:04X} retn")
            i += 3
            continue
        # skip other bytes quietly in chunks
        i += 1
    # also show 16 bytes before sync@0x31624
    sync = 0x31624
    fo2 = rva_to_off(sync - 16)
    print("\nctx before PublishSync call@0x31624:")
    print(" ", data[fo2 : fo2 + 32].hex(" "))


if __name__ == "__main__":
    main()
