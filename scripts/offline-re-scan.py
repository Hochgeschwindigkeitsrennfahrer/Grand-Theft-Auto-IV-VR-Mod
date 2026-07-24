#!/usr/bin/env python3
"""Offline PE scan for GTA IV CE (GTAIV.exe). Read-only; no game launch."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IMAGE_BASE = 0x400000


def parse_pe(data: bytes) -> tuple[int, int, int]:
    if data[:2] != b"MZ":
        raise SystemExit("Not PE")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise SystemExit("Bad PE signature")
    # Optional header magic at e_lfanew+24
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise SystemExit(f"Expected PE32, got 0x{magic:X}")
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    return e_lfanew, size_of_image, opt


def parse_pattern(pat: str) -> list[int | None]:
    out: list[int | None] = []
    parts = pat.split()
    for p in parts:
        if p == "?" or p == "??":
            out.append(None)
        else:
            out.append(int(p, 16))
    return out


def find_all(data: bytes, pattern: str, limit: int = 32) -> list[int]:
    pat = parse_pattern(pattern)
    n = len(pat)
    hits: list[int] = []
    for i in range(len(data) - n + 1):
        ok = True
        for j, b in enumerate(pat):
            if b is not None and data[i + j] != b:
                ok = False
                break
        if ok:
            hits.append(i)
            if len(hits) >= limit:
                break
    return hits


def rva(off: int) -> int:
    return IMAGE_BASE + off


def fmt_rva(off: int) -> str:
    return f"0x{off:X} (file+0x{off:X})"


def read_cstr(data: bytes, off: int, maxlen: int = 120) -> str:
    end = data.find(b"\x00", off, off + maxlen)
    if end < 0:
        end = off + maxlen
    try:
        return data[off:end].decode("ascii", errors="replace")
    except Exception:
        return ""


def find_string_refs(data: bytes, needle: bytes, limit: int = 8) -> list[int]:
    """Find file offsets where needle appears (RTTI / debug strings)."""
    hits: list[int] = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        if len(hits) >= limit:
            break
        start = i + 1
    return hits


def scan_e8_to(data: bytes, target_rva: int, image_size: int) -> list[tuple[int, int]]:
    """Find E8 rel32 calls whose target RVA == target_rva."""
    target_va = IMAGE_BASE + target_rva
    hits: list[tuple[int, int]] = []
    for off in range(image_size - 5):
        if data[off] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, off + 1)[0]
        dst = rva(off) + 5 + rel
        if dst == target_va:
            hits.append((off, rva(off)))
    return hits


def bytes_at(data: bytes, off: int, n: int = 16) -> str:
    chunk = data[off : off + n]
    return " ".join(f"{b:02X}" for b in chunk)


def find_fn_start(data: bytes, addr_in_fn: int, max_back: int = 0x800) -> int | None:
    """Heuristic: walk back for common prologues within same section."""
    lo = max(0, addr_in_fn - max_back)
    best = None
    for i in range(addr_in_fn, lo, -1):
        b0, b1, b2, b3 = data[i], data[i + 1] if i + 1 < len(data) else 0, data[i + 2] if i + 2 < len(data) else 0, data[i + 3] if i + 3 < len(data) else 0
        # push ebx; push ebp; push esi; push edi / mov edi,ecx / sub esp
        if b0 == 0x55 and b1 == 0x8B and b2 == 0xEC:
            best = i
            break
        if b0 == 0x56 and b1 == 0x57 and b2 == 0x8B and b3 == 0xF9:
            best = i
            break
        if b0 == 0x53 and b1 == 0x55 and b2 == 0x56 and b3 == 0x57:
            best = i
            break
        if b0 == 0x83 and b1 == 0xEC:
            best = i
            break
        if b0 == 0x56 and b1 == 0x8B and b2 == 0xF1:
            best = i
            break
    return best


def main() -> int:
    if not EXE.is_file():
        print(f"MISSING: {EXE}")
        return 1

    data = EXE.read_bytes()
    _, image_size, _ = parse_pe(data)
    print(f"GTAIV.exe size={len(data)} image_size=0x{image_size:X} base=0x{IMAGE_BASE:X}")
    print()

    # --- Known RVAs from project docs ---
    known = {
        0x2C73E: ("VsRet", "SetVSConstF upload return — replay thread anchor"),
        0x2C6AC: ("VS wrap", "FORBIDDEN — Mode 28 crash"),
        0x37BD0: ("VsParent wrong ABI", "FORBIDDEN — Mode 27/29 crash"),
        0x1BF010: ("thiscall-frame COUNT", "FORBIDDEN — Mode 34 hard-kill"),
        0x4DDAD0: ("Mode34 dual", "FORBIDDEN — vsPatch=0"),
        0x309D0: ("indirect edge", "Mode 41/42 observation — FF/2 call [eax+disp]"),
        0x8F7F00: ("BuildRootA", "Game-thread 13-phase build root"),
        0x8F73B0: ("ExecRoot dispatch", "Exec dispatcher vt[9]"),
        0xA87680: ("BuildExecCD loop", "PhaseC+DrawScene build+exec"),
        0x527ED1: ("BuildRenderList DrawScene", "Mid AOB start ~0x527EDE-13"),
        0x527EC0: ("PhaseA start", "BuildRenderList PhaseA"),
        0x975D50: ("PhaseC start", "BuildRenderList PhaseC (approx)"),
        0x6DC600: ("DrawScene fn start", "From CURRENT-STATE table"),
    }

    print("=== Known RVA byte verify (first 8 bytes at file offset) ===")
    for rva_off, (tag, note) in sorted(known.items()):
        fo = rva_off  # CE 1.2.0.59: file offset == RVA for .text (typical)
        if fo + 8 > len(data):
            print(f"  0x{rva_off:X} {tag}: OUT OF RANGE")
            continue
        print(f"  0x{rva_off:06X} {tag:22s} {bytes_at(data, fo, 12)}  | {note}")
    print()

    # --- AOB patterns from our ASI ---
    patterns = {
        "CopyMat onfoot_front": "E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6",
        "CopyMat onfoot_behind": "E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74",
        "CopyMat vehicle_front": "E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24",
        "CopyMat vehicle_behind": "E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00",
        "FindPlayerPed": "8B 44 24 04 85 C0 75 18 A1",
        "FovSite CE": "E8 ? ? ? ? F6 87 ? ? ? ? ? 5B",
        "FovSite classic": "E8 ? ? ? ? 8B CE E8 ? ? ? ? F6 86 ? ? ? ? ? 5F",
        "BuildRenderList DrawScene mid": "83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C",
        "BuildRenderList PhaseA mid": "83 BF 38 09 00 00 FF 0F 84 76 03 00 00 80 3D",
        "BuildRenderList PhaseC mid": "83 BF 38 09 00 00 FF 0F 84 77 02 00 00 8D 8F B0 00 00 00",
        "PedHide SetDraw": "E8 ? ? ? ? 83 C4 04 85 C0 74 ? 8B 0D",
    }

    print("=== AOB scan (first hit) ===")
    for name, pat in patterns.items():
        hits = find_all(data, pat, 4)
        if not hits:
            print(f"  MISS  {name}")
            continue
        for h in hits[:3]:
            call_rva = None
            if data[h] == 0xE8:
                rel = struct.unpack_from("<i", data, h + 1)[0]
                call_rva = rva(h) + 5 + rel
            extra = f" E8->0x{call_rva - IMAGE_BASE:X}" if call_rva else ""
            print(f"  0x{h:06X}  {name}{extra}  [{bytes_at(data, h, 14)}]")
    print()

    # --- Static E8 -> VsRet ---
    print("=== Static E8 callers -> VsRet (0x2C73E) ===")
    e8_hits = scan_e8_to(data, 0x2C73E, min(image_size, len(data)))
    print(f"  direct E8 call sites: {len(e8_hits)}")
    for off, site_rva in e8_hits[:20]:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"    call@0x{off:X} site=0x{site_rva - IMAGE_BASE:X} enclosing~{fn_s}  {bytes_at(data, off - 8, 20)}")
    print()

    # --- VsRet chain mids from Mode 41 log ---
    chain_mids = [
        (0x22187, "slot10 epilogue"),
        (0x37C01, "slot15 -> 0x37BD0"),
        (0x37520, "slot22 stackarg"),
        (0x32BD7, "slot27 stackarg"),
        (0x30D13, "0x309D0 indirect"),
        (0x3199A, "direct call unaligned"),
        (0x319A4, "direct call unaligned"),
        (0x2C6A0, "slot10 callee"),
        (0x372B0, "forbidden stackarg"),
        (0x32A40, "forbidden stackarg"),
    ]
    print("=== VsRet chain mid-RVAs (bytes + enclosing start guess) ===")
    for mid, note in chain_mids:
        if mid >= len(data):
            continue
        fn = find_fn_start(data, mid)
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"  0x{mid:06X} start~{fn_s:8s}  {bytes_at(data, mid, 8)}  | {note}")
    print()

    # --- RTTI / render phase strings ---
    rtti_needles = [
        b"CRenderPhaseDrawScene",
        b"CRenderPhaseDeferredLighting_SceneToGBuffer",
        b"CCam",
        b"SetDrawPlayerComponent",
        b"BuildRenderList",
    ]
    print("=== Interesting strings (file offset) ===")
    for needle in rtti_needles:
        refs = find_string_refs(data, needle, 3)
        if refs:
            for ref in refs:
                s = read_cstr(data, ref, 80)
                print(f"  +0x{ref:X}: {s[:70]}")
        else:
            print(f"  (none) {needle.decode()}")
    print()

    # --- Global render manager mov ecx imm32 near BuildRootA caller ---
    print("=== BuildRootA caller wrapper (mov ecx, imm32 before E8) ===")
    br_off = 0x8F7F00
    # scan backwards from 0x5C2BF0 area documented
    for probe in [0x5C2BF0, 0x5C2380]:
        if probe < len(data):
            print(f"  0x{probe:X}: {bytes_at(data, probe, 24)}")
    # find mov ecx, 0x118D7F0
    imm = struct.pack("<I", 0x118D7F0)
    pat = b"\xB9" + imm
    mov_hits = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        mov_hits.append(i)
        start = i + 1
    print(f"  mov ecx, 0x118D7F0 sites: {len(mov_hits)}")
    for h in mov_hits[:8]:
        print(f"    0x{h:X}: {bytes_at(data, h, 16)}")
    print()

    # --- 0x309D0: scan for FF /2 indirect near 0x30D0D ---
    print("=== 0x309D0 region (indirect call edge) ===")
    region = 0x309D0
    print(f"  prologue@0x{region:X}: {bytes_at(data, region, 32)}")
    for off in range(0x30D00, 0x30D20):
        if off + 6 <= len(data) and data[off] == 0xFF and data[off + 1] in (0x10, 0x11, 0x12, 0x50, 0x51, 0x52, 0x90, 0x91, 0x92):
            print(f"    FF/{data[off+1]:02X} @0x{off:X}: {bytes_at(data, off, 8)}")
    print()

    # --- CCam+0x60 FOV: find movss/addss near FovSite if found ---
    fov_hits = find_all(data, patterns["FovSite CE"], 1)
    if fov_hits:
        site = fov_hits[0]
        rel = struct.unpack_from("<i", data, site + 1)[0]
        tgt = rva(site) + 5 + rel
        print(f"=== FovSite CALL target ===")
        print(f"  hook site 0x{site:X} -> callee 0x{tgt - IMAGE_BASE:X}")
        print(f"  callee bytes: {bytes_at(data, tgt - IMAGE_BASE, 24)}")
    print()

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
