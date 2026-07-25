#!/usr/bin/env python3
"""Map PublishProj 0x31BA0 callers + shared PublishSync/PublishProj tails (mapped RVAs)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IMAGE_BASE = 0x400000


def main() -> int:
    data = EXE.read_bytes()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    secs = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, vsz, raw, rsz))
    va, vsz, raw, rsz = secs[0]

    def e8_to(tgt: int) -> list[int]:
        hits = []
        for fo in range(raw, raw + rsz - 5):
            if data[fo] != 0xE8:
                continue
            site = va + (fo - raw)
            rel = struct.unpack_from("<i", data, fo + 1)[0]
            if site + 5 + rel == tgt:
                hits.append(site)
        return hits

    def find_frame(site: int):
        for d in range(0, 0x500):
            r = site - d
            fo = raw + (r - va) if va <= r < va + rsz else None
            if fo is None or fo < 0:
                break
            if data[fo : fo + 3] == b"\x55\x8B\xEC":
                return r
            if d > 0 and data[fo] == 0xC3:
                break
        return None

    sync = set(e8_to(0x30F00))
    proj = set(e8_to(0x31BA0))
    print(f"PublishSync callers: {len(sync)}")
    print(f"PublishProj callers: {len(proj)}")
    # Sites where PublishProj call is within 16 bytes after PublishSync call
    pairs = []
    for s in sorted(sync):
        for p in sorted(proj):
            if 0 < p - s <= 16:
                pairs.append((s, p, find_frame(s)))
    print(f"PublishSync then PublishProj within +16: {len(pairs)}")
    for s, p, enc in pairs:
        enc_s = f"0x{enc:X}" if enc else "?"
        print(f"  sync@0x{s:X} proj@0x{p:X} enclosing~{enc_s}")

    only_sync = sorted(sync - {s for s, _, _ in [(a, b, c) for a, b, c in pairs]})
    # recompute only_sync properly
    paired_sync = {s for s, _, _ in pairs}
    print("PublishSync without nearby PublishProj:")
    for s in sorted(sync - paired_sync):
        print(f"  sync@0x{s:X} enclosing~{hex(find_frame(s) or 0)}")

    print("PublishProj without nearby PublishSync:")
    paired_proj = {p for _, p, _ in pairs}
    for p in sorted(proj - paired_proj):
        print(f"  proj@0x{p:X} enclosing~{hex(find_frame(p) or 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
