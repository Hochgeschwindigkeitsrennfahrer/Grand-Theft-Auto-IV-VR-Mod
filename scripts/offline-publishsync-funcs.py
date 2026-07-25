#!/usr/bin/env python3
"""Classify each E8->PublishSync site: inside helper vs external + nearest prologue."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
PUBLISH = 0x30F00
PROJ = 0x31BA0
HELPER = 0x31940
VIEWMAT = 0x314C0
VIEWCONST = 0x32470
SKEW = 0xC00


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

    callers = []
    for fo in range(text_raw, text_raw + text_rsize - 5):
        if data[fo] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        call_rva = fo - text_raw + text_va
        if call_rva + 5 + rel == PUBLISH:
            callers.append(call_rva)

    known = {
        VIEWMAT: "ViewMatWriter",
        VIEWCONST: "ViewConstTRUE",
        HELPER: "helper31940",
        0x319B0: "fn319B0",
        0x219070: "fn219070",
    }

    print(f"E8 -> PublishSync: {len(callers)}")
    for c in callers:
        fo = c - text_va + text_raw
        has_proj = False
        for j in range(0, 24):
            if fo + 5 + j + 5 > len(data):
                break
            if data[fo + 5 + j] != 0xE8:
                continue
            rel = struct.unpack_from("<i", data, fo + 5 + j + 1)[0]
            if c + 5 + j + 5 + rel == PROJ:
                has_proj = True
                break
        # nearest known / nearest 55 8B EC with CC pad or after ret
        nearest_known = None
        for kr in known:
            if kr <= c < kr + 0x200:
                if nearest_known is None or kr > nearest_known:
                    nearest_known = kr
        # walk back for prologue after int3 pad
        fn = None
        for back in range(0, 0x180):
            r = c - back
            o = r - text_va + text_raw
            if data[o : o + 3] != b"\x55\x8B\xEC":
                continue
            # prefer if previous byte is CC or function start after ret
            prev = data[o - 1] if o > text_raw else 0
            if prev in (0xCC, 0xC3) or data[o - 3 : o] == b"\xC2\x04\x00" or back < 0x40:
                fn = r
                break
            if fn is None:
                fn = r
        label = known.get(nearest_known or -1, "")
        if HELPER <= c < HELPER + 0x70:
            label = "INSIDE helper 0x31940"
        tag = "+Proj" if has_proj else "ONLY"
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"  0x{c:X}  {tag:5}  fn~{fn_s}  {label}")


if __name__ == "__main__":
    main()
