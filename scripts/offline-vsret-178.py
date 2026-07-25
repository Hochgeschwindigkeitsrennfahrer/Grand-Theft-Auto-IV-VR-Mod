#!/usr/bin/env python3
"""Disasm call [ecx+0x178] @ 0x2C738 near VsRet region (mapped)."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    # find frame before 0x2C738
    site = 0x2C738
    enc = None
    for d in range(0, 0x200):
        r = site - d
        f = r - 0xC00
        if f < 1:
            break
        if data[f : f + 3] == b"\x55\x8B\xEC":
            enc = r
            break
    print(f"site 0x{site:X} frame~{hex(enc) if enc else '?'}")
    print(f"VsRet mapped 0x2D33E bytes: {(data[0x2D33E-0xC00:0x2D33E-0xC00+4]).hex(' ')}")
    print(f"call178 @0x2C738 bytes: {(data[0x2C738-0xC00:0x2C738-0xC00+6]).hex(' ')}")
    start = enc or (site - 0x40)
    fo = start - 0xC00
    for insn in md.disasm(data[fo : fo + 0x120], 0x400000 + start):
        rva = insn.address - 0x400000
        mark = ""
        if rva == 0x2C738:
            mark = "  ; call [ecx+0x178]"
        if rva == 0x2D33E:
            mark = "  ; VsRet"
        if rva == 0x2D2AC:
            mark = "  ; FORBIDDEN VS wrap"
        print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        if insn.mnemonic.startswith("ret") and rva > site:
            break
    # also 0x25720
    print()
    site = 0x25720
    print(f"=== other early call178 @0x{site:X} ===")
    fo = site - 0xC00 - 0x20
    for insn in md.disasm(data[fo : fo + 0x60], 0x400000 + site - 0x20):
        rva = insn.address - 0x400000
        mark = "  ; +0x178" if rva == site else ""
        print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        if rva > site + 0x20:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
