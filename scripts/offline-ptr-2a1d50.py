#!/usr/bin/env python3
"""Find absolute pointers to cloneA fn 0x2A1D50 (indirect calls)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
# Image base for CE is typically 0x400000 — absolute VA = base + RVA
IMAGE_BASE = 0x400000
TARGET_RVA = 0x2A1D50
ABS = IMAGE_BASE + TARGET_RVA


def main() -> None:
    data = EXE.read_bytes()
    imm = struct.pack("<I", ABS)
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    print(f"Looking for abs VA 0x{ABS:X} (base+0x{TARGET_RVA:X})")
    total = 0
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        _, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        chunk = data[raw : raw + rsize]
        pos = 0
        while True:
            j = chunk.find(imm, pos)
            if j < 0:
                break
            total += 1
            print(f"  {name} rva=0x{va + j:X}")
            pos = j + 1
    print(f"total={total}")


if __name__ == "__main__":
    main()
