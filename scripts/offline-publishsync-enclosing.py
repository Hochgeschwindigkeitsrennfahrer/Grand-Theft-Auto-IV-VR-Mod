#!/usr/bin/env python3
"""For each E8->PublishSync, find enclosing push-ebp frame and whether PublishProj follows."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
PUBLISH = 0x30F00
PROJ = 0x31BA0


def main() -> None:
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

    def rva_of(fo: int) -> int:
        return fo - text_raw + text_va

    callers = []
    for fo in range(text_raw, text_raw + text_rsize - 5):
        if data[fo] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        call_rva = rva_of(fo)
        if call_rva + 5 + rel == PUBLISH:
            callers.append(call_rva)

    print(f"PublishSync callers: {len(callers)}")
    for c in callers:
        fo = c - text_va + text_raw
        # look ahead 24 bytes for PublishProj
        has_proj = False
        for j in range(0, 24):
            if data[fo + 5 + j] != 0xE8:
                continue
            rel = struct.unpack_from("<i", data, fo + 5 + j + 1)[0]
            if c + 5 + j + 5 + rel == PROJ:
                has_proj = True
                break
        fn = None
        for back in range(0, 0x400):
            r = c - back
            o = r - text_va + text_raw
            if o < text_raw:
                break
            if data[o : o + 3] == b"\x55\x8B\xEC":
                fn = r
                break
        tag = " +PublishProj" if has_proj else " PublishSync-ONLY"
        print(f"  call@0x{c:X}  fn~0x{fn:X}{tag}" if fn else f"  call@0x{c:X}{tag}")


if __name__ == "__main__":
    main()
