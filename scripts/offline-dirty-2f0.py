#!/usr/bin/env python3
"""Find mov byte [reg+0x2F0], imm near ViewMat / PublishSync cluster (dirty rebuild bit)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name == b".text":
            _, text_va, text_rsize, text_raw = struct.unpack_from("<IIII", data, off + 8)
            break
    chunk = data[text_raw : text_raw + text_rsize]
    # C6 8x F0 02 00 00 xx  (mov byte [reg+0x2F0], imm8)
    # C6 80/81/82/86/87
    hits = []
    for j in range(len(chunk) - 7):
        if chunk[j] != 0xC6:
            continue
        modrm = chunk[j + 1]
        disp = chunk[j + 2 : j + 6]
        if disp != b"\xF0\x02\x00\x00":
            continue
        # mod=01 or 10 with disp32; for mod=10 disp32 after modrm
        # actually pattern C6 8x F0 02 00 00 means mod=10 (mem+disp32), reg/op=0, r/m=x
        if (modrm & 0xC0) != 0x80:
            continue
        imm = chunk[j + 6]
        rva = text_va + j
        hits.append((rva, modrm, imm))
    print(f"mov byte [reg+0x2F0],imm : {len(hits)}")
    near = [h for h in hits if 0x30000 <= h[0] <= 0x33000 or 0x210000 <= h[0] <= 0x220000]
    print(f"near publish/viewmat windows: {len(near)}")
    for rva, modrm, imm in near[:40]:
        print(f"  0x{rva:X} modrm=0x{modrm:02X} imm={imm}")


if __name__ == "__main__":
    main()
