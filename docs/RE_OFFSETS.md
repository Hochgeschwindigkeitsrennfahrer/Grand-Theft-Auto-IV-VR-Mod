# RE offset map — GTA IV CE (Steam)

**Binary:** `GTAIV.exe` (PlayGTAIV.exe is byte-identical on CE 1.2.0.59)  
**Image base:** `0x00400000` (fixed; no ASLR on main module)  
**Image size:** `0x01BE6400`  
**Last verified:** 2026-07-25 ~01:45 (**PE section skew fix** — mapped RVA ≠ file offset)

## CRITICAL — file offset vs mapped RVA (+0xC00)

CE `.text`: `VirtualAddress=0x1000`, `PointerToRawData=0x400` → **`mappedRVA = fileOffset + 0xC00`**.

Older notes (and Mode 64/66/67 first cut) treated **file offsets as RVAs**. Live Mode **66**
REJECT (`bytes=FF 83 F8 FF 74 0C` @ doc `0x30300`) was exactly that bug.

| Site | Old (file off, WRONG as RVA) | **Mapped RVA (correct)** |
|------|------------------------------|--------------------------|
| PublishSync | `0x30300` | **`0x30F00`** |
| ViewMatWriter | `0x308C0` | **`0x314C0`** |
| ViewConst TRUE start | `0x31870` | **`0x32470`** (Mode **64**; mid `0x3247C` unsafe) |
| ViewConst mid (old doc) | `0x3187C` | **`0x3247C`** — do **not** hook |
| `call [eax+0x178]` | `0x3010D` | **`0x30D0D`** (ret after = **`0x30D13`**) |
| MatMul helper | `0x2FBF0` | **`0x307F0`** (not `0x307F0`) |
| VsRet | `0x2C73E` | **`0x2D33E`** |
| VS wrap (FORBIDDEN) | `0x2C6AC` | **`0x2D2AC`** |
| VS wrapper fn | `0x2C180` | **`0x2CD80`** |
| FovSite CALL | `0x70637C` | **`0x706F7C`** |
| FindPlayerPed | `0x4D14E0` | **`0x4D20E0`** |
| SetActiveView | `0x2FFF0` | **`0x30BF0`** |
| Replay dispatch | `0x300D0` | **`0x30CD0`** |
| PublishProj (post-sync) | `0x30FA0` | **`0x31BA0`** — called after PublishSync from ViewConst/ViewMat |

**ASI fix:** `ResolveReSite()` AOB + mapped RVAs for Mode **64/66/67/68**; Mode 66/68 no CC-pad.
**Offline:** `py scripts/offline-seam-mapped.py` (preferred) or `offline-re-scan.py`.
**Mode 68:** COUNT @ ReplayDispatch **`0x30CD0`** → slot178 live **`0x220D0`**.

Use **mapped RVA** = address − `0x400000` in the running process.

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

**CopyMat ABI (offline disasm @ `0x83DB90`):**

| Reg / stack | Role |
|-------------|------|
| `ecx` | Dest cam matrix (`CCam` internal mat base in `edi` after prologue) |
| `[ebp+8]` | Source 4×3 row-major matrix pointer (`esi`) |
| Returns | Writes rows to `[edi+16..44]` via `movss`/`mov` — position row +3 at `+16`, right `+20/+24`, up `+32/+36`, at `+44` |

Inner call: `call 0x4D5720` (matrix helper) before row copy. **29 static `E8` callers** in CE exe; production hooks only the four follow-cam sites below.
| `0x620BC8` | `E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6` | CopyMat call — on-foot front | **SAFE** hook site |
| `0x620B9B` | `E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74` | CopyMat call — on-foot behind | **SAFE** |
| `0x6166E6` | `E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24` | CopyMat call — vehicle front | **SAFE** |
| `0x618CB3` | `E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00` | CopyMat call — vehicle behind | **SAFE** |
| `0x4D20E0` | `8B 44 24 04 85 C0 75 18 A1` | **FindPlayerPed** | **SAFE** (AOB entry, not hooked) |
| — | CCam **`+0x60`** float | Live FOV field (FusionFix / Mode 35+) | **SAFE** write after recompute CALL |
| — | Cam matrix **`+0x50`** (+80) | CopyMat mat FOV probe (Mode 16) | READ-ONLY probe |
| — | Cam matrix **`+0xD0`** (+208) | Inert FOV field (Mode 17 test) | **UNTESTED** write (inert on CE) |

**Ovr→GTA mapping** (position): `gx=ox`, `gy=-oz`, `gz=oy`. Rage matrix: right=X, up=Y, at=Z (forward/up convention in `cam_matrix.cpp`).

---

## FOV recompute site (game thread — SAFE)

| RVA | AOB | Role | Status |
|-----|-----|------|--------|
| `0x706F7C` | `E8 ? ? ? ? F6 87 ? ? ? ? ? 5B` | FusionFix **Custom FOV** CALL hook site (CE) | **SAFE** chain-hook |
| `0x706A00` | prologue `55 8B EC 83 E4 F0 83 EC 1C 56 6A 00` | Callee — cam process / FOV recompute | **SAFE** (called via chain) |
| `0x706174` | enclosing fn of FovSite | CCam update thunk — `mov ecx,edi` before both calls | READ-ONLY |
| — | Classic pattern `E8 ? ? ? ? 8B CE E8 …` | Pre-CE layout | **MISS** on this CE build |

**FovSite call graph (offline disasm):**

```
0x706174  CCam update (this=edi)
  ├─ call 0x7063B0   ; pre-pass
  ├─ call 0x706A00   ← FusionFix / Mode 45 hook site (0x706F7C)
  └─ test byte [edi+0x1C4], 4  ; post-flag
0x706A00  Fov recompute (this=esi=CCam)
  ├─ call 0x4D5A80   ; query helper (push 0,0)
  └─ call 0x67BD70   ; FOV path when helper ok
```

Only **one** static `E8→0x706A00` in the whole exe (`0x706F7C`). ASI chains here for `fovadd` ADD at `CCam+0x60`.

Mode **45** adds a **late head-owned CopyMat refresh** at the FOV site (after recompute). Does **not** decouple collision or create distinct eyes.

---

## VS upload / replay thread (same-frame critical path)

| RVA | Bytes @ RVA (anchor) | Role | Status |
|-----|----------------------|------|--------|
| `0x2CD80` | `55 8B EC 83 E4 F8 81 EC A0 03 00 00` | **VS upload wrapper** function start (contains VsRet + wrap) | READ-ONLY |
| `0x2D33E` | `85 C0 75 14 8D 44 24 0C …` | **VsRet** — return inside wrapper after `SetVSConstF` work | **READ-ONLY** anchor (~100k+/session) |
| `0x2D2AC` | `89 51 0A …` (`mov [ecx+0x0A], edx`) | Inner **VS wrap** slot/index write (same fn as VsRet) | **FORBIDDEN** (Mode 28 crash) |
| `0x2DB10` | callee from VsRet path | Inner VS const resolve helper (`lea esp+0xC; call`) | UNTESTED |
| — | Static **`E8 → 0x2D33E`** | Direct callers | **0 sites** on CE — indirect only |
| — | Static **`E8` into `0x2CD80–0x2D3FE`** | Sub-entry calls (9 sites) | See table below |

**VS wrapper region (`0x2CD80`–`0x2D3FE`) — offline call graph:**

| Caller RVA | Target | Enclosing fn | Note |
|------------|--------|--------------|------|
| `0x2BDC3` | `0x2C630` | `~0x2BDC0` | Near wrapper base |
| `0x2C0C0` | `0x2C690` | `~0x2BFC0` | Lands before forbidden `0x2D2AC` |
| `0x37BE0` | `0x2C7A0` | `~0x37620` | Same family as forbidden `0x387BD0` |
| `0x38062` | `0x2C700` | `~0x38006` | |
| `0x3B8DD` | `0x2C700` | `~0x3B8A0` | |
| `0x201B71` | `0x2C7A0` | ? | |
| `0x4FC0D2` | `0x2C6E0` | `~0x4FB8EC` | |
| `0x4FC3D7` | `0x2C6E0` | `~0x4FC360` | |
| `0x8A1D5E` | `0x2C7A0` | `~0x8A1CA0` | |

Prologue sets `this=0x110C0A0` (shader/device context global), stack `0x3A0`, then `call 0xA810` (technique lookup). **VsRet @ `0x2D33E`:** `test eax,eax; jnz` → else `lea eax,[esp+0xC]; push; call 0x2DB10` — D3D `SetVSConstF` return lands here on replay thread.

**Implication (L4D2VR / HL2VR lesson):** Source mods hook **`RenderView`** with static call graph. Rage uploads view constants on a **dedicated replay thread** through one wrapper; you cannot reach it with a simple `E8` scan. Same-frame distinct eyes need a **replay draw owner** called ~1×/frame with hookable ABI — not VsRet itself.

---

## VsRet stack chain (Mode 41/42 live log — READ-ONLY)

Mid return addresses seen when `HookSetVSConstF` returns to **`0x2D33E`** (not function starts):

| Mid RVA | Resolves / note | Status |
|---------|-----------------|--------|
| `0x22D87` | Epilogue `… 5F C2 10 00` — slot 10 | **FORBIDDEN** as hook target (not a start) |
| `0x2D2A0` | Callee near slot 10 | UNTESTED |
| `0x38801` | `E8` → **`0x387BD0`** — slot 15 | **FORBIDDEN** |
| `0x387BD0` | Wrong ABI prologue | **FORBIDDEN** (Mode 27/29 crash) |
| `0x38120` | Stackarg chain — slot 22 | **FORBIDDEN** |
| `0x37EB0` | CC-pad but stackarg | **FORBIDDEN** |
| `0x337D7` | → **`0x33640`** stackarg — slot 27 | **FORBIDDEN** |
| `0x33640` | `[ebp+8]` thiscall+stackarg | **FORBIDDEN** |
| `0x30D13` | Ret after **`call [eax+0x178]`** @ **`0x30D0D`** — Mode **42** OWNER-EDGE | READ-ONLY |
| `0x325CD3` / old `0x31913` | File-off-as-RVA noise — do not use | obsolete |
| `0x3259A` / `0x325A4` | Mid-body inside **`0x3247C`** (Mode 41 stack slots) | READ-ONLY |

---

## Indirect replay edge (corrected — READ-ONLY)

Real seam is **`call [eax+0x178]` @ `0x30D0D`** inside **`0x30CD0`**. Mode **42** watches stack ret **`0x30D13`** (instruction after that call). Old file-off labels **`0x31913` / `0x3010D`** were PE-skew noise.

| RVA | Bytes / insn | Role | Status |
|-----|----------------|------|--------|
| `0x30D0D` | `FF 90 78 01 00 00` | **`call [eax+0x178]`** — vtable replay dispatch | READ-ONLY anchor |
| `0x30D13` | follows call | Mode **42** OWNER-EDGE return address | READ-ONLY |
| `0x30CD0` | `cmp [0x17ed918],0; push esi; mov esi,ecx` | **Owner fn** of `0x30D0D` (NOT MatMul) | Mode **68** COUNT-only |
| `0x307F0` | `55 8B EC …` | **4×4 MatMul** — PublishSync / ViewConst callee | UNTESTED |
| `0x314C0` | frame thiscall | ViewMatWriter TRUE start (Mode **67**) | COUNT-only |
| `0x3259A` / `0x325A4` | mid-SSE in ViewConst | **Not** call returns — ignore as seams | obsolete labels |

**Correction:** MatMul **`0x307F0`** is **not** the owner of `0x30D0D`.

### `0x30D0D` dispatch (offline disasm @ **`0x30CD0`**)

```
0x30CD0  thiscall (esi=ecx) — replay device gate
  cmp [0x17ed918], 0          ; replay gate — skip when non-zero
  push esi / mov esi, ecx
  … optional call [0x18de9c8] ; callback if [0x18de9bc]/[0x18de9c4] set
  mov ecx, [0x17ed8d8]        ; singleton device/context object
  mov eax, [ecx]              ; vtable pointer
  lea arg, [esi+0x80]         ; sub esi,-0x80 → replay payload at this+0x80
  push 0 / push arg / push ecx
  call [eax+0x178]            ; 0x30D0D — vtable slot +0x178 (entry ~94)
  … second dispatch call [ecx+0x1b4] @ 0x30D20 (slot +0x1B4)
  ret                         ; 0x30D27
```

| Reg / global | Role |
|--------------|------|
| `[0x17ed8d8]` | Live device/context `this` |
| `eax` @ `0x30D0D` | **`[ecx]` = vtable** |
| `[eax+0x178]` | Virtual method — live target Mode **65** `slot178=0x220D0` (DXVK SetVSConstF) |
| `[0x17ed918]` | Gate — **skip** dispatch when **non-zero** |
| **2 static E8** → `0x30CD0` | SetActiveView `@0x30C92`, PublishSync `@0x30F53` |

**Mode 68 (implemented — opt-in stereo=68):** COUNT-only MinHook @ **`0x30CD0`** ReplayDispatch
prologue (`83 3D 18 D9 7E 01 00 56 8B F1`). thiscall-esi; **no** CC-pad required. Measures
avg entries/EndScene over 45 ES; PASS=`[0.8,4]`, SHARED=`>4`, REJECT=`<0.8`. Auto-reverts to
**`45`**. Dual=OFF. SameFrameSeamGate=CLOSED.

**ABI → slot178 `0x220D0`:** after gate, `mov ecx,[0x17ed8d8]`; `push 0x10` (16 floats);
`sub esi,-0x80` (= view+0x80); `push esi`; `push 0`; `push ecx`; `call [eax+0x178]` @ **`0x30D0D`**.

**Dual plan NOTES only (not implemented):** inject distinct L/R matrices at **view+0x80** after
gate / before call @ **`0x30D0D`** (~`0x30CFC`..`0x30D0D`). Do **not** open SameFrameSeamGate
until Mode **68** PASS + dual plan review.

**Next live step:** stereo **`68`** COUNT. Kill **`45`**.

---

## View-const apply @ `0x32470` TRUE start (Mode 64 — COUNT-only)

TRUE start is frame prologue **`0x32470`** (`55 8B EC …`); mid **`0x3247C`** is after `sub esp` — **do not hook mid**. Sole static `E8→0x32470` @ **`0x9777C2`** (fn **`0x977600`**) is gated by BSS **`[0x1797694]`** (no static writer → COUNT may be 0). Mode 41 mid-SSE slots **`0x3259A/A4`** are **not** call returns.

### ABI

| Reg / stack | Role |
|-------------|------|
| `ecx` / `edi` | **`this`** object (view-const / replay camera block) |
| `[edi+0x100..0x13C]` | Source view vectors (16× `movss` loads → stack `0x70..0xB0`) |
| `[ebp+8]` | Stack arg — pointer to 4× float input used in matrix multiply path |
| Returns | **`ret 4`** — stack cleanup; **no `push ebp`** prologue (uses caller `ebp`) |

Prologue: `56 57 8B F9 8D 44 24 70` — Mode 34 class **`thiscall-5657`** (CC-pad OK).

### Body summary (Capstone, fn len ~`0x397`)

1. Gather `[edi+0x100..0x13C]` → stack buffer  
2. `call 0x66DAE0` — helper  
3. Heavy `mulss`/`addss` block (view-projection style) using `[ebp+8]` row  
4. `call 0x417610` — normalize / helper  
5. `lea esi,[edi+0x1C0]` — **dest view matrix** on object  
6. `call 0x307F0` — **4×4 matrix multiply** into `[edi+0x1C0]`  
7. `call 0x31810`, `0x30F00`, `0x31BA0` — publish / follow-up (same tail as `0x314C0`)  
8. `ret 4`

### Publish tail callees (offline disasm — mapped RVAs)

| RVA | Prologue | Role | Stereo relevance |
|-----|----------|------|------------------|
| **`0x31810`** | mid-entry → **`0x31660`** `56 8B F1` | Writes view-projection block **`[this+0x1C0..0x1F8]`** from stack args; **`ret 0x18`** | **High** — final VP rows on view object |
| **`0x30F00`** | `55 8B EC 83 E4 F8 51 56 8B F1` | **PublishSync** — 3× **`0x307F0`** matrix sync + optional **`call 0x30CD0`** (see below) | **Highest** — bridge to **`call [eax+0x178]`** |
| **`0x31BA0`** | `55 8B EC … sub esp,0x88` | Builds 6×4 float block at **`[this+0x308..]`** (projection/tangent publish); may call **`0x31810`** when **`[this+0x3E0]==1`** | **Medium** — shader/canvas constants, not draw owner |

**Note:** Capstone labels **`call 0x430300`** etc. — those are **VA display noise** (image base `0x400000`). Correct **mapped RVAs** are **`0x30F00`**, **`0x31BA0`**, **`0x31810`**.

---

### Publish sync @ **`0x30F00`** (offline disasm — UNTESTED hook)

**Prologue → ret** (`fn len ~0x5D`, 30 insns):

```
0x30F00  thiscall (esi=ecx) — PublishSync
  push ebp / mov ebp,esp / and esp,-8 / push ecx / push esi / mov esi,ecx
  ; --- matrix sync via 0x307F0 (stack args left on stack between calls) ---
  lea ecx,[esi+0xC0];  push [esi+0x80]; push [esi+0x180]; call 0x307F0
      → [this+0xC0]  = MatMul([this+0x180], [this+0x80])
  lea ecx,[esi+0x100]; push [esi+0x1C0]; call 0x307F0   ; 2nd arg still [this+0x80] on stack
      → [this+0x100] = MatMul([this+0x1C0], [this+0x80])
  lea ecx,[esi+0x200]; push [esi+0x240]; call 0x307F0   ; 2nd arg [this+0x1C0] on stack
      → [this+0x200] = MatMul([this+0x240], [this+0x1C0])
  cmp [0x17F583C], esi
  jne 0x30F58                         ; skip replay unless active view
  mov ecx, esi
  call 0x30CD0                        ; replay vtable dispatch (see 0x30D0D)
  pop esi / mov esp,ebp / pop ebp
  ret
```

#### **`0x307F0` matrix multiply ABI** (28 static `E8` callers)

| Reg / stack | Role |
|-------------|------|
| `ecx` | Dest 4×4 matrix (`movaps` store to `[ecx]`) |
| `[ebp+8]` | Source matrix A (row-major 4×4) |
| `[ebp+0xC]` | Source matrix B |
| Prologue | `55 8B EC 83 E4 F8` — SSE `mulps`/`addps` dot-product rows |

PublishSync chains three multiplies reusing the **same stack arg** between calls (caller does not `add esp` between **`0x307F0`** invocations).

#### **`[0x17F583C]` — active view object pointer**

| Item | Value |
|------|-------|
| Role | Points at the **currently active** view/render-camera block (`this` for replay dispatch) |
| Static **writes** | **1** — `mov [0x17F583C], ecx` @ **`0x30C6F`** (fn **`0x30BF0`** view-activation path) |
| Static **reads** | **~116** (view switch, compare, Mode **65** `activeView=`) |
| PublishSync gate | **`0x30F49`**: `cmp [0x17F583C], esi` — **`call 0x30CD0` only when `this` is the active view** |
| Sibling compare | **`0x30BD0`**, **`0x3143F`** — same global, other branches |

---

### View activation @ **`0x30BF0`** (offline disasm — UNTESTED)

**Role:** **SetActiveView** — the **only** static writer of **`[0x17F583C]`**. Runs on view **switch** (camera/UI/game paths), **before** view-const apply (**`0x3247C`**) or PublishSync (**`0x30F00`**) publish matrix work on the already-active object.

**Prologue → ret** (`fn len ~0xBC`, 46 insns; **no `push ebp`** — standalone entry @ **`0x30BF0`**):

```
0x30BF0  cdecl SetActiveView(view, replayFlag) — view activation
  cmp [0x1B4BC50], 0          ; lazy-init guard ×4 (RTTI/name via call 0x35BB0)
  push esi
  mov esi, [0x17F583C]        ; save OLD active view → return value
  … call 0x35BB0 …            ; init [0x1B4BC50/58/54/4C] once each
  mov ecx, [esp+8]            ; arg0 = new view object
  mov [0x17F583C], ecx        ; 0x30C6F — ONLY static write to activeView
  mov byte [0x105BCA7], 1     ; view-active flag
  test ecx, ecx
  je 0x30CA8
  cmp byte [esp+0xC], 0       ; arg1 = replay trigger byte
  je 0x30CA8
  call 0x30E30                ; viewport dim sync (this=ecx)
  mov ecx, [0x17F583C]
  call 0x30CD0                ; 0x30C92 — replay vtable dispatch (same owner as PublishSync)
  mov ecx, [0x17F583C]
  add ecx, 0x280
  call 0x223D0                ; sub-object refresh on view block +0x280
0x30CA8  mov eax, esi          ; return previous active view
  pop esi
  ret
```

#### ABI

| Reg / stack | Role |
|-------------|------|
| **`[esp+8]`** | New view object pointer (written to **`[0x17F583C]`**) |
| **`[esp+0xC]`** | **`byte`** replay flag — non-zero + valid object → **`0x30E30`** + **`0x30CD0`** + **`0x223D0`** |
| **Returns `eax`** | Previous **`[0x17F583C]`** (saved in `esi` at entry) |
| Calling convention | **`cdecl`** — callers **`add esp, 8`** after call |
| Typical caller args | **`push 1; push viewPtr`** or **`push 1; push 0`** (deactivate) |

**Not a CC-pad thiscall** — stack args; **not** a Mode 34 COUNT/dual candidate without ABI reclass.

#### Callees (7 static `E8` from body)

| Call @ | Target | Role |
|--------|--------|------|
| **`0x30C07`**, **`0x30C24`**, **`0x30C3C`**, **`0x30C5E`** | **`0x35BB0`** | Lazy type/name lookup (one-time init of 4 globals) |
| **`0x30C87`** | **`0x30E30`** | Viewport dimension sync helper (`this=ecx`; compares **`[this+0x2B0/0x2B4]`**) |
| **`0x30C92`** | **`0x30CD0`** | **Replay dispatch** — only when arg1≠0 (paired with PublishSync **`0x30F53`**) |
| **`0x30CA3`** | **`0x223D0`** | Refresh sub-block at **`[view+0x280]`** |

#### Static **`E8 → 0x30BF0`** caller graph (**16 sites**)

| Call @ | Enclosing fn | Typical push pattern | Note |
|--------|--------------|----------------------|------|
| **`0x245FC`** | **`0x2405B`** | `1, 0` | Camera/view mgr |
| **`0x24D32`** | **`0x249C0`** | `1, 0` | Camera/view mgr |
| **`0x2FFDC`** | **`0x2FFD0`** | `1, 0` | **Sibling** — re-activate when **`[activeView]==ecx`** |
| **`0xB7F10`** | **`0xB7EF0`** | `1, 0` | Distant UI/render |
| **`0x1CBEEB`** | **`0x1CBB70`** | `1, ebx` | Vtable-driven switch |
| **`0x1CC2FD`** | **`0x1CC294`** | `1, 0` | |
| **`0x1E0957`** | **`0x1E090A`** | `1, 0` | |
| **`0x22C8C6`** | **`0x22C4D2`** | `1, viewPtr` | **Shared** with PublishSync distant caller |
| **`0x22CAAF`** | **`0x22C4D2`** | `1, 0` | Same fn — also calls **`0x30F00`** @ **`0x22D550`** |
| **`0x4D83F4`** | **`0x4D8310`** | `1, esi` | Reads **`[0x17F5838]`** sibling global |
| **`0x4DD20D`**, **`0x4DD21A`** | **`0x4DD150`** | `1, 0` / `1, ptr` | |
| **`0x52C404`** | **`0x52C139`** | `1, 0` | |
| **`0x6DD5FF`** | **`0x6DD5C0`** | `1, esi` | After vtable **`[eax+0x60]`** |
| **`0x6DD920`** | **`0x6DD89B`** | `1, 0` | After vtable **`[eax+0x64]`** |
| **`0x9BBB91`** | **`0x9BB570`** | `1, edi` | After **`0x30D40`** setup |

**Verdict:** activation is **game/camera-thread view switching**, not replay-thread matrix upload. **Zero** static **`E8`** from **`0x3247C`** or **`0x30F00`** to **`0x30BF0`**.

#### Sibling thunks (same **`0x30Bxx`** cluster)

| RVA | Role |
|-----|------|
| **`0x30BD0`** | **`cmp [0x17F583C], ecx; jne ret; push 1,0; call 0x30BF0`** — idempotent re-activate current view |
| **`0x30B97`** | Wrapper: stack setup → **`call 0x31110`** → `mov eax, esi; ret` (separate from **`0x30BF0`** body) |
| **`0x3143F`** | Inside viewport/resize **`0x31110`**: optional **`call ViewMatWriter`** @ **`0x3143A`**, then **`cmp esi,[0x17F583C]; call 0x22FD0 [view+0x280]`** if active — **no** **`0x30CD0`** |

#### Relationship to **`0x3247C`** / **`0x30F00`**

```
  [view switch — 16 callers]
       │
       ▼
  0x30BF0  SetActiveView
       ├─ mov [0x17F583C], view     @ 0x30C6F  (only write site)
       └─ if replayFlag: call 0x30CD0 @ 0x30C92  ──┐
                                                    │
  [per-frame matrix — replay thread family]         │
       │                                            │
       ├─ 0x3247C  view-const apply                 │
       │     └─ tail: call 0x30F00 @ 0x327FF       │
       ├─ 0x314C0  view-matrix writer              │
       │     └─ tail: call 0x30F00 @ 0x31624       │
       └─ 0x30F00  PublishSync                     │
             cmp [0x17F583C], this @ 0x30F49        │
             call 0x30CD0 @ 0x30F53  ───────────────┘
                    │
                    ▼
             0x30CD0 → call [device+0x178] @ 0x30D0D
```

| Question | Answer |
|----------|--------|
| Does **`0x3247C`** call **`0x30BF0`**? | **No** — **`0x3247C`** only calls **`0x30F00`** (and mat helpers) |
| Does **`0x30F00`** call **`0x30BF0`**? | **No** — PublishSync assumes view already active |
| Shared caller with PublishSync? | **`0x22C4D2`** only (distant path; calls both activate + publish) |
| Two **`0x30CD0`** entry points | **`0x30C92`** (on switch + flag) and **`0x30F53`** (on publish + active match) |
| Mode **65** `activeView=` | Live pointer written here; must match view object in Mode **64**/**66** COUNT epochs |

**Stereo implication:** replay draw fires on (a) **view switch** when callers pass **`replayFlag=1`**, and (b) **every PublishSync** on the active view object. Mode **66** COUNT @ **`0x30F00`** measures publish cadence; activation cadence is separate (16 call sites, likely ≪1×/frame except camera cuts).

Mode **65** logs `activeView=0x…` (RVA of live object). When PublishSync runs on the active view, expect **`activeView` RVA == view object** passed to **`0x30CD0`**.

#### **`call 0x30CD0` conditions (replay dispatch)**

Two static **`E8 → 0x30CD0`** sites:

| Call @ | Enclosing | When |
|--------|-----------|------|
| **`0x30F53`** | **`0x30F00`** | After matrix sync; **`[0x17F583C] == this`** |
| **`0x30C92`** | **`0x30BF0`** | After view activation sets **`[0x17F583C]`**; replay gate **`[0x17ED918]==0`** and flag byte checks |

Inside **`0x30CD0`** (before **`call [eax+0x178]` @ `0x30D0D`**):

1. **`cmp [0x17ED918], 0`** — if **non-zero**, skip all dispatch (`ret` @ `0x30D26`)  
2. Optional callback **`call [0x18DE9C8]`** if **`[0x18DE9BC]`** or **`[0x18DE9C4]`** set  
3. Load device **`[0x17ED8D8]`**, **`call [vtable+0x178]`**, then paired **`call [vtable+0x1B4]`**

**Stereo implication:** replay draw/upload fires only for the **active** view object, and only when replay gate is clear.

#### Static **`E8 → 0x30F00`** caller graph (**12 sites**)

Most sites end with `movss [this+0x1F8],xmm1; call PublishSync; mov ecx,this; call PublishProj`.  
**11/12** also call **`0x31BA0`** within +16. Orphan (PublishSync only): **`0x3199F`**.

| Call @ | Enclosing / note |
|--------|------------------|
| **`0x31624`** | ViewMatWriter **`0x314C0`** (Mode **67**) |
| **`0x327FF`** | ViewConst **`0x32470`** tail (Mode **64** family) |
| **`0x317C5`**, **`0x317FB`**, **`0x318C9`**, **`0x3192A`**, **`0x31B0C`**, **`0x31B8B`** | Publish/VP family (same `+0x1F8` store tail) |
| **`0x3199F`** | Small copy helper — PublishSync **only** (no PublishProj) |
| **`0x21922D`**, **`0x219C84`**, **`0x22D550`** | Distant publish family |
| **`0x31B0C`**, **`0x31B8B`** | **`0x319B6`** | Publish family |
| **`0x21862D`** | **`0x218491`** | Distant game-render path |
| **`0x219084`** | ? | Distant |
| **`0x22D550`** | **`0x22C4D2`** | Distant |

**Verdict:** PublishSync is a **shared** post-matrix hook used by both **`0x3247C`** and **`0x314C0`** tails plus VP/publish helpers — not exclusive to the view-const candidate. COUNT @ **`0x3247C`** (Mode **64**) measures view-const cadence only; COUNT @ **`0x314C0`** (Mode **67**) measures projection-writer cadence; replay dispatch cadence may differ on other callers.

**Mode 66 (implemented — opt-in stereo=66):** COUNT-only @ **`0x30F00`** PublishSync prologue to measure avg entries/EndScene (12 static **`E8`** callers). Does **not** hook **`0x30CD0`** or forbidden sites. Auto-reverts to **`45`** after 45 EndScenes. Ladder step — **PASS** live 2026-07-25.

**Mode 67 (implemented — opt-in stereo=67):** COUNT-only @ **`0x314C0`** ViewMatWriter TRUE start. Auto-reverts to **`45`**. Live COUNT **REJECT** (sparse).

**Mode 68 (implemented — opt-in stereo=68):** COUNT-only @ **`0x30CD0`** ReplayDispatch (bridge PublishSync → slot178 **`0x220D0`**). Auto-reverts to **`45`**. **Next headset test.**

### Static call graph

| Direction | RVA | Note |
|-----------|-----|------|
| **E8 → `0x32470` TRUE** | **1 site** `@0x9777C2` | Gated by BSS `[0x1797694]` (no static writer) |
| **`0x32470` → callees** | includes MatMul **`0x307F0`**, PublishSync **`0x30F00`**, PublishProj **`0x31BA0`** | |
| Shared tail with | `0x314C0` | Both call **`0x30F00`** + **`0x31BA0`** after matrix write |

**Cadence:** Mode **64** COUNT @ `0x32470` may be **0** (gate). Prefer Mode **66**.

### COUNT-only hook safety (Mode 64 / Mode 66 / Mode 67 / Mode 68)

| Check | Mode **64** @ `0x32470` | Mode **66** @ `0x30F00` | Mode **67** @ `0x314C0` | Mode **68** @ `0x30CD0` |
|-------|-------------------------|-------------------------|-------------------------|-------------------------|
| Aligned prologue | **YES** | **YES** frame | **YES** frame | **YES** `83 3D…56 8B F1` |
| CC-pad thiscall | **YES** | relaxed (epilogue neighbor) | **YES** | N/A (no classic frame) |
| Forbidden list | **Not listed** | **Not listed** | **Not listed** | **Not listed** |
| Dual / replay hook | **NO** — COUNT only | **NO** — no **`0x30CD0`** | **NO** — COUNT only | **NO** — COUNT only |
| Crash history | none (COUNT 0) | **PASS** live | REJECT sparse | untested live |
| Do **not** hook | mid **`0x3247C`** | — | mid **`0x314C9`** | mid after `mov esi` |

**Verdict:** COUNT-only OK for **64/66/67/68**; **not hook-safe for dual** until Mode **68** PASS + dual plan review. SameFrameSeamGate=CLOSED.

### Sibling `56 57 8B F9` thiscall writers (`0x30000–0x34000` scan)

**CORRECTION (2026-07-25 ~01:10):** the ViewMatWriter TRUE start is **`0x314C0`** (`55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9`, CC-pad). Older notes labeled mid **`0x314C9`** (`56 57 8B F9` after frame setup) — that is **not** a MinHook-safe start (no CC-pad; epilogue uses ebp from the real prologue).

| RVA | `[edi+0x100]` view loads | `[edi+0x1C0]` matrix writes | Role | COUNT? |
|-----|--------------------------|-----------------------------|------|--------|
| **`0x3247C`** | **16** `movss` loads | via `call 0x307F0` | View-const apply → mat @ `+0x1C0` | **Mode 64** target |
| **`0x314C0`** | 0 | **1+** stores `+0x1C0..0x1F8` | ViewMatWriter / projection builder → **`call 0x30F00` + `0x31BA0`** | **Mode 67** target |
| `0x33FE6` | 0 | 0 | `[edi+6]` test — unrelated | reject |

**`0x314C0` static facts:**

| Item | Value |
|------|-------|
| Prologue | `55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9` (frame thiscall, `this=edi`) |
| CC-pad | **YES** (`CC CC` immediately before) |
| Static **`E8 → 0x314C0`** | **9** sites (2 inside viewport **`0x31110`**: `@0x3143A` optional + `@0x314B0`; 7 distant) |
| Dirty flag before call | Most distant callers set **`[view+0x2F0]=1`** or **`[view+0x300]=1`** then `call ViewMat` — rebuild/dirty bit, not seam |
| Tail | `call 0x30F00` @ **`0x31624`**, `call 0x31BA0` @ **`0x3162B`**, plain `ret` |
| Mid mislabel | **`0x314C9`** = first SSE body insn — **do not hook** |
| Caller list | `0x3143A` `0x314B0` `0xB8E43` `0xBA93F` `0x1D4A5B` `0x1D4BCB` `0x1D4D30` `0x21BEF3` `0x21C18F` |

No additional hidden view writers near **`0x3247C`**. Shared publish tail: both **`0x3247C`** and **`0x314C0`** call **`0x30F00`** + **`0x31BA0`**.

**Mode 67 (implemented — opt-in stereo=67):** COUNT-only @ **`0x314C0`** ViewMatWriter prologue. Auto-reverts to **`45`**. Ladder step after **66**.

### Device singleton `[0x17ed8d8]` (offline PE)

| Item | Value |
|------|-------|
| Static code **reads** | **247** (`mov reg,[0x17ed8d8]` / `A1`) |
| Static code **writes** | **2** — init **`0x23A48`** `mov [device], 0x18DC2B8`; teardown **`0x24DA8`** `mov [device], 0` |
| Replay gate | **`[0x17ed918]`** — **`0x30CD0`** skips dispatch when non-zero |
| Vtable @ **`0x30D0D`** | **`call [eax+0x178]`** (slot **94**); paired **`call [ecx+0x1B4]`** @ **`0x30D20`** |
| Max **`FF /2`** disp32 slot (exe-wide) | **`0x248`** (~entry **146**) |
| Live read plan | Mode **65** — EndScene read-only log of device/vtable/**`+0x178`**/**`+0x1B4`** (no hook) |

### Safe live experiment ladder (no dual — one mode per session)

Deploy latest `out-asi\gtaiv_dxvk_vr.asi` + `openvr_api.dll`. Edit **`gtaiv_dxvk_vr.stereo`** next to EXE (single integer). **Kill anytime:** **`45`** or delete file.

| Step | stereo | Duration | Exact log lines to capture | Pass criterion |
|------|--------|----------|----------------------------|----------------|
| **0 Baseline** | **`45`** | 2+ min calm | `StereoMode: 45` · `StereoSubmit: L=1 R=1 mode=45` | Smooth; no freeze |
| **1 PublishSync COUNT** | **`66`** | ≥45 ES | **`Mode66: COUNT … cadence=PASS`** @ `0x30F00` | **DONE PASS** avg=3.00 |
| **2 ViewMat COUNT** | **`67`** | ≥45 ES | **`Mode67: COUNT …`** @ `0x314C0` | **DONE REJECT** avg=0.16 |
| **3 Vtable peek** | **`65`** | ≥45 ES | **`Mode65: … slot178=0x220D0`** | **DONE OK** |
| **4 ViewConst COUNT** | **`64`** | ≥45 ES | **`Mode64: COUNT …`** @ `0x32470` | **DONE REJECT** (gate) |
| **5 Owner edge** | **`42`** | 1 calm min | **`Mode42: OWNER-EDGE …`** | **DONE dry** (VsRetHits=0) |
| **6 ReplayDispatch COUNT** | **`68`** | ≥45 ES | **`Mode68: COUNT armed exeRva=0x30CD0`** · **`cadence=PASS\|SHARED\|REJECT`** | **NEXT** — avg in **[0.8,4]** = PASS |

After Mode **68**: if PASS, dual inject notes at view+0x80 remain CLOSED until reviewed. Auto-revert to **`45`**. **No dual.** **SameFrameSeamGate=CLOSED.**

---

## View-const apply candidates (NOT forbidden — UNTESTED)

Rage has no Source `RenderView`. These are **static** candidates for per-draw / replay view-constant work distinct from CopyMat:

| RVA | Prologue / anchor | AOB / note | Why candidate | Status |
|-----|-------------------|------------|---------------|--------|
| `0x32470` | `55 8B EC 83 E4 F0 81 EC A8…` | ViewConst TRUE start | Mode **64** COUNT (gated caller) | COUNT-only |
| `0x3247C` | `56 57 8B F9…` | Mid after `sub esp` — **unsafe hook** | Mid-SSE `0x3259A/A4` here | do not hook |
| `0x30D0D` | `FF 90 78 01 00 00` | `call [eax+0x178]` | Only `FF 90` in `0x30000–0x32000` — replay vtable | READ-ONLY |
| `0x37CD0` | `56 57 8B 7C 24 08` | Helper prologue | Mode 34 helper — no dual observed | UNTESTED |
| `0x530FD0` | `55 8B EC …` stack `0xB4` | Mode 34 rare ~1×/frame | Large stack — ABI unknown | UNTESTED |
| — | string `viewProj` @ file `0xAA60A4` | Debug/RTTI hint | Near shader constant naming | data only |

**Rejected as hook targets:** `0x4D9B10` (mid SSE, avg 0.31/frame), `0x52F3C0` (stackarg), `0x4DE6D0` (dual dead), `0x387BD0` / `0x2D2AC` (crash).

---

## Indirect edge @ `0x315D0` (Mode 42 — READ-ONLY, superseded)

| RVA | Note | Status |
|-----|------|--------|
| `0x315D0` | Mid-instruction / inner block (not a clean prologue) | **UNTESTED** — do not call blindly |
| `0x30D13` | Mode **42** OWNER-EDGE ret; call is **`FF /2 @ 0x30D0D`** `modrm=0x90` → **`call [eax+0x178]`** | READ-ONLY |

**Next RE step (no hook yet):** at live `0x30D0D`, record **`eax`** (object) and **`[eax+disp32]`** vtable target per frame — proves owner ABI/lifetime. Static PE alone cannot resolve indirect vtable calls.

---

## Forbidden same-frame / dual targets (do not hook)

| RVA | Why |
|-----|-----|
| `0x2D2AC` | Wrap hook → crash after load (Mode 28); old file-off `0x2C6AC` |
| `0x387BD0` | Wrong ABI / crash (Mode 27/29); old file-off `0x37BD0` |
| `0x1BFC10` | Mode 34 COUNT hard-kill (SSE stackarg misclassified); old file-off `0x1BF010` |
| `0x4DE6D0` | Mode 34 dual armed; **`vsPatch=0` / `vsCallsR=0`** forever; old file-off `0x4DDAD0` |
| `0x315D0` | Observation only until indirect owner proven; old file-off `0x309D0` |

---

## Game-thread render roots (BuildRootA path — dual-list work)

*mapped RVA (+0xC00 from older file-off notes)*

| RVA | Prologue / anchor | Role | Status |
|-----|-------------------|------|--------|
| `0x8F8B00` | `55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08` | **BuildRootA** — 13-phase frame build (~1×/frame) | READ-ONLY (Mode 19/20); dual build-only = mono |
| `0x8F7FB0` | near `0x8F7C33`: `53 55 56 57 8B F1` | **ExecRoot** dispatcher — vt[9] Execute | READ-ONLY; 2nd call = UAF (Mode 22) |
| `0x8F8061` | caller cluster | Multiple `E8→BuildRootA` from `0x8F8220` etc. | **10** static BuildRootA call sites, all inside `0x8F8xxx` |
| `0x5C2F80` | FrameA path (len ~`0x866`) | Game-thread frame driver | READ-ONLY (Mode 19) — **no** direct `E8→BuildRootA` |
| `0x5C37F0` | mid-instr in CE scan | Documented “BuildWrapB” — **verify** before hook | DRIFT risk — not a clean prologue in static PE |
| `0x118D7F0` | Global **render-phase manager** | `this` for build/exec | data |
| `0xD8D8C0` / `0xD8D9C0` | Manager **`+0xD0` / `+0x1D0`** pos rows | Live cam WORLD copies (Mode 21) | Shifting at exec did not change draws |

**BuildRootA offline disasm (`0x8F8B00`):** `this=esi=ecx` (render manager), `[ebp+8]` phase descriptor; first checks `[edi+0x387]` with `cmp/jg`; phase loop uses **`call [ecx+disp]`** (vtable) at `~0x8F8B54` → **`call 0x755C70`**. **ExecRoot `0x8F7FB0` has 0 static E8 callers** — reached only via vtable/indirect.

**Correction:** No `mov ecx,0x118D7F0; call BuildRootA` pair exists in static PE. Manager `this` is passed through the `0x8F8xxx` dispatch chain (135 `mov ecx,0x118D7F0` sites elsewhere call **`0x4F7C00`** family helpers, not BuildRootA directly).

**BuildRenderList phase AOBs** (mid-body — FusionFix-safe):

| Phase | Mid RVA | Mid AOB (unique tail) | Fn start (mid − offset) |
|-------|---------|------------------------|---------------------------|
| DrawScene | `0x6DD20D` | `… FF 0F 84 DE 09 00 00 6A 00 6A 0C` | **`0x6DD200`** |
| PhaseA | `0x528ADE` | `… FF 0F 84 76 03 00 00 80 3D` | **`0x528AC0`** (−30) |
| PhaseC | `0x976977` | `… FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | **`0x976950`** (−39) |

RTTI strings (file offset): `.?AVCRenderPhaseDrawScene@@` @ `0xC3E664`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@` @ `0xC55640`.

**Hard finding:** BuildRootA ×2 (Mode 20) removes temporal jump but **does not** produce distinct eyes — draw lists double-buffer; view constants bake at **build**. Exec re-call from build thread = cross-thread crash. Real draws + VS uploads align on **replay thread**, not BuildRootA.

---

## Mode 34 COUNT candidates (historical)

*mapped RVA (+0xC00 from older file-off notes)*

| RVA | Mode 34 note | Status |
|-----|--------------|--------|
| `0x4D9B10` | avg ≈ 0.31 entries/frame — not ~1× | REJECT |
| `0x52F3C0` | ~1×/frame but stackarg | **FORBIDDEN** |
| `0x530FD0` | Rare agg ~1× | UNTESTED |
| `0x37CD0` | Helper `56 57 8B 7C` — no dual | UNTESTED |

---

## Ped / presentation (SAFE)

*mapped RVA (+0xC00 from older file-off notes)*

| RVA | AOB | Role | Status |
|-----|-----|------|--------|
| `0x7B538C` | `E8 ? ? ? ? 83 C4 04 85 C0 74 ? 8B 0D` | SetDraw-style call site (PedHide) | **SAFE** |
| `0x66E8D0` | (callee from above) | CE draw-component helper | **SAFE** via AOB |

---

## Cross-mod RE targets (what we still need)

| Reference mod | Their seam | Rage equivalent (this project) |
|---------------|------------|--------------------------------|
| **L4D2VR / HL2VR** | Two **`RenderView`** / **`CViewSetup`** per tick | **Missing** — only CopyMat + VS observation |
| **BotW BetterVR** | PPC patches at **GPU fixed-function draw** | Mode 55 tried VS translate-only; parallax partial, controls fragile |
| **UEVR** | Native per-eye view + projection | No UE path — temporal pair-hold only |
| **Halo MCC VR** | Same-frame + wide FOV | FOV via **`0x706F7C`** OK; same-frame **blocked** |

### Overnight marathon 2026-07-25 (~18:00–18:30) — Approaches A–G

`py -3 scripts/offline-world-draw-seam.py` → `docs/_re_scratch/world_draw_seam_report.txt`

| Fact | Value |
|------|-------|
| BuildRootA `0x8F8B00` | prologue **MATCH**; **10** static `E8` callers (all in `0x8F8xxx`) |
| DrawScene `0x6DD200` | prologue **MATCH**; **0** static `E8` (vtable — Mode77 hooks via AOB) |
| PhaseA / PhaseC | MATCH |
| Denser Mode75 fearvr 1/2 | **SCRAPPED** (headset LESS 3D) |
| Mode77 DrawScene×2 | Log-proven: `DRAWSCENE-ONLY dual #` + StereoDiff ≫0 |
| Viewport/SBS (B) | Exhausted — no parallax without second world draw |
| SameFrameSeamGate | Still **CLOSED** for ungated every-frame BuildRootA dual |

**SameFrameSeamGate:** `CLOSED` until a replay-thread owner satisfies:

1. ~1×/frame cadence on VsRet thread  
2. Zero-arg or CC-padded **thiscall** (no stackarg / no SSE misclass)  
3. COUNT-only ≥45 EndScenes without exception  
4. Dual trial shows **`vsPatch>0`** or distinct L/R canvas diff — **never** satisfied by `0x4DE6D0`  
5. Not in **FORBIDDEN** table above  

ASI stereo modes **`62`** / **`63`** scaffold Mode 45 + RE logs; **`66`** COUNT @ **`0x30F00`** (**PASS**); **`67`** COUNT @ **`0x314C0`** (REJECT); **`65`** VT178 peek (**`slot178=0x220D0`**); **`64`** COUNT @ **`0x32470`** (REJECT gated); **`68`** COUNT @ **`0x30CD0`** (**next**). Default **`45`**. Next: **`45→68`**.

### Offline mapped seam scan (2026-07-25 ~02:00)

`py scripts/offline-seam-mapped.py` confirmed:

| Fact | Value |
|------|-------|
| MatMul mapped | **`0x307F0`** (old typo `0x307BF0`) |
| `E8 → ReplayDispatch 0x30CD0` | **exactly 2** — SetActiveView `@0x30C92`, PublishSync `@0x30F53` |
| `E8 → PublishSync 0x30F00` | **12** (incl. ViewConst `@0x327FF`, ViewMat `@0x31624`) |
| `E8 → ViewConst TRUE 0x32470` | **1** (`@0x9777C2` in fn **`0x977600`**) — **gated** by `cmp byte [0x1797694],0` before call — COUNT may be sparse |
| `E8 → ViewMatWriter 0x314C0` | **9** |
| Mode41 `0x3259A/A4` | Mid-SSE inside ViewConst — **not** call returns |
| Mode42 `0x30D13` | Correct mapped ret after `call [eax+0x178]` |
| Prefer COUNT order | **66** PublishSync (12 callers) before **64** ViewConst (1 gated) for cadence signal |
| ViewConst gate | Absolute VA **`[0x1797694]`** — `cmp` before sole `E8→0x32470` @ **`0x9777C2`** (fn **`0x977600`**). BSS zero at load. **Only static ref = that cmp** (no `mov`/`movss`/`c7` writer). Neighbors in `0x1797654..0x17976D4` **do** have writers (`movss`/`a3`/`c7`) — gate is uniquely unread-write. Mode **64** may always be **0**. Prefer **66/67**. **Control flow:** `cmp byte [gate],0; je skip` — **gate!=0 required** to reach `E8→0x32470` @ `0x9777C2`. Parent fn `0x977600` has **0** static E8 callers (indirect). |
| PublishProj | Mapped **`0x31BA0`** (old file-off `0x30FA0`) — **11** E8 callers; **11/12** PublishSync sites call PublishProj within +16 bytes. Orphan PublishSync-only: **`0x3199F`**. |
| `call [reg+0x178]` exe-wide | **12** sites (see list below). Stereo seam = **`0x30D0D`**. VsRet obs = **`0x2D33E`**. |
| `0x22FD0` | Active-view **`[this+0x280]`** apply; **2** E8 callers (`0x30CA3` SetActive, `0x3144D` viewport) — **not** ReplayDispatch |
| activeView → PublishSync-only | Thunks **`0x32B40`** / **`0x32B60`** load `[0x17F583C]` then **`call 0x31940`** (matrix copy → PublishSync @ **`0x3199F`**, no PublishProj). Helper **`0x31940` has 58 E8 callers** — Mode **66** COUNT may see **avg ≫ 1** (shared publish, not 1×/frame owner). |
| Helper **`0x31940` body** | Copy dwords from `[esp+4]` → `this`; `lea +0x80; call 0x30720` (small copy); **`call PublishSync 0x30F00`**; `retn 4`. Not MatMul. |
| Thunk **`0x32B40`** | `mov ecx,[activeView]; push 0x1110090; call 0x31940` — copies **global matrix buffer** into active view then publish. Global has **121** imm32 refs. |
| Thunk **`0x32B60`** | `push [esp+4]; mov ecx,[activeView]; call 0x31940` — copy caller-supplied src into active view. |
| Exactly **3** MatMul×3 clusters | (1) PublishSync; (2)(3) both inside fn **`0x2A1E10`** at `@0x2A2110…`→`+178@0x2A217D` and `@0x2A258C…`→`+178@0x2A25F9`. |
| ReplayGate-guarded +178 | Only **3** of 12 `call [reg+0x178]` sites have `cmp [0x17ED918]` within −0x40: **`0x30D0D`**, **`0x2A217D`**, **`0x2A25F9`**. These are the stereo-relevant uploads. |
| MatMul+178 upload fn | **ONE** giant vtable method **`0x2A1E10`** contains **two** MatMul×3→`+178` paths: `@0x2A217D` (after helper `@0x2A1F85`) and `@0x2A25F9` (second path; no second helper). Sibling **`0x2A1D50`** helper-only. False `C3` mid-SSE (`0F 28 C3`) is not a ret. OWNER-EDGE `0x30D13` misses both. Vtable: sibling `@…C8→0x2A1D50`, upload `@…CC→0x2A1E10` (`0xAA83C8/CC`, `0xBE2560/64`, `0xBE3FA8/AC`). Ends `retn 0x14` @`0x2A2657` (thiscall + stack args). Prologue: `[ebp+8]`/`[ebp+0xC]`; early je if arg0 NULL. Both +178 sites: `cmp [activeView],esi` (@`0x2A213D` / `@0x2A25B9`) then **inline ReplayDispatch-equivalent** (`cmp [0x17ED918]`, ReplayAlt, device `+0x178/+0x1B4`). 17 activeView refs in fn. Ownership `cmp [activeView],esi` matches PublishSync @0x30F49 before upload. Mid-fn `mov eax,[ebp+8]; test eax,eax; je @0x2A227A→0x2A24F0` — NULL arg takes second +178 path. |
| SetActiveView chain | `0x30BF0`: write `[0x17F583C]` @`0x30C6F` → `call 0x30D30` → **`call ReplayDispatch 0x30CD0`** → `call 0x22FD0`. (Other ReplayDispatch caller = PublishSync when active.) |
| Future COUNT candidate: ReplayDispatch | `0x30CD0` — only **2** E8 callers; early-out on `[0x17ED918]!=0`. Writers @ `0x25562` (mov 0) / `0x44A6E` (mov ecx). Do **not** add Mode until after 66/67 live. |
| dirty bit +0x2F0 | mov byte [view+0x2F0],0/1 — 11 sites; near ViewMat/PublishSync cluster clears/sets before rebuild+publish. |
| Mode64/66/67 AOB unique | Full Mode patterns = **1 hit each** @ 0x32470 / 0x30F00 / 0x314C0. Short PublishSync prologue alone = 21 hits — ResolveReSite prefers expected RVA. |
| Mode **66** cadence labels | PASS `[0.8,4]` · **SHARED** `>4` (expected if helper hot) · REJECT `<0.8` — SHARED is still useful, not a failure. |

**All `call [reg+0x178]` (mapped):**  
`0x25720`, `0x2C738`, **`0x30D0D`** ← ReplayDispatch, `0x3725E`, `0x37BFB` (near FORBIDDEN `0x37BD0`), `0x635DB`, `0x29EE34`, `0x2A217D`, `0x2A25F9`, `0x2A3675`, `0x9B18B6`, `0x9F1D1A`.  
Device-singleton `[0x17ed8d8]` nearby: **`0x30D0D`**, `0x3725E`, `0x37BFB`, `0x635DB`, `0x29EE34`, `0x2A217D`, `0x2A25F9`.  
Paired `+0x1B4` at ReplayDispatch: **`0x30D20`**. Mode **65** logs live vtable target; Mode **42** confirms stack ret **`0x30D13`**. Do **not** hook `0x37BFB` (FORBIDDEN neighborhood).

---


**PublishProj `0x31BA0`:** frame thiscall; early `cmp byte [this+0x3E0],1` (skip if clear); `call 0x31810` + MatMul `0x307F0`; ret ~`0x31CED`. Runs after Sync on 11/12 sites. Flag byte writers: 11 `mov [reg+0x3E0],imm` sites (not only 0/1).

### PublishSync E8 site table (mapped — 12 call sites)

| Call site | Enclosing (CC-pad / known) | +PublishProj | Role |
|-----------|----------------------------|--------------|------|
| **`0x31624`** | ViewMatWriter **`0x314C0`** (Mode **67**) | yes | ViewMat tail |
| **`0x317C5`** | near **`0x31750`/`0x317E0`** cluster | yes | local matrix helpers |
| **`0x317FB`** | **`0x317E0`** | yes | local matrix helpers |
| **`0x318C9`** | **`0x31880`/`0x318E0`** | yes | local matrix helpers |
| **`0x3192A`** | **`0x318E0`** | yes | local matrix helpers |
| **`0x3199F`** | **INSIDE helper `0x31940`** | **no** | orphan PublishSync-only |
| **`0x31B0C`** | **`0x319B0`** | yes | larger publish builder |
| **`0x31B8B`** | **`0x319B0`** | yes | larger publish builder |
| **`0x327FF`** | ViewConst TRUE **`0x32470`** (Mode **64**) | yes | gated view-const tail |
| **`0x21922D`** | **`0x219070`** | yes | distant |
| **`0x219C84`** | near **`0x219xxx`** | yes | distant |
| **`0x22D550`** | distant | yes | distant |

**Count meaning for Mode 66:** MinHook on PublishSync prologue sees **all** of the above plus every path that reaches the internal `@0x3199F` via helper **`0x31940`×58**. Expect **SHARED** (avg>4) often.

**Related shared helpers:** `0x319B0` (17 E8, two Sync+Proj tails); `0x31810` (44 E8 dword copy, often pre-Sync); `0x317E0` (2 E8).

## How to re-find after Rockstar patch

1. Confirm image size / version string changed.  
2. `py scripts/offline-re-scan.py` — refresh AOB hits and anchor bytes.  
3. Update **expected RVA** column in this doc + `re_validate.cpp` `ExpectedSite` table.  
4. Re-run Mode **41**/**42** for one calm minute; paste new `Mode41: CHAIN` / `Mode42: OWNER-EDGE` rows.  
5. Optional Mode **64**: set `gtaiv_dxvk_vr.stereo` to **`64`**, play ≥45 EndScenes, read `Mode64: COUNT` lines (auto-writes **`45`** when done).  
6. Do **not** hook new RVAs until COUNT-only + ABI class passes Mode 33/34 filters.

---

## Quick forbidden list (paste for agents)

```
0x2D2AC  0x387BD0  0x1BFC10  0x4DE6D0  0x315D0(hook)  Mode34 dual @0x4DE6D0  0x315D0 blind call
```

Safe production stack: stereo **`45`**, FOV site **`0x706F7C`**, CopyMat **`0x83DB90`**, VsRet observation **`0x2D33E`** only.

---

## World visual size (does not fix same-frame jump)

Three **independent** levers — do not combine aggressively in one test:

| Lever | File / key | Effect | vs perfect VR |
|-------|------------|--------|-----------------|
| **F7 leanGain** | `gtaiv_dxvk_vr.scale` (×100) | 6DoF head-lean only; `eyeDelta = hmdDelta / leanGain`; higher = smaller world when leaning | Does not change building scale or stereo epoch |
| **F6 stereoscale** | `gtaiv_dxvk_vr.stereoscale` | Soft IPD/disparity multiplier (100–130%) | Can affect fusion if too high |
| **fovadd** | `gtaiv_dxvk_vr.fovadd` | Engine FOV ADD at **`0x706F7C`** site → true canvas fill | Kills black bars; wide FOV can add temporal stress |

**Rule:** F7 ≠ Halo/Luke “world scale”. Real building size needs engine FOV (`fovadd`) + eventual same-frame stereo. Mode **45** keeps all three decoupled.

**F7 preset ladder (Mode 45, 11 steps to 30.0):**  
`1.0 → 1.25 → 1.5 → 2.0 → 2.5 → 3.0 → 4.0 → 5.0 → 8.0 → 12.0 → 30.0`  
File stores **leanGain×100** (100 = normal; max 3000 = 30.0).

