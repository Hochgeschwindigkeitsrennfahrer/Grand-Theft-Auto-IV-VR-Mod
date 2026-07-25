#!/usr/bin/env python3
"""Disasm the optional ViewMatWriter caller ending at activeView cmp 0x3143F."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32

        md = Cs(CS_ARCH_X86, CS_MODE_32)
    except ImportError:
        print("need capstone")
        return 1

    site = 0x3143A
    enc = None
    for d in range(0, 0x800):
        r = site - d
        f = r - 0xC00
        if f < 0:
            break
        if data[f : f + 3] == b"\x55\x8B\xEC":
            enc = r
            break
        if d > 0 and data[f] == 0xC3:
            # keep going — may be nested
            continue
        # stackless thiscall: 56 8B F1 etc after CC pad
        if d > 1 and data[f] == 0xCC and data[f + 1] == 0xCC and data[f + 2] != 0xCC:
            # next byte after pad could be start
            if data[f + 2 : f + 5] == b"\x55\x8B\xEC":
                enc = r - d + 2  # wrong
            # try r as pad end: start at r+1 if walking from pad
            pass

    # Prefer nearest CC-pad + prologue before site
    enc = None
    for d in range(0, 0x800):
        r = site - d
        f = r - 0xC00
        if f < 2:
            break
        if data[f : f + 3] == b"\x55\x8B\xEC" and data[f - 1] == 0xCC:
            enc = r
            break
        if data[f : f + 3] == b"\x55\x8B\xEC":
            enc = r
            break

    print(f"call ViewMat @0x{site:X}; frame~{hex(enc) if enc else '?'}")
    if not enc:
        # dump 0x80 bytes before
        fo = site - 0xC00 - 0x80
        print("raw before:")
        print(data[fo : fo + 0x90].hex(" "))
        return 0

    fo = enc - 0xC00
    print(f"prologue: {data[fo:fo+16].hex(' ')}")
    for insn in md.disasm(data[fo : fo + 0x220], 0x400000 + enc):
        rva = insn.address - 0x400000
        mark = ""
        if rva == 0x3143A:
            mark = "  ; call ViewMatWriter"
        if rva == 0x3143F:
            mark = "  ; cmp activeView"
        print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        if insn.mnemonic.startswith("ret") and rva > site:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
