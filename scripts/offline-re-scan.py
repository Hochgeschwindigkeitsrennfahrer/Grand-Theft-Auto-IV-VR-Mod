#!/usr/bin/env python3
"""Offline PE scan for GTA IV CE (GTAIV.exe). Read-only; no game launch."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\GTAIV.exe")
IMAGE_BASE = 0x400000

# --- x86 helpers (linear disasm for RE notes; not a full decoder) ---

REG8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def modrm_reg(modrm: int, is32: bool = True) -> str:
    idx = modrm & 7
    return (REG32 if is32 else REG8)[idx]


def modrm_mem(modrm: int, disp: bytes, addr: int) -> str:
    mod = (modrm >> 6) & 3
    rm = modrm & 7
    if mod == 3:
        return modrm_reg(modrm)
    base = REG32[rm]
    if mod == 0 and rm == 5:
        d = struct.unpack_from("<i", disp, 0)[0] if len(disp) >= 4 else 0
        return f"[0x{d & 0xFFFFFFFF:X}]"
    if mod == 1 and len(disp) >= 1:
        d = struct.unpack_from("<b", disp, 0)[0]
        return f"[{base}{d:+d}]"
    if mod == 2 and len(disp) >= 4:
        d = struct.unpack_from("<i", disp, 0)[0]
        return f"[{base}{d:+d}]"
    return f"[{base}]"


def disasm_one(data: bytes, off: int, image_size: int) -> tuple[int, str]:
    """Return (instr_len, text) or (1, db XX) on unknown."""
    if off >= image_size:
        return 0, ""
    b0 = data[off]
    rva = off

    # push reg32
    if 0x50 <= b0 <= 0x57:
        return 1, f"push {REG32[b0 - 0x50]}"
    # pop reg32
    if 0x58 <= b0 <= 0x5F:
        return 1, f"pop {REG32[b0 - 0x58]}"
    # ret / ret imm16
    if b0 == 0xC3:
        return 1, "ret"
    if b0 == 0xC2 and off + 3 <= image_size:
        imm = struct.unpack_from("<H", data, off + 1)[0]
        return 3, f"ret 0x{imm:X}"
    # nop
    if b0 == 0x90:
        return 1, "nop"
    # call rel32
    if b0 == 0xE8 and off + 5 <= image_size:
        rel = struct.unpack_from("<i", data, off + 1)[0]
        tgt = IMAGE_BASE + off + 5 + rel
        return 5, f"call 0x{tgt - IMAGE_BASE:X}"
    # jmp rel32
    if b0 == 0xE9 and off + 5 <= image_size:
        rel = struct.unpack_from("<i", data, off + 1)[0]
        tgt = IMAGE_BASE + off + 5 + rel
        return 5, f"jmp 0x{tgt - IMAGE_BASE:X}"
    # jmp rel8
    if b0 == 0xEB and off + 2 <= image_size:
        rel = struct.unpack_from("<b", data, off + 1)[0]
        tgt = off + 2 + rel
        return 2, f"jmp short 0x{tgt:X}"
    # mov ecx, imm32 (thiscall this)
    if b0 == 0xB9 and off + 5 <= image_size:
        imm = struct.unpack_from("<I", data, off + 1)[0]
        return 5, f"mov ecx, 0x{imm:X}"
    # mov edi, ecx (this in edi)
    if b0 == 0x8B and off + 2 <= image_size and data[off + 1] == 0xF9:
        return 2, "mov edi, ecx"
    # mov esi, ecx
    if b0 == 0x8B and off + 2 <= image_size and data[off + 1] == 0xF1:
        return 2, "mov esi, ecx"
    # push imm8
    if b0 == 0x6A and off + 2 <= image_size:
        imm = data[off + 1]
        return 2, f"push 0x{imm:X}"
    # push imm32
    if b0 == 0x68 and off + 5 <= image_size:
        imm = struct.unpack_from("<I", data, off + 1)[0]
        return 5, f"push 0x{imm:X}"
    # test eax,eax / cmp
    if b0 == 0x85 and off + 2 <= image_size:
        modrm = data[off + 1]
        if modrm == 0xC0:
            return 2, "test eax, eax"
        if modrm == 0xC9:
            return 2, "test ecx, ecx"
    if b0 == 0x84 and off + 2 <= image_size:
        modrm = data[off + 1]
        if modrm == 0xC0:
            return 2, "test al, al"
    # jcc rel8
    if 0x70 <= b0 <= 0x7F and off + 2 <= image_size:
        rel = struct.unpack_from("<b", data, off + 1)[0]
        tgt = off + 2 + rel
        names = ["jo", "jno", "jb", "jnb", "jz", "jnz", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"]
        return 2, f"{names[b0 - 0x70]} 0x{tgt:X}"
    # lea reg, [mem]
    if b0 == 0x8D and off + 2 <= image_size:
        modrm = data[off + 1]
        mod = (modrm >> 6) & 3
        extra = 0
        if mod == 1:
            extra = 1
        elif mod == 2:
            extra = 4
        elif mod == 0 and (modrm & 7) == 5:
            extra = 4
        if off + 2 + extra <= image_size:
            reg = REG32[(modrm >> 3) & 7]
            mem = modrm_mem(modrm, data[off + 2 : off + 2 + extra], off)
            return 2 + extra, f"lea {reg}, {mem}"
    # mov r/m32, r32 or r32, r/m32
    if b0 == 0x89 and off + 2 <= image_size:
        modrm = data[off + 1]
        mod = (modrm >> 6) & 3
        extra = 1 if mod == 1 else (4 if mod == 2 else (4 if mod == 0 and (modrm & 7) == 5 else 0))
        if mod == 3:
            dst = modrm_reg(modrm)
            src = REG32[(modrm >> 3) & 7]
            return 2, f"mov {dst}, {src}"
        if off + 2 + extra <= image_size:
            src = REG32[(modrm >> 3) & 7]
            mem = modrm_mem(modrm, data[off + 2 : off + 2 + extra], off)
            return 2 + extra, f"mov {mem}, {src}"
    if b0 == 0x8B and off + 2 <= image_size:
        modrm = data[off + 1]
        if modrm == 0xEC:
            return 2, "mov ebp, esp"
        if modrm == 0xE5:
            return 2, "mov esp, ebp"
        mod = (modrm >> 6) & 3
        extra = 1 if mod == 1 else (4 if mod == 2 else (4 if mod == 0 and (modrm & 7) == 5 else 0))
        if mod == 3:
            dst = REG32[(modrm >> 3) & 7]
            src = modrm_reg(modrm)
            return 2, f"mov {dst}, {src}"
        if off + 2 + extra <= image_size:
            dst = REG32[(modrm >> 3) & 7]
            mem = modrm_mem(modrm, data[off + 2 : off + 2 + extra], off)
            return 2 + extra, f"mov {dst}, {mem}"
    # sub esp, imm8
    if b0 == 0x83 and off + 3 <= image_size and data[off + 1] == 0xEC:
        imm = data[off + 2]
        return 3, f"sub esp, 0x{imm:X}"
    # and esp, imm8 (align)
    if b0 == 0x83 and off + 3 <= image_size and data[off + 1] == 0xE4:
        imm = data[off + 2]
        return 3, f"and esp, 0x{imm:X}"
    # FF /2 call [mem] or FF /6 push [mem]
    if b0 == 0xFF and off + 2 <= image_size:
        modrm = data[off + 1]
        op = (modrm >> 3) & 7
        mod = (modrm >> 6) & 3
        extra = 1 if mod == 1 else (4 if mod == 2 else (4 if mod == 0 and (modrm & 7) == 5 else 0))
        if off + 2 + extra <= image_size:
            mem = modrm_mem(modrm, data[off + 2 : off + 2 + extra], off)
            if op == 2:
                return 2 + extra, f"call {mem}"
            if op == 6:
                return 2 + extra, f"push {mem}"
            if op == 4:
                return 2 + extra, f"jmp {mem}"
    # SSE movss / addss (F3 0F 10/11/58)
    if b0 == 0xF3 and off + 3 <= image_size and data[off + 1] == 0x0F:
        op2 = data[off + 2]
        if op2 in (0x10, 0x11, 0x58) and off + 4 <= image_size:
            modrm = data[off + 3]
            mod = (modrm >> 6) & 3
            extra = 1 if mod == 1 else (4 if mod == 2 else (4 if mod == 0 and (modrm & 7) == 5 else 0))
            if off + 4 + extra <= image_size:
                mnem = {0x10: "movss", 0x11: "movss", 0x58: "addss"}[op2]
                if mod == 3:
                    a = REG32[(modrm >> 3) & 7]
                    b = modrm_reg(modrm)
                    if op2 == 0x10:
                        return 4 + extra, f"movss {a}, {b}"
                    if op2 == 0x11:
                        return 4 + extra, f"movss {b}, {a}"
                else:
                    reg = REG32[(modrm >> 3) & 7]
                    mem = modrm_mem(modrm, data[off + 4 : off + 4 + extra], off)
                    if op2 == 0x10:
                        return 4 + extra, f"movss {reg}, {mem}"
                    if op2 == 0x11:
                        return 4 + extra, f"movss {mem}, {reg}"
                    if op2 == 0x58:
                        return 4 + extra, f"addss {reg}, {mem}"
    # group: push ebp; mov ebp,esp (55 8B EC)
    if b0 == 0x55:
        return 1, "push ebp"
    # unknown — skip 1 byte
    return 1, f"db 0x{b0:02X}"


def disasm_range(data: bytes, start: int, length: int, image_size: int, max_insn: int = 64) -> list[str]:
    lines: list[str] = []
    off = start
    end = min(start + length, image_size)
    n = 0
    while off < end and n < max_insn:
        ln, txt = disasm_one(data, off, image_size)
        if ln <= 0:
            break
        lines.append(f"  0x{off:06X}: {txt}")
        off += ln
        n += 1
    return lines


def parse_pe(data: bytes) -> tuple[int, int, int]:
    if data[:2] != b"MZ":
        raise SystemExit("Not PE")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise SystemExit("Bad PE signature")
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise SystemExit(f"Expected PE32, got 0x{magic:X}")
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    return e_lfanew, size_of_image, opt


def parse_pattern(pat: str) -> list[int | None]:
    out: list[int | None] = []
    for p in pat.split():
        if p in ("?", "??"):
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


def scan_e8_to(data: bytes, target_rva: int, image_size: int, limit: int = 64) -> list[tuple[int, int]]:
    target_va = IMAGE_BASE + target_rva
    hits: list[tuple[int, int]] = []
    for off in range(min(image_size, len(data)) - 5):
        if data[off] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, off + 1)[0]
        dst = rva(off) + 5 + rel
        if dst == target_va:
            hits.append((off, rva(off)))
            if len(hits) >= limit:
                break
    return hits


def scan_e8_into_range(data: bytes, lo_rva: int, hi_rva: int, image_size: int, limit: int = 32) -> list[tuple[int, int, int]]:
    """E8 calls landing anywhere in [lo_rva, hi_rva]."""
    hits: list[tuple[int, int, int]] = []
    lo_va = IMAGE_BASE + lo_rva
    hi_va = IMAGE_BASE + hi_rva
    for off in range(min(image_size, len(data)) - 5):
        if data[off] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, off + 1)[0]
        dst = rva(off) + 5 + rel
        if lo_va <= dst <= hi_va:
            hits.append((off, rva(off), dst - IMAGE_BASE))
            if len(hits) >= limit:
                break
    return hits


def bytes_at(data: bytes, off: int, n: int = 16) -> str:
    chunk = data[off : off + n]
    return " ".join(f"{b:02X}" for b in chunk)


def find_fn_start(data: bytes, addr_in_fn: int, max_back: int = 0x800) -> int | None:
    lo = max(0, addr_in_fn - max_back)
    for i in range(addr_in_fn, lo, -1):
        if i + 3 >= len(data):
            continue
        b0, b1, b2, b3 = data[i], data[i + 1], data[i + 2], data[i + 3]
        if b0 == 0x55 and b1 == 0x8B and b2 == 0xEC:
            return i
        if b0 == 0x56 and b1 == 0x57 and b2 == 0x8B and b3 == 0xF9:
            return i
        if b0 == 0x53 and b1 == 0x55 and b2 == 0x56 and b3 == 0x57:
            return i
        if b0 == 0x83 and b1 == 0xEC:
            return i
        if b0 == 0x56 and b1 == 0x8B and b2 == 0xF1:
            return i
    return None


def scan_movss_disp(data: bytes, disp: int, image_size: int, limit: int = 12) -> list[int]:
    """Find movss [reg+disp32], xmm — candidate struct field writes."""
    needle = struct.pack("<I", disp & 0xFFFFFFFF)
    hits: list[int] = []
    for off in range(image_size - 8):
        # F3 0F 11 ?? disp32  (store)
        if data[off] == 0xF3 and data[off + 1] == 0x0F and data[off + 2] == 0x11:
            modrm = data[off + 3]
            mod = (modrm >> 6) & 3
            if mod == 2 and off + 8 <= len(data) and data[off + 4 : off + 8] == needle:
                hits.append(off)
                if len(hits) >= limit:
                    break
        # F3 0F 10 ?? disp32  (load)
        if data[off] == 0xF3 and data[off + 1] == 0x0F and data[off + 2] == 0x10:
            modrm = data[off + 3]
            mod = (modrm >> 6) & 3
            if mod == 2 and off + 8 <= len(data) and data[off + 4 : off + 8] == needle:
                hits.append(off)
                if len(hits) >= limit:
                    break
    return hits


def scan_e8_from_fn(data: bytes, fn_rva: int, fn_len: int, image_size: int) -> list[tuple[int, int]]:
    """E8 callees inside [fn_rva, fn_rva+fn_len)."""
    hits: list[tuple[int, int]] = []
    lo = fn_rva
    hi = min(fn_rva + fn_len, image_size)
    for off in range(lo, hi - 5):
        if data[off] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, off + 1)[0]
        dst = rva(off) + 5 + rel - IMAGE_BASE
        hits.append((off, dst))
    return hits


def scan_ff90_178(data: bytes, lo: int, hi: int) -> list[int]:
    needle = struct.pack("<I", 0x178)
    out: list[int] = []
    for off in range(lo, min(hi, len(data) - 6)):
        if data[off : off + 2] == b"\xFF\x90" and data[off + 2 : off + 6] == needle:
            out.append(off)
    return out


def print_capstone_fn(data: bytes, start: int, max_bytes: int, image_size: int, title: str) -> None:
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        print(f"=== {title} @ 0x{start:X} ===")
        print("  (install capstone for full disasm: py -m pip install capstone)")
        print(f"  prologue: {bytes_at(data, start, 16)}")
        print()
        return

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print(f"=== {title} @ 0x{start:X} ===")
    print(f"  prologue: {bytes_at(data, start, 16)}")
    code = data[start : min(start + max_bytes, image_size)]
    n = 0
    end = start
    for insn in md.disasm(code, IMAGE_BASE + start):
        rva_off = insn.address - IMAGE_BASE
        mark = ""
        if rva_off == 0x3010D:
            mark = "  ; call [eax+0x178]"
        if rva_off == 0x30349:
            mark = "  ; cmp [activeView]; je -> call 0x300D0"
        if rva_off in (0x3199A, 0x319A4):
            mark = "  ; Mode41 mid-ret"
        print(f"  0x{rva_off:06X}: {insn.mnemonic} {insn.op_str}{mark}")
        n += 1
        end = rva_off + insn.size
        if insn.mnemonic == "ret":
            break
        if n > 220:
            print("  ... truncated")
            break
    print(f"  insns={n} fn_len~0x{end - start:X}")
    print()


def scan_active_view_global(data: bytes, image_size: int) -> None:
    """Map [0x17F583C] active view object — single writer, many readers."""
    active_rva = 0x17F583C
    imm = struct.pack("<I", active_rva)
    reads: list[tuple[int, str]] = []
    writes: list[tuple[int, str]] = []
    for off in range(min(image_size, len(data)) - 6):
        b = data[off]
        if b == 0xA1 and data[off + 1 : off + 5] == imm:
            reads.append((off, "mov eax, [activeView]"))
        elif b in (0x8B, 0x89) and off + 6 <= len(data):
            modrm = data[off + 1]
            if (modrm & 0xC7) == 0x05 and data[off + 2 : off + 6] == imm:
                reg = REG32[(modrm >> 3) & 7]
                if b == 0x8B:
                    reads.append((off, f"mov {reg}, [activeView]"))
                else:
                    writes.append((off, f"mov [activeView], {reg}"))
        elif b == 0x39 and off + 6 <= len(data):
            modrm = data[off + 1]
            if (modrm & 0xC7) == 0x05 and data[off + 2 : off + 6] == imm:
                reg = REG32[(modrm >> 3) & 7]
                reads.append((off, f"cmp [activeView], {reg}"))
        elif b == 0x3B and off + 6 <= len(data):
            modrm = data[off + 1]
            if (modrm & 0xC7) == 0x05 and data[off + 2 : off + 6] == imm:
                reg = REG32[(modrm >> 3) & 7]
                reads.append((off, f"cmp {reg}, [activeView]"))
    print("=== Active view global [0x17F583C] (PublishSync replay gate) ===")
    print(f"  code reads: {len(reads)}  writes: {len(writes)}")
    for off, kind in writes:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"    WRITE 0x{off:X}  {kind}  enclosing~{fn_s}")
    key_reads = [0x2FFD0, 0x30349, 0x30841, 0x3006F]
    print("  key compare/set sites:")
    for key in key_reads:
        fn = find_fn_start(data, key)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"    0x{key:X}  enclosing~{fn_s}")
    print()


def scan_global_device_singleton(data: bytes, image_size: int) -> None:
    """Map [0x17ed8d8] device singleton — reads/writes and vtable slot usage."""
    global_rva = 0x17ED8D8
    gate_rva = 0x17ED918
    imm_dev = struct.pack("<I", global_rva)
    imm_gate = struct.pack("<I", gate_rva)
    reads: list[tuple[int, str]] = []
    writes: list[tuple[int, str]] = []
    for off in range(min(image_size, len(data)) - 6):
        b = data[off]
        if b == 0xA1 and data[off + 1 : off + 5] == imm_dev:
            reads.append((off, "mov eax, [device]"))
        elif b == 0xA3 and data[off + 1 : off + 5] == imm_dev:
            writes.append((off, "mov [device], eax"))
        elif b == 0xC7 and data[off + 1] == 0x05 and data[off + 2 : off + 6] == imm_dev:
            val = struct.unpack_from("<I", data, off + 6)[0]
            writes.append((off, f"mov dword [device], 0x{val:X}"))
        elif b in (0x8B, 0x89) and off + 6 <= len(data):
            modrm = data[off + 1]
            if (modrm & 0xC7) == 0x05 and data[off + 2 : off + 6] == imm_dev:
                reg = REG32[(modrm >> 3) & 7]
                if b == 0x8B:
                    reads.append((off, f"mov {reg}, [device]"))
                else:
                    writes.append((off, f"mov [device], {reg}"))
        if b == 0x83 and off + 7 <= len(data) and data[off + 1] == 0x3D and data[off + 2 : off + 6] == imm_gate:
            reads.append((off, "cmp [replayGate], imm"))
    print("=== Device singleton [0x17ed8d8] + replay gate [0x17ed918] ===")
    print(f"  code reads [device]: {len(reads)}  writes: {len(writes)}")
    for off, kind in writes:
        print(f"    WRITE 0x{off:X}  {kind}")
    print("  sample reads (first 8):")
    for off, kind in reads[:8]:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"    READ  0x{off:X}  {kind}  enclosing~{fn_s}")
    # vtable slots +0x178 / +0x1B4 (same pair as 0x3010D)
    vt_hits: list[tuple[int, str, int]] = []
    for off in range(min(image_size, len(data)) - 6):
        if data[off] != 0xFF:
            continue
        modrm = data[off + 1]
        if ((modrm >> 3) & 7) != 2 or (modrm >> 6) != 2:
            continue
        disp = struct.unpack_from("<i", data, off + 2)[0]
        if disp in (0x178, 0x1B4):
            reg = REG32[modrm & 7]
            vt_hits.append((off, reg, disp))
    print(f"  call [reg+0x178/0x1B4] sites (exe-wide): {len(vt_hits)}")
    max_slot = 0
    for off in range(min(image_size, len(data)) - 6):
        if data[off] != 0xFF:
            continue
        modrm = data[off + 1]
        if ((modrm >> 3) & 7) != 2 or (modrm >> 6) != 2:
            continue
        disp = struct.unpack_from("<i", data, off + 2)[0]
        if 0 <= disp <= 0x400 and (disp & 3) == 0:
            max_slot = max(max_slot, disp)
    print(f"  max FF/2 disp32 vtable slot: 0x{max_slot:X} (entry ~{max_slot // 4})")
    for off, reg, disp in vt_hits:
        if off in (0x3010D, 0x30120):
            print(f"    0x{off:X} call [{reg}+0x{disp:X}]  ; ReplayDispatch owner")
    print()


def print_sameframe_deep_scan(data: bytes, image_size: int) -> None:
    print("=== SameFrameSeamGate deep scan (0x3187C + 0x3010D) ===")
    fn318 = 0x3187C
    print(f"  ViewConst 0x{fn318:X}: {bytes_at(data, fn318, 16)}")
    e8_in = scan_e8_from_fn(data, fn318, 0x400, image_size)
    print(f"  E8 callees from 0x{fn318:X} body: {len(e8_in)}")
    for off, dst in e8_in[:12]:
        print(f"    0x{off:X} -> 0x{dst:X}")
    e8_to = scan_e8_to(data, fn318, image_size, 32)
    print(f"  Static E8 -> 0x{fn318:X}: {len(e8_to)} sites")
    print()

    print_capstone_fn(data, fn318, 0x400, image_size, "View-const fn 0x3187C (to ret)")

    print("=== 0x3010D owner fn 0x300D0 (NOT 0x2FBF0) ===")
    for off in scan_ff90_178(data, 0x2F000, 0x32000):
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"  call [eax+0x178] @ 0x{off:X} enclosing~{fn_s}")
    print_capstone_fn(data, 0x300D0, 0x70, image_size, "Replay dispatch owner 0x300D0")
    print_capstone_fn(data, 0x2FBF0, 0x50, image_size, "Matrix mul helper 0x2FBF0 (callee from 0x3187C)")
    print_capstone_fn(data, 0x308C9, 0x180, image_size, "View-matrix writer 0x308C9 (Mode41 ret 0x30D13)")
    print_capstone_fn(data, 0x30300, 0x70, image_size, "Publish sync 0x30300 (12 E8 callers; may call 0x300D0)")
    hits303 = scan_e8_to(data, 0x30300, min(image_size, len(data)), 64)
    print(f"  All static E8 -> 0x30300 ({len(hits303)} sites):")
    for off, _ in hits303:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"    call@0x{off:X} enclosing~{fn_s}")
    print()
    print_capstone_fn(data, 0x30FA0, 0x120, image_size, "Publish proj 0x30FA0 (tail from 0x3187C)")
    print_capstone_fn(data, 0x30A60, 0x140, image_size, "Publish VP block 0x30A60 (entry via call 0x30C10)")

    targets = [
        (0x3187C, "ViewConst"),
        (0x308C9, "ViewMatWriter"),
        (0x300D0, "ReplayDispatch"),
        (0x30300, "PublishSync"),
        (0x30FA0, "PublishProj"),
        (0x30C10, "PublishVP"),
        (0x2FBF0, "MatMul"),
    ]
    print("=== Static E8 caller counts (same-frame family) ===")
    for tgt, name in targets:
        hits = scan_e8_to(data, tgt, min(image_size, len(data)), 64)
        print(f"  -> {name} 0x{tgt:X}: {len(hits)} direct E8")
        for off, _ in hits[:4]:
            fn = find_fn_start(data, off)
            fn_s = f"0x{fn:X}" if fn else "?"
            print(f"      call@0x{off:X} enclosing~{fn_s}")
    print()


def print_disasm_section(title: str, data: bytes, rva_off: int, span: int, image_size: int) -> None:
    print(f"=== {title} @ 0x{rva_off:X} ===")
    fn = find_fn_start(data, rva_off)
    if fn is not None and fn != rva_off:
        print(f"  fn_start~0x{fn:X}  (anchor+0x{rva_off - fn:X})")
    for line in disasm_range(data, rva_off, span, image_size):
        print(line)
    print()


def main() -> int:
    if not EXE.is_file():
        print(f"MISSING: {EXE}")
        return 1

    data = EXE.read_bytes()
    _, image_size, _ = parse_pe(data)
    print(f"GTAIV.exe size={len(data)} image_size=0x{image_size:X} base=0x{IMAGE_BASE:X}")
    print()

    known = {
        0x2C180: ("VS upload wrapper fn", "Contains VsRet+wrap; replay thread"),
        0x2C73E: ("VsRet", "SetVSConstF upload return — replay thread anchor"),
        0x2C6AC: ("VS wrap", "FORBIDDEN — Mode 28 crash"),
        0x37BD0: ("VsParent wrong ABI", "FORBIDDEN — Mode 27/29 crash"),
        0x1BF010: ("thiscall-frame COUNT", "FORBIDDEN — Mode 34 hard-kill"),
        0x4DDAD0: ("Mode34 dual", "FORBIDDEN — vsPatch=0"),
        0x309D0: ("indirect edge", "Mode 41/42 observation — FF/2 call [eax+disp]"),
        0x83DB90: ("CopyMat callee", "All four follow-cam CopyMat sites"),
        0x706A00: ("Fov recompute callee", "Called from 0x70637C"),
        0x8F7F00: ("BuildRootA", "Game-thread 13-phase build root"),
        0x8F73B0: ("ExecRoot dispatch", "Exec dispatcher vt[9]"),
        0x5C2BF0: ("BuildRootA wrapper", "mov ecx,0x118D7F0; call BuildRootA"),
        0x5C2380: ("FrameA / ExecRoot caller", "Adjacent to BuildWrap"),
        0xA87680: ("BuildExecCD loop", "PhaseC+DrawScene build+exec"),
        0x527EC0: ("PhaseA start", "BuildRenderList PhaseA"),
        0x975D50: ("PhaseC start", "BuildRenderList PhaseC"),
        0x6DC600: ("DrawScene fn start", "DrawScene BuildRenderList"),
        0x66DCD0: ("SetDraw helper", "PedHide callee"),
    }

    print("=== Known RVA byte verify (first 12 bytes) ===")
    for rva_off, (tag, note) in sorted(known.items()):
        if rva_off + 8 > len(data):
            print(f"  0x{rva_off:X} {tag}: OUT OF RANGE")
            continue
        print(f"  0x{rva_off:06X} {tag:24s} {bytes_at(data, rva_off, 12)}  | {note}")
    print()

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
        "VS wrapper prologue": "55 8B EC 83 E4 F8 81 EC A0 03 00 00",
        "BuildRootA prologue": "55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08",
        "CopyMat prologue": "55 8B EC 83 E4 F0 83 EC 18 56 8B 75 08",
        "Fov callee prologue": "55 8B EC 83 E4 F0 83 EC 1C 56 6A 00",
    }

    print("=== AOB scan (first hits) ===")
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

    # --- Deep disasm blocks ---
    print_disasm_section("BuildRootA prologue+body", data, 0x8F7F00, 0x90, image_size)
    print_disasm_section("BuildRootA wrapper 0x5C2BF0", data, 0x5C2BF0, 0x40, image_size)
    print_disasm_section("CopyMat callee 0x83DB90", data, 0x83DB90, 0x60, image_size)
    print_disasm_section("FovSite hook 0x70637C", data, 0x706370, 0x30, image_size)
    print_disasm_section("Fov recompute callee 0x706A00", data, 0x706A00, 0x50, image_size)
    print_disasm_section("VS upload wrapper 0x2C180", data, 0x2C180, 0x50, image_size)
    print_disasm_section("VsRet anchor 0x2C730", data, 0x2C730, 0x30, image_size)
    print_disasm_section("VS wrap FORBIDDEN 0x2C6A0", data, 0x2C690, 0x30, image_size)
    print_disasm_section("Indirect edge 0x309D0", data, 0x309C0, 0x60, image_size)

    # --- E8 caller graphs ---
    targets = [
        (0x83DB90, "CopyMat"),
        (0x706A00, "FovRecompute"),
        (0x8F7F00, "BuildRootA"),
        (0x8F73B0, "ExecRoot"),
        (0x66DCD0, "SetDrawPed"),
    ]
    print("=== Static E8 caller counts ===")
    for tgt, name in targets:
        hits = scan_e8_to(data, tgt, min(image_size, len(data)), 64)
        print(f"  -> {name} 0x{tgt:X}: {len(hits)} direct E8 sites")
        for off, site_rva in hits[:8]:
            fn = find_fn_start(data, off)
            fn_s = f"0x{fn:X}" if fn is not None else "?"
            print(f"      call@0x{off:X} enclosing~{fn_s}")
        if len(hits) > 8:
            print(f"      ... +{len(hits) - 8} more")
    print()

    print("=== E8 callers into VS wrapper region 0x2C180–0x2C7FE ===")
    vs_hits = scan_e8_into_range(data, 0x2C180, 0x2C7FE, min(image_size, len(data)), 32)
    print(f"  sites: {len(vs_hits)}")
    for off, site, dst in vs_hits[:16]:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"    0x{off:X} -> 0x{dst:X}  enclosing~{fn_s}")
    print()

    print("=== Static E8 callers -> VsRet (0x2C73E) ===")
    e8_hits = scan_e8_to(data, 0x2C73E, min(image_size, len(data)))
    print(f"  direct E8 call sites: {len(e8_hits)}")
    print()

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
    print("=== VsRet chain mid-RVAs ===")
    for mid, note in chain_mids:
        if mid >= len(data):
            continue
        fn = find_fn_start(data, mid)
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"  0x{mid:06X} start~{fn_s:8s}  {bytes_at(data, mid, 8)}  | {note}")
    print()

    # --- CCam+0x60 (0x60) movss candidates ---
    print("=== movss/addss with disp=0x60 (CCam FOV field candidate) ===")
    fov60 = scan_movss_disp(data, 0x60, min(image_size, len(data)), 16)
    print(f"  hits: {len(fov60)} (showing first 12)")
    for h in fov60[:12]:
        fn = find_fn_start(data, h)
        fn_s = f"0x{fn:X}" if fn is not None else "?"
        print(f"    0x{h:X} enclosing~{fn_s}  {bytes_at(data, h, 10)}")
    print()

    # --- View-matrix manager offsets (+0xD0/+0x1D0 from 0x118D7F0) ---
    print("=== Render manager camera matrix touch sites (+0xD0/+0x1D0) ===")
    for disp, label in [(0xD0, "camA"), (0x1D0, "camB")]:
        hits = scan_movss_disp(data, disp, min(image_size, len(data)), 8)
        print(f"  disp 0x{disp:X} ({label}): {len(hits)} SSE sites (first 4)")
        for h in hits[:4]:
            fn = find_fn_start(data, h)
            fn_s = f"0x{fn:X}" if fn is not None else "?"
            print(f"    0x{h:X} enclosing~{fn_s}")
    print()

    # --- Mode 34 COUNT candidates ---
    count_candidates = [0x4D8F10, 0x52E7C0, 0x5303D0, 0x370D0, 0x4DDAD0, 0x3199A, 0x319A4]
    print("=== Same-frame COUNT candidate prologues ===")
    for c in count_candidates:
        if c >= len(data):
            continue
        fn = find_fn_start(data, c) or c
        print(f"  0x{c:06X} start~0x{fn:X}: {bytes_at(data, fn, 16)}")
    print()

    rtti_needles = [
        b"CRenderPhaseDrawScene",
        b"CRenderPhaseDeferredLighting_SceneToGBuffer",
        b"CCam",
        b"SetDrawPlayerComponent",
        b"BuildRenderList",
        b"CShader",
        b"viewProj",
    ]
    print("=== Interesting strings (file offset) ===")
    for needle in rtti_needles:
        refs = find_string_refs(data, needle, 3)
        if refs:
            for ref in refs:
                s = read_cstr(data, ref, 80)
                print(f"  +0x{ref:X}: {s[:70]}")
        else:
            print(f"  (none) {needle.decode(errors='replace')}")
    print()

    print("=== BuildRootA caller: mov ecx,0x118D7F0 near E8 BuildRootA ===")
    br_e8 = scan_e8_to(data, 0x8F7F00, min(image_size, len(data)), 16)
    imm = struct.pack("<I", 0x118D7F0)
    for off, _ in br_e8:
        # scan 16 bytes before call for mov ecx, imm32
        for back in range(max(0, off - 20), off):
            if data[back] == 0xB9 and data[back + 1 : back + 5] == imm:
                print(f"  BuildRootA call@0x{off:X} preceded by mov ecx,0x118D7F0 @0x{back:X}")
                for line in disasm_range(data, back, off - back + 5, image_size, 8):
                    print(f"    {line.strip()}")
    print()

    print("=== 0x309D0 region indirect calls ===")
    for off in range(0x30CE0, 0x30D30):
        if off + 2 <= len(data) and data[off] == 0xFF:
            modrm = data[off + 1]
            op = (modrm >> 3) & 7
            if op in (2, 4):
                print(f"    FF/{modrm:02X} @0x{off:X}: {bytes_at(data, off, 8)}")
    print()

    print("=== Corrected indirect replay dispatch (static PE) ===")
    off = 0x3010D
    fn = find_fn_start(data, off)
    fn_s = f"0x{fn:X}" if fn is not None else "?"
    print(f"  0x3010D: {bytes_at(data, off, 8)}  enclosing~{fn_s}  (call [eax+0x178])")
    print("  NOTE: owner is ~0x300D0; 0x2FBF0 is mat-mul callee from 0x3187C")
    print("  NOTE: Mode41/42 stack ret 0x30D13 is mid-SSE in fn 0x308C9 — not a call site")
    vc = 0x3187C
    print(f"  View-const candidate 0x{vc:X}: {bytes_at(data, vc, 16)}")
    for ret in [0x3199A, 0x319A4]:
        fn2 = find_fn_start(data, ret)
        if fn2:
            print(f"    ret 0x{ret:X} -> enclosing~0x{fn2:X}")
    print()

    print_sameframe_deep_scan(data, image_size)

    print("=== View activation 0x2FFF0 (SetActiveView — [0x17F583C] writer) ===")
    print_capstone_fn(data, 0x2FFF0, 0xC0, image_size, "View activation 0x2FFF0")
    hits2f = scan_e8_to(data, 0x2FFF0, min(image_size, len(data)), 64)
    print(f"  Static E8 -> 0x2FFF0: {len(hits2f)} sites")
    for off, _ in hits2f:
        fn = find_fn_start(data, off)
        fn_s = f"0x{fn:X}" if fn else "?"
        print(f"    call@0x{off:X} enclosing~{fn_s}")
    e8_out = scan_e8_from_fn(data, 0x2FFF0, 0xC0, image_size)
    print(f"  E8 callees from 0x2FFF0: {len(e8_out)}")
    for off, dst in e8_out:
        print(f"    0x{off:X} -> 0x{dst:X}")
    fn303 = {find_fn_start(data, o) for o, _ in scan_e8_to(data, 0x30300, image_size, 64)}
    fn2f = {find_fn_start(data, o) for o, _ in hits2f}
    fn303.discard(None)
    fn2f.discard(None)
    shared = fn303 & fn2f
    print(f"  Shared enclosing fns with 0x30300 callers: {[hex(x) for x in sorted(shared)]}")
    print()

    scan_active_view_global(data, image_size)
    scan_global_device_singleton(data, image_size)

    fov_hits = find_all(data, patterns["FovSite CE"], 1)
    if fov_hits:
        site = fov_hits[0]
        rel = struct.unpack_from("<i", data, site + 1)[0]
        tgt = rva(site) + 5 + rel
        print(f"=== FovSite CALL chain ===")
        print(f"  hook site 0x{site:X} -> callee 0x{tgt - IMAGE_BASE:X}")
        fn = find_fn_start(data, site)
        print(f"  caller fn~0x{fn:X}" if fn else "  caller fn=?")
        print(f"  callee bytes: {bytes_at(data, tgt - IMAGE_BASE, 24)}")
    print()

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
