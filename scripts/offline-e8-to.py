#!/usr/bin/env python3
"""Generic: list E8 callers of a mapped RVA target."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", type=lambda s: int(s, 0))
    ap.add_argument("--limit", type=int, default=80)
    args = ap.parse_args()
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    text_raw = text_va = text_rsize = None
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name == b".text":
            _, text_va, text_rsize, text_raw = struct.unpack_from("<IIII", data, off + 8)
            break
    assert text_raw is not None
    callers = []
    for fo in range(text_raw, text_raw + text_rsize - 5):
        if data[fo] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        call_rva = fo - text_raw + text_va
        if call_rva + 5 + rel == args.target:
            callers.append(call_rva)
    print(f"E8 -> 0x{args.target:X}: {len(callers)}")
    for c in callers[: args.limit]:
        print(f"  0x{c:X}")
    if len(callers) > args.limit:
        print(f"  ... +{len(callers) - args.limit}")


if __name__ == "__main__":
    main()
