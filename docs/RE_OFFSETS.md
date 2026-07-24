# RE offset map — GTA IV CE (Steam)

**Binary:** `GTAIV.exe` (PlayGTAIV.exe is byte-identical on CE 1.2.0.59)  
**Image base:** `0x00400000` (fixed; no ASLR on main module)  
**Image size:** `0x01BE6400`  
**Last verified:** 2026-07-25 (offline PE scan + live project logs; **no game launched this session**)

Use **RVA** = file offset in `.text` for this build (RVA == file offset for code sites below).

Re-find after patch: run `py scripts/offline-re-scan.py` against your local `GTAIV.exe`, or launch with stereo **`62`** once and read `ReValidate:` lines in `gtaiv_dxvk_vr.log`.

---

## Status legend

| Status | Meaning |
|--------|---------|
| **SAFE** | Hooked in production ASI today; post-load freeze-test passed for its role |
| **READ-ONLY** | Observation / COUNT / stack trace only — no game-function hook |
| **UNTESTED** | Statically identified; no safe live hook yet |
| **FORBIDDEN** | Crash, hard-kill, or disproven (do not MinHook / dual / replay) |

---

## Camera / CopyMat (game thread — SAFE)

| RVA | AOB / anchor | Role | Status |
|-----|----------------|------|--------|
| `0x83DB90` | prologue `55 8B EC 83 E4 F0 83 EC 18 56 8B 75 08` | **CopyMat** callee (all four follow-cam sites) | **SAFE** (called from hooks, not hooked at prologue) |
| `0x61FFC8` | `E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6` | CopyMat call — on-foot front | **SAFE** hook site |
| `0x61FF9B` | `E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74` | CopyMat call — on-foot behind | **SAFE** |
| `0x615AE6` | `E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24` | CopyMat call — vehicle front | **SAFE** |
| `0x6180B3` | `E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00` | CopyMat call — vehicle behind | **SAFE** |
| `0x4D14E0` | `8B 44 24 04 85 C0 75 18 A1` | **FindPlayerPed** | **SAFE** (AOB entry, not hooked) |
| — | CCam **`+0x60`** float | Live FOV field (FusionFix / Mode 35+) | **SAFE** write after recompute CALL |
| — | Cam matrix **`+0x50`** (+80) | CopyMat mat FOV probe (Mode 16) | READ-ONLY probe |
| — | Cam matrix **`+0xD0`** (+208) | Inert FOV field (Mode 17 test) | **UNTESTED** write (inert on CE) |

**Ovr→GTA mapping** (position): `gx=ox`, `gy=-oz`, `gz=oy`. Rage matrix: right=X, up=Y, at=Z (forward/up convention in `cam_matrix.cpp`).

---

## FOV recompute site (game thread — SAFE)

| RVA | AOB | Role | Status |
|-----|-----|------|--------|
| `0x70637C` | `E8 ? ? ? ? F6 87 ? ? ? ? ? 5B` | FusionFix **Custom FOV** CALL hook site (CE) | **SAFE** chain-hook |
| `0x706A00` | prologue `55 8B EC 83 E4 F0 83 EC 1C 56 6A 00` | Callee — cam process / FOV recompute | **SAFE** (called via chain) |
| — | Classic pattern `E8 ? ? ? ? 8B CE E8 …` | Pre-CE layout | **MISS** on this CE build |

Mode **45** adds a **late head-owned CopyMat refresh** at the FOV site (after recompute). Does **not** decouple collision or create distinct eyes.

---

## VS upload / replay thread (same-frame critical path)

| RVA | Bytes @ RVA (anchor) | Role | Status |
|-----|----------------------|------|--------|
| `0x2C180` | `55 8B EC 83 E4 F8 81 EC A0 03 00 00` | **VS upload wrapper** function start (contains VsRet + wrap) | READ-ONLY |
| `0x2C73E` | `85 C0 75 14 8D 44 24 0C …` | **VsRet** — return inside wrapper after `SetVSConstF` work | **READ-ONLY** anchor (~100k+/session) |
| `0x2C6AC` | `89 51 0A 3C 02 …` | Inner **VS wrap** site (same fn as VsRet) | **FORBIDDEN** (Mode 28 crash) |
| — | Static **`E8 → 0x2C73E`** | Direct callers | **0 sites** on CE — indirect only |

**Implication (L4D2VR / HL2VR lesson):** Source mods hook **`RenderView`** with static call graph. Rage uploads view constants on a **dedicated replay thread** through one wrapper; you cannot reach it with a simple `E8` scan. Same-frame distinct eyes need a **replay draw owner** called ~1×/frame with hookable ABI — not VsRet itself.

---

## VsRet stack chain (Mode 41/42 live log — READ-ONLY)

Mid return addresses seen when `HookSetVSConstF` returns to **`0x2C73E`** (not function starts):

| Mid RVA | Resolves / note | Status |
|---------|-----------------|--------|
| `0x22187` | Epilogue `… 5F C2 10 00` — slot 10 | **FORBIDDEN** as hook target (not a start) |
| `0x2C6A0` | Callee near slot 10 | UNTESTED |
| `0x37C01` | `E8` → **`0x37BD0`** — slot 15 | **FORBIDDEN** |
| `0x37BD0` | Wrong ABI prologue | **FORBIDDEN** (Mode 27/29 crash) |
| `0x37520` | Stackarg chain — slot 22 | **FORBIDDEN** |
| `0x372B0` | CC-pad but stackarg | **FORBIDDEN** |
| `0x32BD7` | → **`0x32A40`** stackarg — slot 27 | **FORBIDDEN** |
| `0x32A40` | `[ebp+8]` thiscall+stackarg | **FORBIDDEN** |
| `0x30D13` | Returns from **`0x309D0`** region — indirect edge | READ-ONLY (see below) |
| `0x3199A` / `0x319A4` | Direct `E8` but enclosing start unaligned | **UNTESTED** |

---

## Indirect edge @ `0x309D0` (Mode 42 — READ-ONLY)

| RVA | Note | Status |
|-----|------|--------|
| `0x309D0` | Mid-instruction / inner block (not a clean prologue) | **UNTESTED** — do not call blindly |
| `0x30D13` | Frequent return; Mode 42 logged **`FF /2 @ 0x30D0D`** `modrm=0x90` → **`call [eax+disp32]`** | READ-ONLY |

**Next RE step (no hook yet):** at live `0x30D0D`, record **`eax`** (object) and **`[eax+disp32]`** vtable target per frame — proves owner ABI/lifetime. Static PE alone cannot resolve indirect vtable calls.

---

## Forbidden same-frame / dual targets (do not hook)

| RVA | Why |
|-----|-----|
| `0x2C6AC` | Wrap hook → crash after load (Mode 28) |
| `0x37BD0` | Wrong ABI / crash (Mode 27/29) |
| `0x1BF010` | Mode 34 COUNT hard-kill (SSE stackarg misclassified) |
| `0x4DDAD0` | Mode 34 dual armed; **`vsPatch=0` / `vsCallsR=0`** forever |
| `0x309D0` | Observation only until indirect owner proven |

---

## Game-thread render roots (BuildRootA path — dual-list work)

| RVA | Prologue / anchor | Role | Status |
|-----|-------------------|------|--------|
| `0x8F7F00` | `55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08` | **BuildRootA** — 13-phase frame build (~1×/frame) | READ-ONLY (Mode 19/20); dual build-only = mono |
| `0x8F73B0` | near `0x8F7033`: `53 55 56 57 8B F1` | **ExecRoot** dispatcher — vt[9] Execute | READ-ONLY; 2nd call = UAF (Mode 22) |
| `0x5C2BF0` | `mov ecx, 0x118D7F0` … `E8` → BuildRootA | Wrapper sets render-phase manager **`this`** | UNTESTED |
| `0x118D7F0` | Global **render-phase manager** | `this` for build/exec | data |
| `0xD8D8C0` / `0xD8D9C0` | Manager **`+0xD0` / `+0x1D0`** pos rows | Live cam WORLD copies (Mode 21) | Shifting at exec did not change draws |

**BuildRenderList phase AOBs** (mid-body — FusionFix-safe):

| Phase | Mid RVA | Mid AOB (unique tail) | Fn start (mid − offset) |
|-------|---------|------------------------|---------------------------|
| DrawScene | `0x6DC60D` | `… FF 0F 84 DE 09 00 00 6A 00 6A 0C` | **`0x6DC600`** |
| PhaseA | `0x527EDE` | `… FF 0F 84 76 03 00 00 80 3D` | **`0x527EC0`** (−30) |
| PhaseC | `0x975D77` | `… FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | **`0x975D50`** (−39) |

RTTI strings (file offset): `.?AVCRenderPhaseDrawScene@@` @ `0xC3E664`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@` @ `0xC55640`.

**Hard finding:** BuildRootA ×2 (Mode 20) removes temporal jump but **does not** produce distinct eyes — draw lists double-buffer; view constants bake at **build**. Exec re-call from build thread = cross-thread crash. Real draws + VS uploads align on **replay thread**, not BuildRootA.

---

## Mode 34 COUNT candidates (historical)

| RVA | Mode 34 note | Status |
|-----|--------------|--------|
| `0x4D8F10` | avg ≈ 0.31 entries/frame — not ~1× | REJECT |
| `0x52E7C0` | ~1×/frame but stackarg | **FORBIDDEN** |
| `0x5303D0` | Rare agg ~1× | UNTESTED |
| `0x370D0` | Helper `56 57 8B 7C` — no dual | UNTESTED |

---

## Ped / presentation (SAFE)

| RVA | AOB | Role | Status |
|-----|-----|------|--------|
| `0x7B478C` | `E8 ? ? ? ? 83 C4 04 85 C0 74 ? 8B 0D` | SetDraw-style call site (PedHide) | **SAFE** |
| `0x66DCD0` | (callee from above) | CE draw-component helper | **SAFE** via AOB |

---

## Cross-mod RE targets (what we still need)

| Reference mod | Their seam | Rage equivalent (this project) |
|---------------|------------|--------------------------------|
| **L4D2VR / HL2VR** | Two **`RenderView`** / **`CViewSetup`** per tick | **Missing** — only CopyMat + VS observation |
| **BotW BetterVR** | PPC patches at **GPU fixed-function draw** | Mode 55 tried VS translate-only; parallax partial, controls fragile |
| **UEVR** | Native per-eye view + projection | No UE path — temporal pair-hold only |
| **Halo MCC VR** | Same-frame + wide FOV | FOV via **`0x70637C`** OK; same-frame **blocked** |

**SameFrameSeamGate:** `CLOSED` until a replay-thread owner satisfies:

1. ~1×/frame cadence on VsRet thread  
2. Zero-arg or CC-padded **thiscall** (no stackarg / no SSE misclass)  
3. COUNT-only ≥45 EndScenes without exception  
4. Dual trial shows **`vsPatch>0`** or distinct L/R canvas diff — **never** satisfied by `0x4DDAD0`  
5. Not in **FORBIDDEN** table above  

ASI stereo modes **`62`** (pattern validate + Mode 45 render) and **`63`** (seam gate log + Mode 45 render) scaffold this path; default remains **`45`**.

---

## How to re-find after Rockstar patch

1. Confirm image size / version string changed.  
2. `py scripts/offline-re-scan.py` — refresh AOB hits and anchor bytes.  
3. Update **expected RVA** column in this doc + `re_validate.cpp` `ExpectedSite` table.  
4. Re-run Mode **41**/**42** for one calm minute; paste new `Mode41: CHAIN` / `Mode42: OWNER-EDGE` rows.  
5. Do **not** hook new RVAs until COUNT-only + ABI class passes Mode 33/34 filters.

---

## Quick forbidden list (paste for agents)

```
0x2C6AC  0x37BD0  0x1BF010  0x4DDAD0  0x309D0(hook)  Mode34 dual @0x4DDAD0  0x309D0 blind call
```

Safe production stack: stereo **`45`**, FOV site **`0x70637C`**, CopyMat **`0x83DB90`**, VsRet observation **`0x2C73E`** only.

---

## World visual size (does not fix same-frame jump)

Three **independent** levers — do not combine aggressively in one test:

| Lever | File / key | Effect | vs perfect VR |
|-------|------------|--------|-----------------|
| **F7 leanGain** | `gtaiv_dxvk_vr.scale` (×100) | 6DoF head-lean only; higher = smaller world when leaning | Does not change building scale or stereo epoch |
| **F6 stereoscale** | `gtaiv_dxvk_vr.stereoscale` | Soft IPD/disparity multiplier (100–130%) | Can affect fusion if too high |
| **fovadd** | `gtaiv_dxvk_vr.fovadd` | Engine FOV ADD at **`0x70637C`** site → true canvas fill | Kills black bars; wide FOV can add temporal stress |

**Rule:** F7 ≠ Halo/Luke “world scale”. Real building size needs engine FOV (`fovadd`) + eventual same-frame stereo. Mode **45** keeps all three decoupled.

