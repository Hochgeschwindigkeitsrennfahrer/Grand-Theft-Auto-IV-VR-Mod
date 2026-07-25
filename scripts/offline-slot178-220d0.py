#!/usr/bin/env python3
"""Document Mode65 slot178 target 0x220D0 (DXVK SetVSConstF) + ReplayDispatch ABI."""
from __future__ import annotations

import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00
IMAGE_BASE = 0x400000


def main() -> None:
    data = EXE.read_bytes()
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    print("Mode65 slot178 live target = mapped RVA 0x220D0")
    print("Role: D3D9/DXVK SetVertexShaderConstantF-style queue writer")
    print("  ret 0x10 -> stdcall cleanup of 4 stack args after 'this'")
    print("  sibling slot1B4 @0x225B0 ~ same body, tag or 0xE (PS const) vs 0xB (VS)")
    print()
    print("ReplayDispatch 0x30CD0 call site ABI -> 0x220D0:")
    print("  ecx = device = [0x17ED8D8]")
    print("  push count=0x10 (16 floats = 4x4 matrix)")
    print("  push src = view+0x80 (esi after sub esi,-0x80)")
    print("  push startRegister=0")
    print("  push device")
    print("  call [vtable+0x178]")
    print()
    print("Stereo implication (DUAL NOTES ONLY — not implemented):")
    print("  Distinct L/R needs different bytes at view+0x80 (or dual ReplayDispatch)")
    print("  BEFORE call [eax+0x178] @ 0x30D0D. Submit-only cannot create parallax.")
    print("  Mode 68 COUNT @ ReplayDispatch 0x30CD0 (2 E8 callers).")
    print("  Inject site candidates (future dual, SameFrameSeamGate=CLOSED for now):")
    print("    1) Hook ReplayDispatch entry 0x30CD0 COUNT-proven; patch matrix at esi+0x80")
    print("       after gate, before lea/push @ ~0x30CF0..0x30D0D")
    print("    2) Or write L then R into view+0x80 and call 0x220D0 twice (risky ABI)")
    print("    3) Do NOT open SameFrameSeamGate until Mode68 PASS + dual plan review")
    print()

    for rva, title in [
        (0x220D0, "slot178 / SetVSConstF"),
        (0x225B0, "slot1B4 / SetPSConstF-like"),
        (0x30CD0, "ReplayDispatch"),
    ]:
        fo = rva - SKEW
        print(f"=== {title} @ 0x{rva:X} ===")
        print(" ", data[fo : fo + 16].hex(" "))
        for insn in md.disasm(data[fo : fo + 0x40], IMAGE_BASE + rva):
            print(f"  0x{insn.address - IMAGE_BASE:06X}: {insn.mnemonic} {insn.op_str}")
            if insn.mnemonic.startswith("ret"):
                break
        print()


if __name__ == "__main__":
    main()
