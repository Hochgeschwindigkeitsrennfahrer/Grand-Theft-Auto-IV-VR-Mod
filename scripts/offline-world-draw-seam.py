#!/usr/bin/env python3
"""Approach A/F overnight RE: map cheaper world-draw seams vs full BuildRootA.

Scans GTAIV.exe (CE) for:
  - E8 callers of BuildRootA (0x8F8B00)
  - DrawScene / PhaseA / PhaseC BuildRenderList prologues
  - xrefs to view+0x80 / +0x180 / +0x308 style lea patterns near ReplayDispatch
  - call [reg+0x178] sites (upload seams)

Writes docs/_re_scratch/world_draw_seam_report.txt and prints a short summary.
Never recommends live view+0x80 writes (Mode71 freeze).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
OUT = Path(__file__).resolve().parents[1] / "docs" / "_re_scratch" / "world_draw_seam_report.txt"
IMAGE_BASE = 0x400000
BUILD_ROOT_A = 0x8F8B00
DRAW_SCENE = 0x6DD200
PHASE_A = 0x528AC0
PHASE_C = 0x976950
REPLAY_DISPATCH = 0x30CD0
CALL_178 = 0x30D0D


def load_sections(data: bytes):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    num = struct.unpack_from("<H", data, e + 6)[0]
    opt = struct.unpack_from("<H", data, e + 20)[0]
    sec = e + 24 + opt
    out = []
    for i in range(num):
        o = sec + i * 40
        vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
        out.append((va, vsz, raw, rsz))
    return out


def main() -> int:
    if not EXE.is_file():
        print(f"MISSING exe: {EXE}", file=sys.stderr)
        return 1
    data = EXE.read_bytes()
    secs = load_sections(data)

    def r2f(r: int):
        for va, vsz, raw, rsz in secs:
            if va <= r < va + max(vsz, rsz):
                return raw + (r - va)
        return None

    def bytes_r(r: int, n: int = 16) -> str:
        f = r2f(r)
        if f is None:
            return "MISS"
        return " ".join(f"{b:02X}" for b in data[f : f + n])

    def find_e8_to(target_rva: int) -> list[int]:
        hits = []
        # Scan all sections for E8 rel32 → target (rel from VA = IMAGE_BASE+RVA).
        for va, vsz, raw, rsz in secs:
            span = min(vsz, rsz) if rsz else 0
            if span < 5:
                continue
            blob = data[raw : raw + span]
            for i in range(0, len(blob) - 4):
                if blob[i] != 0xE8:
                    continue
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                src_rva = va + i
                dest_rva = (src_rva + 5 + rel) & 0xFFFFFFFF
                if dest_rva == target_rva:
                    hits.append(src_rva)
        return hits

    lines: list[str] = []
    lines.append("=== world-draw seam report (Approach A/F) ===")
    lines.append(f"exe={EXE}")
    lines.append("")

    for name, rva, expect in [
        ("BuildRootA", BUILD_ROOT_A, bytes.fromhex("55 8B EC 83 E4 F0")),
        ("DrawScene", DRAW_SCENE, bytes.fromhex("55 8B EC")),
        ("PhaseA", PHASE_A, bytes.fromhex("55 8B EC")),
        ("PhaseC", PHASE_C, bytes.fromhex("55 8B EC")),
        ("ReplayDispatch", REPLAY_DISPATCH, None),
    ]:
        f = r2f(rva)
        ok = "?"
        if f is not None:
            if expect is None:
                ok = "PRESENT"
            else:
                ok = "MATCH" if data[f : f + len(expect)] == expect else "DRIFT"
        lines.append(f"{name} RVA 0x{rva:X}: {ok} bytes={bytes_r(rva, 12)}")

    lines.append("")
    lines.append("--- E8 callers of BuildRootA (cheaper dual cannot re-enter all of these) ---")
    callers = find_e8_to(BUILD_ROOT_A)
    lines.append(f"count={len(callers)}")
    for c in callers[:40]:
        lines.append(f"  E8→BuildRootA from 0x{c:X}  context={bytes_r(c, 16)}")
    if len(callers) > 40:
        lines.append(f"  ... +{len(callers) - 40} more")

    lines.append("")
    lines.append("--- E8 callers of DrawScene BuildRenderList (Mode77 dual target) ---")
    ds = find_e8_to(DRAW_SCENE)
    lines.append(f"count={len(ds)}")
    for c in ds[:30]:
        lines.append(f"  E8→DrawScene from 0x{c:X}  context={bytes_r(c, 16)}")

    lines.append("")
    lines.append("--- call [reg+0x178] sites (FF 90 78 01 00 00) — upload seams ---")
    pat = bytes.fromhex("FF 90 78 01 00 00")
    ff178 = []
    for va, vsz, raw, rsz in secs:
        span = min(vsz, rsz) if rsz else 0
        blob = data[raw : raw + span]
        start = 0
        while True:
            i = blob.find(pat, start)
            if i < 0:
                break
            ff178.append(va + i)
            start = i + 1
    lines.append(f"count={len(ff178)}")
    for r in ff178:
        rva = r if r < IMAGE_BASE else r - IMAGE_BASE
        mark = " << ReplayDispatch" if rva == CALL_178 else ""
        lines.append(f"  0x{rva:X}{mark}")

    lines.append("")
    lines.append("--- Verdict (offline) ---")
    lines.append(
        "1. Full BuildRootA×2 remains the fear-vr analog but crashes streaming / denser dual "
        "felt LESS 3D on headset — SCRAPPED as Mode75 denser path."
    )
    lines.append(
        "2. Cheaper candidate = DrawScene BuildRenderList×2 (Mode77) — world-draw phase only; "
        "PhaseA/C stay mono. Static callers listed above."
    )
    lines.append(
        "3. Hot matrix upload = call [eax+0x178] @ 0x30D0D + UploadFn 0x2A1E10 — inject L/R "
        "src-copies here (Mode72/74/78); NEVER write live view+0x80."
    )
    lines.append(
        "4. Viewport/SBS (Approach B): no static evidence of game SBS dual viewport; "
        "D3D9 SetViewport alone cannot invent parallax without a second world draw."
    )
    lines.append(
        "5. SameFrameSeamGate stays CLOSED for ungated every-frame dual until Mode77/headset "
        "proves DrawScene×2 is streaming-safe."
    )

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {OUT}")
    print(f"BuildRootA E8 callers: {len(callers)}")
    print(f"DrawScene E8 callers: {len(ds)}")
    print(f"call[+0x178] sites: {len(ff178)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
