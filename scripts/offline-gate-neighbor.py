#!/usr/bin/env python3
"""Classify refs to ViewConst-gate neighbor [0x1797668] (6 refs)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
ABS = 0x1797668
IMM = struct.pack("<I", ABS)


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        _, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        chunk = data[raw : raw + rsize]
        pos = 0
        while True:
            j = chunk.find(IMM, pos)
            if j < 0:
                break
            rva = va + j
            ctx = chunk[max(0, j - 6) : j + 4]
            print(f"{name} 0x{rva:X} ctx={ctx.hex(' ')}")
            pos = j + 1


if __name__ == "__main__":
    main()
