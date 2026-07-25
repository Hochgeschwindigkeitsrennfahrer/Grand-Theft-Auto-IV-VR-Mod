#!/usr/bin/env python3
"""Disassemble helper 0x31940 body + nearby 0x30720."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()

    def dump(rva: int, n: int = 16) -> None:
        b = data[rva - SKEW : rva - SKEW + n]
        print(f"0x{rva:X}: {b.hex(' ')}")

    for rva in (0x30720, 0x307F0, 0x308F0, 0x31940, 0x32B40, 0x32B60):
        dump(rva, 24)

    print("\n0x31940 E8 targets:")
    b = data[0x31940 - SKEW : 0x31940 - SKEW + 0x70]
    for i in range(len(b) - 5):
        if b[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", b, i + 1)[0]
        print(f"  @{0x31940 + i:X} -> {0x31940 + i + 5 + rel:X}")

    print("\nThunk E8:")
    for base in (0x32B40, 0x32B60):
        b = data[base - SKEW : base - SKEW + 32]
        for i in range(len(b) - 5):
            if b[i] != 0xE8:
                continue
            rel = struct.unpack_from("<i", b, i + 1)[0]
            print(f"  @{base + i:X} -> {base + i + 5 + rel:X}")


if __name__ == "__main__":
    main()
