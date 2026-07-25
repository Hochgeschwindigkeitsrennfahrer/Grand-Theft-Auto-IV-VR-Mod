#!/usr/bin/env python3
"""Map writers vs readers in ViewConst gate neighborhood 0x1797654..0x17976D4."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
BASE = 0x1797654
END = 0x17976D8


def classify(prev: bytes) -> str:
    h = prev.hex(" ")
    # common x86 patterns ending with opcode before imm32
    if prev.endswith(b"\x3D") or prev.endswith(b"\x80\x3D") or prev.endswith(b"\x83\x3D"):
        return "cmp"
    if prev.endswith(b"\xA1") or prev.endswith(b"\x8B\x0D") or prev.endswith(b"\x8B\x15") \
       or prev.endswith(b"\x8B\x1D") or prev.endswith(b"\x8B\x35") or prev.endswith(b"\x8B\x3D"):
        return "read"
    if prev.endswith(b"\xA3") or prev.endswith(b"\x89\x0D") or prev.endswith(b"\x89\x15") \
       or prev.endswith(b"\x89\x1D") or prev.endswith(b"\x89\x35") or prev.endswith(b"\x89\x3D") \
       or prev.endswith(b"\xC7\x05"):
        return "WRITE"
    if prev.endswith(b"\xFF\x05") or prev.endswith(b"\xFF\x0D"):
        return "WRITE"
    if prev.endswith(b"\x68"):
        return "push"
    return f"?({h})"


def main() -> None:
    data = EXE.read_bytes()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    num_sec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec0 = pe_off + 24 + opt_size
    # use .text only
    text_raw = text_va = text_rsize = None
    for i in range(num_sec):
        off = sec0 + i * 40
        name = data[off : off + 8].split(b"\0", 1)[0]
        if name == b".text":
            _, text_va, text_rsize, text_raw = struct.unpack_from("<IIII", data, off + 8)
            break
    assert text_raw is not None
    chunk = data[text_raw : text_raw + text_rsize]
    for absva in range(BASE, END, 4):
        imm = struct.pack("<I", absva)
        pos = 0
        hits = []
        while True:
            j = chunk.find(imm, pos)
            if j < 0:
                break
            rva = text_va + j
            prev = chunk[max(0, j - 2) : j]
            hits.append((rva, classify(prev), prev.hex(" ")))
            pos = j + 1
        if not hits:
            continue
        kinds = ",".join(sorted({k for _, k, _ in hits}))
        print(f"[0x{absva:X}] n={len(hits)} kinds={kinds}")
        for rva, kind, prev in hits:
            if kind == "WRITE" or absva == 0x1797694:
                print(f"    {kind:5} @0x{rva:X} prev={prev}")


if __name__ == "__main__":
    main()
