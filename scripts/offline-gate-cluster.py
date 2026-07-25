#!/usr/bin/env python3
"""Scan absolute VA refs near ViewConst gate BSS cluster 0x1797600..0x1797700."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
LO, HI = 0x1797600, 0x1797700


def main() -> int:
    data = EXE.read_bytes()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    secs = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, vsz, raw, rsz))

    def f2r(fo: int):
        for va, vsz, raw, rsz in secs:
            if raw <= fo < raw + rsz:
                return va + (fo - raw)
        return None

    refs: dict[int, list[tuple[int, str]]] = {}
    for fo in range(len(data) - 6):
        # any imm32 in range
        imm = struct.unpack_from("<I", data, fo)[0]
        if not (LO <= imm < HI):
            continue
        # classify opcode byte(s) before imm
        kind = "imm?"
        if fo >= 1 and data[fo - 1] == 0xB8:
            kind = "mov eax,imm"
        elif fo >= 2 and data[fo - 2] == 0xC7 and data[fo - 1] == 0x05:
            kind = "mov [imm],imm32"  # wrong - imm is addr
        elif fo >= 2 and data[fo - 2] == 0xC6 and data[fo - 1] == 0x05:
            kind = "mov byte [imm],imm8"
        elif fo >= 2 and data[fo - 2] == 0x80 and data[fo - 1] == 0x3D:
            kind = "cmp byte [imm],imm8"
        elif fo >= 2 and data[fo - 2] == 0x83 and data[fo - 1] == 0x3D:
            kind = "cmp dword [imm],imm8"
        elif fo >= 2 and data[fo - 2] in (0x8B, 0x89) and (data[fo - 1] & 0xC7) == 0x05:
            kind = "mov r/[imm]"
        elif fo >= 1 and data[fo - 1] == 0xA1:
            kind = "mov eax,[imm]"
        elif fo >= 1 and data[fo - 1] == 0xA3:
            kind = "mov [imm],eax"
        elif fo >= 2 and data[fo - 2] == 0xFF and data[fo - 1] == 0x35:
            kind = "push [imm]"
        elif fo >= 2 and data[fo - 2] == 0xFF and data[fo - 1] == 0x15:
            kind = "call [imm]"
        site = f2r(fo - 2) if fo >= 2 else f2r(fo)
        refs.setdefault(imm, []).append((site or fo, kind))

    print(f"Absolute refs in VA range 0x{LO:X}..0x{HI:X}:")
    for addr in sorted(refs):
        print(f"  VA 0x{addr:X}:")
        for site, kind in refs[addr][:8]:
            print(f"    @0x{site:X}  {kind}")
        if len(refs[addr]) > 8:
            print(f"    ... +{len(refs[addr]) - 8} more")
    print(f"distinct addresses: {len(refs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
