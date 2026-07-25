#!/usr/bin/env python3
"""Locate writers of ReplayDispatch early-out gate [0x17ED918]."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
ABS = 0x17ED918
SKEW = 0xC00


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
    imm = struct.pack("<I", ABS)
    pos = 0
    print("WRITE-like refs:")
    while True:
        j = chunk.find(imm, pos)
        if j < 0:
            break
        rva = text_va + j
        # look back up to 3 bytes for store opcodes
        for back in range(1, 4):
            op = chunk[j - back : j]
            kind = None
            if op.endswith(b"\xA3"):
                kind = "mov [abs],eax"
            elif op == b"\x89\x0D":
                kind = "mov [abs],ecx"
            elif op == b"\x89\x15":
                kind = "mov [abs],edx"
            elif op == b"\x89\x35":
                kind = "mov [abs],esi"
            elif op == b"\xC7\x05":
                kind = "mov [abs],imm32"
            if kind:
                print(f"  0x{rva - back:X} {kind}")
                # show imm32 if c7
                if kind.startswith("mov [abs],imm"):
                    imm32 = struct.unpack_from("<I", chunk, j + 4)[0]
                    print(f"      imm=0x{imm32:X}")
                break
        pos = j + 1


if __name__ == "__main__":
    main()
