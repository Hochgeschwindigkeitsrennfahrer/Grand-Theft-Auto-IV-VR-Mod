#!/usr/bin/env python3
"""Deep note: ViewConst sole caller + PublishSync caller list (mapped RVAs)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IMAGE_BASE = 0x400000


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
    data = EXE.read_bytes()
    secs = load_sections(data)

    def r2f(r: int):
        for va, vsz, raw, rsz in secs:
            if va <= r < va + max(vsz, rsz):
                return raw + (r - va)
        return None

    def bytes_r(r: int, n: int = 16) -> str:
        f = r2f(r)
        if f is None:
            return "MISS"
        return " ".join(f"{b:02X}" for b in data[f : f + n])

    def find_frame(site: int, max_back: int = 0x400):
        for d in range(0, max_back + 1):
            r = site - d
            f = r2f(r)
            if f is None:
                break
            if data[f : f + 3] == b"\x55\x8B\xEC":
                return r
            if d > 0 and data[f] == 0xC3:
                break
            if d > 1 and data[f] == 0xCC and data[f + 1] == 0xCC:
                break
        return None

    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32

        md = Cs(CS_ARCH_X86, CS_MODE_32)
    except ImportError:
        md = None

    caller = 0x9777C2
    print(f"=== ViewConst sole E8 caller @ 0x{caller:X} ===")
    print(f"  bytes: {bytes_r(caller, 16)}")
    start = find_frame(caller, 0x800)
    print(f"  enclosing frame: 0x{start:X}" if start else "  enclosing: ?")
    if md and start is not None:
        fo = r2f(start)
        assert fo is not None
        for insn in md.disasm(data[fo : fo + 0x280], IMAGE_BASE + start):
            rva = insn.address - IMAGE_BASE
            mark = "  ; << call ViewConst 0x32470" if rva == caller else ""
            print(f"  0x{rva:06X}: {insn.mnemonic} {insn.op_str}{mark}")
            if insn.mnemonic == "ret" and rva > caller:
                break
            if rva > caller + 0x60:
                break
    print()

    # PublishSync callers
    target = 0x30F00
    va, vsz, raw, rsz = secs[0]
    hits = []
    for fo in range(raw, raw + rsz - 5):
        if data[fo] != 0xE8:
            continue
        site = va + (fo - raw)
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        if site + 5 + rel == target:
            hits.append(site)
    print(f"=== All E8 -> PublishSync 0x30F00 ({len(hits)}) ===")
    for site in hits:
        enc = find_frame(site)
        enc_s = f"0x{enc:X}" if enc is not None else "?"
        print(f"  call@0x{site:X} enclosing~{enc_s}  {bytes_r(site, 8)}")
    print()

    # ViewMatWriter callers
    target = 0x314C0
    hits = []
    for fo in range(raw, raw + rsz - 5):
        if data[fo] != 0xE8:
            continue
        site = va + (fo - raw)
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        if site + 5 + rel == target:
            hits.append(site)
    print(f"=== All E8 -> ViewMatWriter 0x314C0 ({len(hits)}) ===")
    for site in hits:
        enc = find_frame(site)
        enc_s = f"0x{enc:X}" if enc is not None else "?"
        print(f"  call@0x{site:X} enclosing~{enc_s}  {bytes_r(site, 8)}")
    print()

    # What is 0x977600 region (ViewConst caller parent from earlier scan)?
    print("=== bytes around PhaseC-ish ViewConst caller parent ===")
    for r in (0x977600, 0x9777C0, 0x9777C2):
        print(f"  0x{r:X}: {bytes_r(r, 20)}")
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
