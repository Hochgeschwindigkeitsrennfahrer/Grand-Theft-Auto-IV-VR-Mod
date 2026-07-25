#!/usr/bin/env python3
"""Disasm 0x22FD0 — active-view +0x280 helper (SetActiveView / viewport tails)."""
from __future__ import annotations

import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> int:
    data = EXE.read_bytes()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    start = 0x22FD0
    fo = start - 0xC00
    print(f"=== 0x{start:X} prologue {data[fo:fo+16].hex(' ')} ===")
    # E8 callers
    va, raw, rsz = 0x1000, 0x400, 0xA72000
    hits = []
    for f in range(raw, raw + rsz - 5):
        if data[f] != 0xE8:
            continue
        site = va + (f - raw)
        rel = int.from_bytes(data[f + 1 : f + 5], "little", signed=True)
        if site + 5 + rel == start:
            hits.append(site)
    print(f"E8 callers: {len(hits)}")
    for s in hits[:16]:
        print(f"  call@0x{s:X}")
    if len(hits) > 16:
        print(f"  ... +{len(hits)-16}")
    print()
    n = 0
    for insn in md.disasm(data[fo : fo + 0x80], 0x400000 + start):
        print(f"  0x{insn.address-0x400000:06X}: {insn.mnemonic} {insn.op_str}")
        n += 1
        if insn.mnemonic.startswith("ret"):
            break
        if n > 40:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
