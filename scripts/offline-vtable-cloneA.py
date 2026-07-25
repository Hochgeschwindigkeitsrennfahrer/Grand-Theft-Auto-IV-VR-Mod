#!/usr/bin/env python3
"""Show .rdata context around abs pointers to cloneA 0x2A1D50."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IB = 0x400000
TARGET = IB + 0x2A1D50


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    imm = struct.pack("<I", TARGET)
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name != b".rdata":
            continue
        _, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        chunk = data[raw : raw + rsize]
        pos = 0
        while True:
            j = chunk.find(imm, pos)
            if j < 0:
                break
            rva = va + j
            # show surrounding dwords as possible vtable
            print(f"hit rva=0x{rva:X}")
            base = j - (j % 4)
            for k in range(-8, 9):
                off2 = base + k * 4
                if off2 < 0 or off2 + 4 > len(chunk):
                    continue
                val = struct.unpack_from("<I", chunk, off2)[0]
                mark = " <<<" if val == TARGET else ""
                # if looks like code ptr in image
                if IB < val < IB + 0x1000000:
                    print(f"  [{k:+d}] VA 0x{val:X} rva 0x{val - IB:X}{mark}")
                else:
                    print(f"  [{k:+d}] 0x{val:X}{mark}")
            pos = j + 1


if __name__ == "__main__":
    main()
