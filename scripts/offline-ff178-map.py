#!/usr/bin/env python3
"""Map all call [reg+0x178] / +0x1B4 sites with mapped RVAs (CE .text +0xC00)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    secs = []
    for i in range(num):
        o = sec + i * 40
        name = data[o : o + 8].split(b"\x00")[0].decode(errors="replace")
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, va, vsz, raw, rsz))

    def f2r(fo: int):
        for name, va, vsz, raw, rsz in secs:
            if raw <= fo < raw + rsz:
                return name, va + (fo - raw)
        return None, None

    hits178 = []
    hits1b4 = []
    for fo in range(len(data) - 6):
        if data[fo] != 0xFF:
            continue
        modrm = data[fo + 1]
        if ((modrm >> 3) & 7) != 2 or (modrm >> 6) != 2:
            continue
        disp = struct.unpack_from("<i", data, fo + 2)[0]
        name, rva = f2r(fo)
        if rva is None:
            continue
        if disp == 0x178:
            hits178.append((rva, name, modrm & 7))
        elif disp == 0x1B4:
            hits1b4.append((rva, name, modrm & 7))

    regs = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
    print(f"call [reg+0x178] sites: {len(hits178)}")
    for rva, secn, rm in hits178:
        print(f"  0x{rva:06X}  .{secn:8s}  call [{regs[rm]}+0x178]")
    print(f"call [reg+0x1B4] sites: {len(hits1b4)}")
    for rva, secn, rm in hits1b4[:20]:
        print(f"  0x{rva:06X}  .{secn:8s}  call [{regs[rm]}+0x1B4]")
    if len(hits1b4) > 20:
        print(f"  ... +{len(hits1b4) - 20} more")
    print()
    print("ReplayDispatch pair (expect 0x30D0D / 0x30D20):")
    for rva, secn, rm in hits178:
        if 0x30C00 <= rva <= 0x30E00:
            print(f"  178 @0x{rva:X}")
    for rva, secn, rm in hits1b4:
        if 0x30C00 <= rva <= 0x30E00:
            print(f"  1B4 @0x{rva:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
