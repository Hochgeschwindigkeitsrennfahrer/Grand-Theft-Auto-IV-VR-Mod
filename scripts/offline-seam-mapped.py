#!/usr/bin/env python3
"""Mapped-RVA seam scan for GTA IV CE — uses PE section VA→file mapping.

All addresses here are LOADED-module RVAs (file_offset + 0xC00 for .text/.rdata).
Do not confuse with older file-offset docs (PublishSync was mislabeled 0x30300).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IMAGE_BASE = 0x400000

Section = tuple[int, int, int, int]
_SECTIONS: list[Section] = []


def load_sections(data: bytes) -> list[Section]:
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec = e_lfanew + 24 + opt
    out: list[Section] = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        out.append((va, vsz, raw, rsz))
    return out


def rva_to_fo(rva: int) -> int | None:
    for va, vsz, raw, rsz in _SECTIONS:
        if va <= rva < va + max(vsz, rsz):
            return raw + (rva - va)
    return None


def fo_to_rva(fo: int) -> int | None:
    for va, vsz, raw, rsz in _SECTIONS:
        if raw <= fo < raw + rsz:
            return va + (fo - raw)
    return None


def bytes_rva(data: bytes, rva: int, n: int = 16) -> str:
    fo = rva_to_fo(rva)
    if fo is None:
        return "MISS"
    return " ".join(f"{b:02X}" for b in data[fo : fo + n])


def code_at(data: bytes, rva: int, n: int) -> bytes | None:
    fo = rva_to_fo(rva)
    if fo is None:
        return None
    return data[fo : fo + n]


def find_fn_start(data: bytes, rva_in: int, max_back: int = 0x800) -> int | None:
    """Nearest prologue walking backward; stop at CC-pad / ret so we don't jump fns."""
    best: int | None = None
    for delta in range(0, max_back + 1):
        rva = rva_in - delta
        if rva < 0:
            break
        chunk = code_at(data, rva, 7)
        if chunk is None or len(chunk) < 3:
            continue
        # Crossing a prior ret + CC pad means we left this function.
        if delta > 0 and chunk[0] == 0xC3:
            break
        if delta > 1 and chunk[0] == 0xCC and chunk[1] == 0xCC:
            break
        if chunk[0] == 0x55 and chunk[1] == 0x8B and chunk[2] == 0xEC:
            return rva
        # ReplayDispatch-style: cmp [imm32], 0 ; push esi ; mov esi,ecx
        if (
            chunk[0] == 0x83
            and chunk[1] == 0x3D
            and len(chunk) >= 7
            and chunk[6] == 0x00
        ):
            best = rva
            continue  # keep walking for a nearer frame if any
        if best is None:
            if chunk[0] == 0x56 and len(chunk) >= 4 and chunk[1] == 0x57 and chunk[2] == 0x8B:
                best = rva
            elif chunk[0] == 0x83 and chunk[1] == 0xEC:
                best = rva
            elif chunk[0] == 0x56 and chunk[1] == 0x8B and chunk[2] == 0xF1:
                best = rva
    return best


def scan_e8_to(data: bytes, target_rva: int, limit: int = 64) -> list[int]:
    """Return mapped RVAs of E8 call sites targeting target_rva."""
    target_va = IMAGE_BASE + target_rva
    hits: list[int] = []
    # Scan .text file range only
    text = _SECTIONS[0]
    va, vsz, raw, rsz = text
    for fo in range(raw, raw + rsz - 5):
        if data[fo] != 0xE8:
            continue
        site_rva = fo_to_rva(fo)
        if site_rva is None:
            continue
        rel = struct.unpack_from("<i", data, fo + 1)[0]
        dst = IMAGE_BASE + site_rva + 5 + rel
        if dst == target_va:
            hits.append(site_rva)
            if len(hits) >= limit:
                break
    return hits


def scan_e8_from(data: bytes, fn_rva: int, fn_len: int) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for rva in range(fn_rva, fn_rva + fn_len - 4):
        chunk = code_at(data, rva, 5)
        if chunk is None or chunk[0] != 0xE8:
            continue
        rel = struct.unpack_from("<i", chunk, 1)[0]
        dst = rva + 5 + rel
        out.append((rva, dst & 0xFFFFFFFF))
    return out


def print_capstone(data: bytes, start: int, max_bytes: int, title: str) -> None:
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        print(f"=== {title} @ mapped 0x{start:X} ===")
        print(f"  prologue: {bytes_rva(data, start, 16)}")
        print("  (pip install capstone for full disasm)")
        print()
        return

    fo = rva_to_fo(start)
    if fo is None:
        print(f"=== {title} @ mapped 0x{start:X} MISS ===\n")
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print(f"=== {title} @ mapped 0x{start:X} (file 0x{fo:X}) ===")
    print(f"  prologue: {bytes_rva(data, start, 16)}")
    code = data[fo : fo + max_bytes]
    n = 0
    end = start
    marks = {
        0x30D0D: "  ; call [eax+0x178]  Mode42 OWNER-EDGE ret follows @0x30D13",
        0x30D13: "  ; ret after slot178 call",
        0x30D20: "  ; call [ecx+0x1B4]",
        0x30F49: "  ; cmp [activeView], esi",
        0x30F53: "  ; call 0x30CD0 (PublishSync -> ReplayDispatch)",
        0x30C92: "  ; call 0x30CD0 (SetActiveView -> ReplayDispatch)",
        0x32470: "  ; ViewConst TRUE start (Mode 64)",
        0x3247C: "  ; ViewConst mid (unsafe hook)",
    }
    for insn in md.disasm(code, IMAGE_BASE + start):
        rva_off = insn.address - IMAGE_BASE
        mark = marks.get(rva_off, "")
        print(f"  0x{rva_off:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        n += 1
        end = rva_off + insn.size
        if insn.mnemonic == "ret":
            break
        if n > 200:
            print("  ... truncated")
            break
    print(f"  insns={n} fn_len~0x{end - start:X}")
    print()


def scan_ff90_178(data: bytes, lo: int, hi: int) -> list[int]:
    hits: list[int] = []
    needle = b"\x78\x01\x00\x00"
    for rva in range(lo, hi - 5):
        chunk = code_at(data, rva, 6)
        if chunk and chunk[:2] == b"\xFF\x90" and chunk[2:6] == needle:
            hits.append(rva)
    return hits


def main() -> int:
    global _SECTIONS
    if not EXE.is_file():
        print(f"MISSING: {EXE}")
        return 1

    data = EXE.read_bytes()
    _SECTIONS = load_sections(data)
    text = _SECTIONS[0]
    delta = text[0] - text[2]
    print(f"GTAIV.exe size={len(data)}")
    print(f".text VA=0x{text[0]:X} Raw=0x{text[2]:X}  mappedRVA = fileOff + 0x{delta:X}")
    print()

    sites = [
        (0x30F00, "PublishSync Mode66", "55 8B EC 83 E4 F8 51 56 8B F1"),
        (0x314C0, "ViewMatWriter Mode67", "55 8B EC 83 E4 F8 83 EC 10"),
        (0x32470, "ViewConst TRUE Mode64", "55 8B EC 83 E4 F0 81 EC"),
        (0x3247C, "ViewConst MID unsafe", "56 57 8B F9 8D 44 24 70"),
        (0x30CD0, "ReplayDispatch", "83 3D"),
        (0x30D0D, "call [eax+0x178]", "FF 90 78 01 00 00"),
        (0x30D13, "ret after slot178", None),
        (0x30BF0, "SetActiveView", "83 3D"),
        (0x2D33E, "VsRet", "85 C0 75 14"),
        (0x2D2AC, "VS wrap FORBIDDEN", "89 51 0A"),
        (0x3259A, "Mode41 mid-ret (was file 0x3199A)", None),
        (0x325A4, "Mode41 mid-ret (was file 0x319A4)", None),
    ]
    print("=== Site byte verify (mapped) ===")
    for rva, name, expect in sites:
        b = bytes_rva(data, rva, 12)
        fo = rva_to_fo(rva)
        ok = ""
        if expect:
            got = b.upper()
            exp = expect.upper()
            ok = " OK" if got.startswith(exp) else " MISMATCH"
        print(f"  0x{rva:06X} file=0x{fo:X}  {name:40s} {b}{ok}")
    print()

    print("=== call [eax+0x178] sites near replay (0x30000–0x33000) ===")
    for hit in scan_ff90_178(data, 0x30000, 0x33000):
        fn = find_fn_start(data, hit)
        print(f"  0x{hit:X} enclosing~0x{fn:X}" if fn else f"  0x{hit:X}")
    print()

    print_capstone(data, 0x30CD0, 0x80, "ReplayDispatch 0x30CD0 (no static E8 — indirect only)")
    print_capstone(data, 0x30F00, 0x80, "PublishSync 0x30F00")
    print_capstone(data, 0x30BF0, 0xC0, "SetActiveView 0x30BF0")
    print_capstone(data, 0x32470, 0x200, "ViewConst TRUE start 0x32470")
    print_capstone(data, 0x314C0, 0x180, "ViewMatWriter 0x314C0")

    print("=== Static E8 caller counts (mapped targets) ===")
    for tgt, name in [
        (0x30F00, "PublishSync"),
        (0x314C0, "ViewMatWriter"),
        (0x32470, "ViewConstTRUE"),
        (0x30CD0, "ReplayDispatch"),
        (0x30BF0, "SetActiveView"),
        (0x307F0, "MatMul"),
    ]:
        hits = scan_e8_to(data, tgt, 64)
        print(f"  -> {name} 0x{tgt:X}: {len(hits)} E8")
        for site in hits[:8]:
            fn = find_fn_start(data, site)
            fn_s = f"0x{fn:X}" if fn else "?"
            print(f"      call@0x{site:X} enclosing~{fn_s}")
        if len(hits) > 8:
            print(f"      ... +{len(hits) - 8} more")
    print()

    print("=== E8 callees from PublishSync / SetActiveView / ViewConst ===")
    for fn, span, name in [
        (0x30F00, 0x70, "PublishSync"),
        (0x30BF0, 0xC0, "SetActiveView"),
        (0x32470, 0x400, "ViewConst"),
        (0x314C0, 0x180, "ViewMatWriter"),
    ]:
        callees = scan_e8_from(data, fn, span)
        print(f"  {name} 0x{fn:X}: {len(callees)} E8")
        for site, dst in callees:
            mark = ""
            if dst == 0x30CD0:
                mark = "  ; ReplayDispatch"
            if dst == 0x30F00:
                mark = "  ; PublishSync"
            if dst == 0x307BF0:
                mark = "  ; MatMul"
            print(f"      0x{site:X} -> 0x{dst:X}{mark}")
    print()

    # Mode41/42 ret remap note
    print("=== Mode 41/42 ret remaps (file-off docs → mapped) ===")
    for old, new, note in [
        (0x3199A, 0x3259A, "ViewConst mid-ret family"),
        (0x319A4, 0x325A4, "ViewConst mid-ret family"),
        (0x30113, 0x30D13, "ret after call [eax+0x178] — Mode42 already uses 0x30D13"),
        (0x309D0, 0x315D0, "old indirect-edge note"),
        (0x308C9, 0x314C9, "old ViewMat mid mislabel"),
    ]:
        print(f"  file-off 0x{old:X} -> mapped 0x{new:X}  {note}  bytes={bytes_rva(data, new, 8)}")
    print()

    print("=== Safe next seams (COUNT-only candidates) ===")
    print("  1. Mode 66 @ 0x30F00 PublishSync — AOB armed (await headset)")
    print("  2. Mode 64 @ 0x32470 ViewConst TRUE — AOB + CC-pad")
    print("  3. Mode 67 @ 0x314C0 ViewMatWriter — AOB + CC-pad")
    print("  4. Mode 65 VT178 peek — no hook; compare slot178 with live @0x30D0D")
    print("  5. DO NOT hook 0x30CD0 / 0x30D0D yet — SameFrameSeamGate=CLOSED")
    print("  6. Mode 41 live filter should prefer mapped mid-rets 0x3259A/0x325A4")
    print()
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
