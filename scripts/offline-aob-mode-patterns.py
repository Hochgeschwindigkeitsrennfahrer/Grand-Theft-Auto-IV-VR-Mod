#!/usr/bin/env python3
"""Uniqueness of exact Mode 64/66/67 IDA patterns in CE .text."""
from __future__ import annotations

import re
import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")

PATTERNS = {
    "Mode66 PublishSync":
        "55 8B EC 83 E4 F8 51 56 8B F1 8D 86 80 00 00 00 50 8D 86 80 01 00 00 50 8D 8E C0 00 00 00 E8",
    "Mode67 ViewMat":
        "55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9 0F 57 C9 F3 0F 10 87 BC 02 00 00",
    "Mode64 ViewConst":
        "55 8B EC 83 E4 F0 81 EC A8 00 00 00 56 57 8B F9 8D 44 24 70 F3 0F 10 87",
}


def pat_to_regex(pat: str) -> bytes:
    parts = []
    for tok in pat.split():
        if tok == "?":
            parts.append(b".")
        else:
            parts.append(re.escape(bytes.fromhex(tok)))
    return b"".join(parts)


def main() -> None:
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
        rx = re.compile(pat_to_regex(pat), re.DOTALL)
        hits = [text_va + m.start() for m in rx.finditer(chunk)]
        print(f"{name}: {len(hits)} " + " ".join(f"0x{h:X}" for h in hits))


if __name__ == "__main__":
    main()
