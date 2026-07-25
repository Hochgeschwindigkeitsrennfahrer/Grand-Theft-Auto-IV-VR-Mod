#!/usr/bin/env python3
"""Find refs to global matrix src 0x1110090 (pushed by thunk 0x32B40)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
ABS = 0x1110090
IMM = struct.pack("<I", ABS)


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    hits = []
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
        chunk = data[raw : raw + rsize]
        pos = 0
        while True:
            j = chunk.find(IMM, pos)
            if j < 0:
                break
            rva = va + j
            # classify opcode before imm32
            prev = chunk[max(0, j - 2) : j]
            hits.append((name, rva, prev.hex(" ")))
            pos = j + 1
    print(f"imm32 0x{ABS:X}: {len(hits)} hits")
    for name, rva, prev in hits[:40]:
        print(f"  {name} rva=0x{rva:X} prev={prev}")
    if len(hits) > 40:
        print(f"  ... +{len(hits) - 40} more")


if __name__ == "__main__":
    main()
