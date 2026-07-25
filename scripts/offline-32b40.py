#!/usr/bin/env python3
"""Disasm activeView reads at 0x32B41 / 0x32B64."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    enc = None
    for d in range(0, 0x200):
        r = 0x32B41 - d
        f = r - 0xC00
        if f < 0:
            break
        if data[f : f + 3] == b"\x55\x8B\xEC":
            enc = r
            break
    print(f"frame~{hex(enc) if enc else '?'}")
    start = enc or 0x32AE0
    fo = start - 0xC00
    for insn in md.disasm(data[fo : fo + 0x120], 0x400000 + start):
        rva = insn.address - 0x400000
        mark = ""
        if rva in (0x32B41, 0x32B64):
            mark = "  ; activeView"
        print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        if insn.mnemonic.startswith("ret") and rva > 0x32B64:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
