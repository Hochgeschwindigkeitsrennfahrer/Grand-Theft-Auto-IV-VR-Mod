#!/usr/bin/env python3
"""Disasm viewport/resize fn that optionally calls ViewMatWriter then activeView tail."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    start = 0x31110
    fo = start - 0xC00
    print(f"=== mapped 0x{start:X} prologue {data[fo:fo+16].hex(' ')} ===")
    n = 0
    for insn in md.disasm(data[fo : fo + 0x360], 0x400000 + start):
        rva = insn.address - 0x400000
        mark = ""
        if rva == 0x3143A:
            mark = "  ; call ViewMatWriter"
        if rva == 0x3143F:
            mark = "  ; cmp activeView"
        if rva == 0x3144D:
            mark = "  ; call 0x22FD0 view+0x280"
        print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        n += 1
        if insn.mnemonic.startswith("ret") and rva >= 0x31450:
            break
        if n > 140:
            print("  ... truncated")
            break
    print()
    print("NOTE: this is one of the 9 E8->ViewMatWriter sites (call@0x3143A).")
    print("After ViewMat it may call 0x22FD0 if this==activeView — no ReplayDispatch.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
