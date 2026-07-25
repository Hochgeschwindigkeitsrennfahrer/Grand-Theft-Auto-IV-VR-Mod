#!/usr/bin/env python3
"""Compare old file-off VsRet 0x2C73E vs mapped +0xC00 claim 0x2D33E."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)

    def show(rva: int, as_file: bool = False):
        if as_file:
            fo = rva
            mapped = rva + 0xC00
            label = f"fileOff 0x{rva:X} => mapped 0x{mapped:X}"
        else:
            fo = rva - 0xC00
            label = f"mapped 0x{rva:X} (file 0x{fo:X})"
        print(f"{label}: {data[fo:fo+12].hex(' ')}")

    print("=== Treating address as MAPPED (subtract 0xC00 for file) ===")
    for rva in (0x2C73E, 0x2D33E, 0x2C738, 0x2D2AC, 0x2CD80):
        show(rva, as_file=False)
    print()
    print("=== Treating address as FILE OFF (add 0xC00 for mapped) ===")
    for rva in (0x2C73E, 0x2C6AC, 0x2C180, 0x2C738):
        show(rva, as_file=True)
    print()
    print("=== disasm mapped 0x2D33E neighborhood ===")
    fo = 0x2D33E - 0xC00 - 0x30
    for insn in md.disasm(data[fo : fo + 0x60], 0x400000 + 0x2D33E - 0x30):
        print(f"  0x{insn.address-0x400000:06X}: {insn.mnemonic} {insn.op_str}")
    print()
    print("=== disasm FILE 0x2C73E as mapped display (WRONG if it was already mapped) ===")
    # If old docs used file-off 0x2C73E, real mapped is 0x2D33E — already above.
    # If old docs accidentally used correct mapped 0x2C73E, bytes at file 0x2BB3E:
    fo = 0x2C73E - 0xC00
    print(f"bytes at file for mapped-0x2C73E: {data[fo:fo+8].hex(' ')}")
    fo2 = 0x2C73E  # raw file
    print(f"bytes at raw file 0x2C73E: {data[fo2:fo2+8].hex(' ')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
