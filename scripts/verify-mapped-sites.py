#!/usr/bin/env python3
"""Assert critical mapped RVAs still match CE GTAIV.exe prologues. Exit 1 on drift."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")

SITES = [
    ("PublishSync", 0x30F00, bytes.fromhex("55 8B EC 83 E4 F8 51 56 8B F1")),
    ("ViewMatWriter", 0x314C0, bytes.fromhex("55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9")),
    ("ViewConstTRUE", 0x32470, bytes.fromhex("55 8B EC 83 E4 F0 81 EC A8 00 00 00")),
    ("ViewConstMID", 0x3247C, bytes.fromhex("56 57 8B F9 8D 44 24 70")),
    ("ReplayDispatch", 0x30CD0, bytes.fromhex("83 3D 18 D9 7E 01 00 56 8B F1")),
    ("call178", 0x30D0D, bytes.fromhex("FF 90 78 01 00 00")),
    ("MatMul", 0x307F0, bytes.fromhex("55 8B EC")),
    ("SetActiveView", 0x30BF0, bytes.fromhex("83 3D 50 BC B4 01 00 56")),
    ("PublishProj", 0x31BA0, bytes.fromhex("55 8B EC 83 E4 F0 81 EC 88 00 00 00")),
    ("UploadFn", 0x2A1E10, bytes.fromhex("55 8B EC 83 E4 F0 81 EC D8 00 00 00")),
    ("UploadSibling", 0x2A1D50, bytes.fromhex("83 EC 08 53 55 56")),
    ("helper31940", 0x31940, bytes.fromhex("8B 54 24 04 56 8B 02 8B F1")),
    ("upload178a", 0x2A217D, bytes.fromhex("FF 91 78 01 00 00")),
    ("upload178b", 0x2A25F9, bytes.fromhex("FF 91 78 01 00 00")),
    ("VsRet", 0x2D33E, bytes.fromhex("85 C0 75 14")),
    ("VSwrapFORBIDDEN", 0x2D2AC, bytes.fromhex("89 51 0A")),
    ("FovSite", 0x706F7C, bytes.fromhex("E8")),
]


def load_sections(data: bytes):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    out = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        out.append((va, vsz, raw, rsz))
    return out


def main() -> int:
    if not EXE.is_file():
        print(f"MISS exe: {EXE}")
        return 1
    data = EXE.read_bytes()
    secs = load_sections(data)

    def r2f(rva: int):
        for va, vsz, raw, rsz in secs:
            if va <= rva < va + max(vsz, rsz):
                return raw + (rva - va)
        return None

    bad = 0
    for name, rva, expect in SITES:
        fo = r2f(rva)
        if fo is None:
            print(f"FAIL {name} 0x{rva:X} — no section")
            bad += 1
            continue
        got = data[fo : fo + len(expect)]
        if got != expect:
            print(f"FAIL {name} 0x{rva:X} file=0x{fo:X}")
            print(f"  want {expect.hex(' ').upper()}")
            print(f"  got  {got.hex(' ').upper()}")
            bad += 1
        else:
            print(f"OK   {name:18s} mapped 0x{rva:X} file 0x{fo:X}")
    if bad:
        print(f"\n{bad} site(s) DRIFTED — update Mode 64/66/67 AOB / RE_OFFSETS")
        return 1
    print("\nAll critical mapped sites match CE exe.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
