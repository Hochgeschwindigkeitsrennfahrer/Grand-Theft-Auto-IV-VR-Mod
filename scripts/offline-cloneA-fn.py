#!/usr/bin/env python3
"""Map E8 targets inside cloneA fn ~0x2A1D50 through slot178 @0x2A217D."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
START = 0x2A1D50
END = 0x2A21B0
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    print(f"fn 0x{START:X} head: {data[START - SKEW : START - SKEW + 24].hex(' ')}")
    for rva in range(START, END - 5):
        o = rva - SKEW
        if data[o] == 0xE8:
            rel = struct.unpack_from("<i", data, o + 1)[0]
            print(f"  E8 @{rva:X} -> {rva + 5 + rel:X}")
        if data[o : o + 2] in (b"\xff\x90", b"\xff\x91", b"\xff\x92", b"\xff\x96") and data[
            o + 2 : o + 6
        ] == b"\x78\x01\x00\x00":
            print(f"  call [reg+0x178] @{rva:X}")
        if data[o : o + 2] in (b"\xff\x90", b"\xff\x91", b"\xff\x92") and data[
            o + 2 : o + 6
        ] == b"\xb4\x01\x00\x00":
            print(f"  call [reg+0x1B4] @{rva:X}")


if __name__ == "__main__":
    main()
