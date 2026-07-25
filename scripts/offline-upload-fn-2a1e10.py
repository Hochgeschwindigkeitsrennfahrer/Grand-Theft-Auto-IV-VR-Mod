#!/usr/bin/env python3
"""Map upload fn 0x2A1E10 (vtable) through +178/+1B4."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
START, END = 0x2A1E10, 0x2A2660
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    print(f"0x{START:X} head: {data[START - SKEW : START - SKEW + 16].hex(' ')}")
    for rva in range(START, END - 5):
        o = rva - SKEW
        if data[o] == 0xE8:
            rel = struct.unpack_from("<i", data, o + 1)[0]
            tgt = rva + 5 + rel
            if tgt in (0x31940, 0x30720, 0x307F0, 0x30F00, 0x30CD0, 0x31810, 0x31BA0):
                print(f"  E8 @{rva:X} -> 0x{tgt:X}")
        if data[o : o + 6] == b"\xff\x91\x78\x01\x00\x00":
            print(f"  +178 @{rva:X}  ret={rva + 6:#x}  (Mode42 OWNER-EDGE)")
        if data[o : o + 6] == b"\xff\x90\x78\x01\x00\x00":
            print(f"  +178(eax) @{rva:X}  ret={rva + 6:#x}")
        if data[o : o + 6] == b"\xff\x91\xb4\x01\x00\x00":
            print(f"  +1B4 @{rva:X}")


if __name__ == "__main__":
    main()
