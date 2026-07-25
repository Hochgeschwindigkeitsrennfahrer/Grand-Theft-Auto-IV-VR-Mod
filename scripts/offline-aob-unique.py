#!/usr/bin/env python3
"""Check Mode 64/66/67 prologue AOBs are unique in CE .text."""
from __future__ import annotations

from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
SKEW = 0xC00

PATTERNS = {
    "PublishSync": bytes.fromhex("55 8B EC 83 E4 F8 51 56 8B F1"),
    "ViewMatWriter": bytes.fromhex("55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9"),
    "ViewConstTRUE": bytes.fromhex("55 8B EC 83 E4 F0 81 EC A8 00 00 00 56 57 8B F9"),
    "ReplayDispatch": bytes.fromhex("83 3D 18 D9 7E 01 00 56 8B F1"),
    "PublishProj": bytes.fromhex("55 8B EC 83 E4 F0 81 EC 88 00 00 00"),
    "helper31940": bytes.fromhex("8B 54 24 04 56 8B 02 8B F1 56 89 06"),
}


def main() -> None:
    import struct

    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name == b".text":
            _, text_va, text_rsize, text_raw = struct.unpack_from("<IIII", data, off + 8)
            break
    chunk = data[text_raw : text_raw + text_rsize]
    for name, pat in PATTERNS.items():
        hits = []
        pos = 0
        while True:
            j = chunk.find(pat, pos)
            if j < 0:
                break
            hits.append(text_va + j)
            pos = j + 1
        print(f"{name}: {len(hits)} hit(s) " + " ".join(f"0x{h:X}" for h in hits[:5]))


if __name__ == "__main__":
    main()
