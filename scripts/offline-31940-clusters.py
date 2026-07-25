#!/usr/bin/env python3
"""Cluster E8 callers of helper 0x31940 (matrix copy -> PublishSync @0x3199F)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
TARGET = 0x31940
TEXT_VA = 0x1000
TEXT_RAW = 0x400
SKEW = TEXT_VA - TEXT_RAW  # 0xC00


def rva_to_off(rva: int) -> int:
    return rva - SKEW


def main() -> None:
    data = EXE.read_bytes()
    text_size = 0xC00000  # generous; scan whole mapped .text-ish
    # Find E8 -> TARGET in mapped RVA space by scanning file and converting
    callers: list[int] = []
    # Scan file bytes as if each file-off F maps to RVA F+SKEW for .text
    # Safer: scan known .text raw range
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    text_raw = text_va = text_vsize = text_rsize = None
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name == b".text":
            text_vsize, text_va, text_rsize, text_raw = struct.unpack_from("<IIII", data, off + 8)
            break
    assert text_raw is not None
    start, end = text_raw, text_raw + text_rsize
    for fo in range(start, end - 5):
        if data[fo] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        call_rva = fo - text_raw + text_va
        next_rva = call_rva + 5
        if next_rva + rel == TARGET:
            callers.append(call_rva)

    print(f"E8 -> 0x{TARGET:X}: {len(callers)}")
    # Cluster by 0x1000 page
    pages: dict[int, list[int]] = {}
    for c in callers:
        pages.setdefault(c & ~0xFFF, []).append(c)
    for page in sorted(pages):
        xs = pages[page]
        print(f"  page 0x{page:X}  n={len(xs)}  " + " ".join(f"0x{x:X}" for x in xs[:8])
              + (" ..." if len(xs) > 8 else ""))

    # Near known seams
    seams = {
        "PublishSync": 0x30F00,
        "ReplayDispatch": 0x30CD0,
        "slot178": 0x30D0D,
        "ViewMat": 0x314C0,
        "PublishProj": 0x31BA0,
        "thunk32B40": 0x32B40,
        "Mode34_4DEA80": 0x4DEA80,
    }
    print("\nNearest caller to seams:")
    for name, rva in seams.items():
        best = min(callers, key=lambda c: abs(c - rva))
        print(f"  {name} 0x{rva:X} -> nearest call@0x{best:X} delta={best - rva:+d}")


if __name__ == "__main__":
    main()
