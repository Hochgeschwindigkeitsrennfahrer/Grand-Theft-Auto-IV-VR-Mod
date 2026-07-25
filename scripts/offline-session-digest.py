#!/usr/bin/env python3
"""Digest key offline RE facts for the 2026-07-25 session."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00


def main() -> None:
    data = EXE.read_bytes()
    checks = [
        ("PublishSync", 0x30F00, bytes.fromhex("55 8B EC 83 E4 F8 51 56 8B F1")),
        ("ViewMat", 0x314C0, bytes.fromhex("55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9")),
        ("ViewConst", 0x32470, bytes.fromhex("55 8B EC 83 E4 F0 81 EC A8 00 00 00")),
        ("UploadFn", 0x2A1E10, bytes.fromhex("55 8B EC 83 E4 F0 81 EC D8 00 00 00")),
        ("Sibling", 0x2A1D50, bytes.fromhex("83 EC 08 53 55 56")),
        ("ReplayDisp", 0x30CD0, bytes.fromhex("83 3D 18 D9 7E 01 00 56 8B F1")),
    ]
    print("Prologue checks:")
    for name, rva, want in checks:
        got = data[rva - SKEW : rva - SKEW + len(want)]
        print(f"  {name:12} 0x{rva:X} {'OK' if got == want else 'FAIL'}")

    # gate
    gate_imm = struct.pack("<I", 0x1797694)
    print(f"ViewConst gate imm32 hits: {data.count(gate_imm)}")

    # upload rets
    for call in (0x2A217D, 0x2A25F9, 0x30D0D):
        print(f"  +178 call 0x{call:X} ret 0x{call + 6:X}")


if __name__ == "__main__":
    main()
