#!/usr/bin/env python3
"""Map SetActiveView 0x30BF0 callers + what follows (ReplayDispatch?)."""
from __future__ import annotations

import struct
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
TARGET = 0x30BF0
REPLAY = 0x30CD0
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
        if call_rva + 5 + rel == TARGET:
            callers.append(call_rva)
    print(f"SetActiveView callers: {len(callers)}")
    for c in callers:
        fo = c - text_va + text_raw
        # next 32 bytes: any call ReplayDispatch?
        nxt = []
        for j in range(0, 40):
            if data[fo + 5 + j] != 0xE8:
                continue
            rel = struct.unpack_from("<i", data, fo + 5 + j + 1)[0]
            tgt = c + 5 + j + 5 + rel
            nxt.append((c + 5 + j, tgt))
        replay_near = any(t == REPLAY for _, t in nxt[:6])
        print(f"  0x{c:X}  replayNear={replay_near}  nextE8="
              + ",".join(f"0x{a:X}->0x{t:X}" for a, t in nxt[:4]))


if __name__ == "__main__":
    main()
