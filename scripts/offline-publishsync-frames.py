#!/usr/bin/env python3
"""Classify enclosing frames for all 12 PublishSync E8 callers (mapped)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")

SITES = [
    0x31624,
    0x317C5,
    0x317FB,
    0x318C9,
    0x3192A,
    0x3199F,
    0x31B0C,
    0x31B8B,
    0x327FF,
    0x21922D,
    0x219C84,
    0x22D550,
]


def main() -> int:
    data = EXE.read_bytes()
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32

        md = Cs(CS_ARCH_X86, CS_MODE_32)
    except ImportError:
        md = None

    print("=== PublishSync caller frames (mapped) ===")
    for s in SITES:
        fo = s - 0xC00
        pre = data[fo - 12 : fo]
        enc = None
        for d in range(0, 0x300):
            r = s - d
            f = r - 0xC00
            if f < 0:
                break
            if data[f : f + 3] == b"\x55\x8B\xEC":
                enc = r
                break
            if d > 0 and data[f] == 0xC3:
                break
            if d > 1 and data[f] == 0xCC and data[f + 1] == 0xCC:
                break
        # also detect 83 3D starts
        if enc is None:
            for d in range(0, 0x100):
                r = s - d
                f = r - 0xC00
                if f < 0:
                    break
                if data[f] == 0x83 and data[f + 1] == 0x3D:
                    enc = r
                    break
                if d > 0 and data[f] == 0xC3:
                    break
        print(f"call@0x{s:X}  pre={pre.hex(' ')}  frame~{hex(enc) if enc else '?'}")
        if md:
            for insn in md.disasm(data[fo - 8 : fo + 5], 0x400000 + s - 8):
                rva = insn.address - 0x400000
                mark = " <<" if rva == s else ""
                print(f"    0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
                if rva >= s:
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
