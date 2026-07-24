# RE offset map — GTA IV CE (Steam)

**Binary:** `GTAIV.exe` (PlayGTAIV.exe is byte-identical on CE 1.2.0.59)  
**Image base:** `0x00400000` (fixed; no ASLR on main module)  
**Image size:** `0x01BE6400`  
**Last verified:** 2026-07-25 ~07:30 (full **`0x2FFF0`** view-activation disasm + 16 caller graph + **`0x3187C`/`0x30300`** relationship; **no game launched**)

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

**CopyMat ABI (offline disasm @ `0x83DB90`):**

| Reg / stack | Role |
|-------------|------|
| `ecx` | Dest cam matrix (`CCam` internal mat base in `edi` after prologue) |
| `[ebp+8]` | Source 4×3 row-major matrix pointer (`esi`) |
| Returns | Writes rows to `[edi+16..44]` via `movss`/`mov` — position row +3 at `+16`, right `+20/+24`, up `+32/+36`, at `+44` |

Inner call: `call 0x4D5720` (matrix helper) before row copy. **29 static `E8` callers** in CE exe; production hooks only the four follow-cam sites below.
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
| `0x706174` | enclosing fn of FovSite | CCam update thunk — `mov ecx,edi` before both calls | READ-ONLY |
| — | Classic pattern `E8 ? ? ? ? 8B CE E8 …` | Pre-CE layout | **MISS** on this CE build |

**FovSite call graph (offline disasm):**

```
0x706174  CCam update (this=edi)
  ├─ call 0x7063B0   ; pre-pass
  ├─ call 0x706A00   ← FusionFix / Mode 45 hook site (0x70637C)
  └─ test byte [edi+0x1C4], 4  ; post-flag
0x706A00  Fov recompute (this=esi=CCam)
  ├─ call 0x4D5A80   ; query helper (push 0,0)
  └─ call 0x67BD70   ; FOV path when helper ok
```

Only **one** static `E8→0x706A00` in the whole exe (`0x70637C`). ASI chains here for `fovadd` ADD at `CCam+0x60`.

Mode **45** adds a **late head-owned CopyMat refresh** at the FOV site (after recompute). Does **not** decouple collision or create distinct eyes.

---

## VS upload / replay thread (same-frame critical path)

| RVA | Bytes @ RVA (anchor) | Role | Status |
|-----|----------------------|------|--------|
| `0x2C180` | `55 8B EC 83 E4 F8 81 EC A0 03 00 00` | **VS upload wrapper** function start (contains VsRet + wrap) | READ-ONLY |
| `0x2C73E` | `85 C0 75 14 8D 44 24 0C …` | **VsRet** — return inside wrapper after `SetVSConstF` work | **READ-ONLY** anchor (~100k+/session) |
| `0x2C6AC` | `89 51 0A …` (`mov [ecx+0x0A], edx`) | Inner **VS wrap** slot/index write (same fn as VsRet) | **FORBIDDEN** (Mode 28 crash) |
| `0x2CF10` | callee from VsRet path | Inner VS const resolve helper (`lea esp+0xC; call`) | UNTESTED |
| — | Static **`E8 → 0x2C73E`** | Direct callers | **0 sites** on CE — indirect only |
| — | Static **`E8` into `0x2C180–0x2C7FE`** | Sub-entry calls (9 sites) | See table below |

**VS wrapper region (`0x2C180`–`0x2C7FE`) — offline call graph:**

| Caller RVA | Target | Enclosing fn | Note |
|------------|--------|--------------|------|
| `0x2BDC3` | `0x2C630` | `~0x2BDC0` | Near wrapper base |
| `0x2C0C0` | `0x2C690` | `~0x2BFC0` | Lands before forbidden `0x2C6AC` |
| `0x37BE0` | `0x2C7A0` | `~0x37620` | Same family as forbidden `0x37BD0` |
| `0x38062` | `0x2C700` | `~0x38006` | |
| `0x3B8DD` | `0x2C700` | `~0x3B8A0` | |
| `0x201B71` | `0x2C7A0` | ? | |
| `0x4FC0D2` | `0x2C6E0` | `~0x4FB8EC` | |
| `0x4FC3D7` | `0x2C6E0` | `~0x4FC360` | |
| `0x8A1D5E` | `0x2C7A0` | `~0x8A1CA0` | |

Prologue sets `this=0x110C0A0` (shader/device context global), stack `0x3A0`, then `call 0xA810` (technique lookup). **VsRet @ `0x2C73E`:** `test eax,eax; jnz` → else `lea eax,[esp+0xC]; push; call 0x2CF10` — D3D `SetVSConstF` return lands here on replay thread.

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
| `0x3199A` / `0x319A4` | Mid-body inside **`0x3187C`** (Mode 41 stack slots) | READ-ONLY |

---

## Indirect replay edge (corrected — READ-ONLY)

Mode 41/42 stack returns at **`0x30D13`** land **mid-SSE** inside view-setup fn **`0x308C9`** (`56 57 8B F9`), **not** at a `call` instruction. Static PE finds the real indirect edge in a **different** function:

| RVA | Bytes / insn | Role | Status |
|-----|----------------|------|--------|
| `0x3010D` | `FF 90 78 01 00 00` | **`call [eax+0x178]`** — vtable replay dispatch | READ-ONLY anchor |
| `0x300D0` | `cmp [0x17ed918],0; push esi; mov esi,ecx` | **Owner fn** of `0x3010D` (NOT `0x2FBF0`) | UNTESTED |
| `0x2FBF0` | `55 8B EC … movaps` | **4×4 matrix multiply** helper — callee **from** `0x3187C` | UNTESTED |
| `0x308C9` | `56 57 8B F9 0F 57 C9 …` | View-matrix field writer (`[edi+0x1C0..0x1F8]`) | UNTESTED |
| `0x309D0` | mid-body SSE | **Not** a prologue — observation label only | **UNTESTED** |
| `0x30D13` | `F3 0F 5E C8` (`divss`) | Mode 41/42 **return address** inside `0x308C9` fn | READ-ONLY |

**Correction:** `0x2FBF0` is **not** the enclosing owner of `0x3010D`. It is a SIMD mat-mul (`dest=[ecx]`, `src0=[ebp+8]`, `src1=[ebp+0xC]`) called from **`0x3187C`** at `0x31BE5`.

### `0x3010D` dispatch (offline disasm @ **`0x300D0`**)

```
0x300D0  thiscall (esi=ecx) — replay device gate
  cmp [0x17ed918], 0          ; replay-active flag
  push esi / mov esi, ecx
  … optional call [0x18de9c8] ; callback if [0x18de9bc]/[0x18de9c4] set
  mov ecx, [0x17ed8d8]        ; singleton device/context object
  mov eax, [ecx]              ; vtable pointer
  lea arg, [esi+0x80]         ; sub esi,-0x80 → replay payload at this+0x80
  push 0 / push arg / push ecx
  call [eax+0x178]            ; 0x3010D — vtable slot +0x178 (entry ~94)
  … second dispatch call [ecx+0x1b4] @ 0x30120 (slot +0x1B4)
  ret
```

| Reg / global | Role |
|--------------|------|
| `[0x17ed8d8]` | Live device/context `this` (273 static code refs) |
| `eax` @ `0x3010D` | **`[ecx]` = vtable pointer** of that object |
| `[eax+0x178]` | Virtual method — likely backend replay draw/upload (live target unknown) |
| `[0x17ed918]` | Gate — skip dispatch when zero |
| `0 static E8` → `0x300D0` | Reached only via vtable / indirect |

**Next live step (Mode 42):** log `call=[eax+0x178]` target each frame when stack hits `0x30D13` family. Mode 42’s old `FF/2@0x30D0D` decode was misaligned stack noise.

---

## View-const apply @ `0x3187C` (offline disasm — UNTESTED)

Best aligned replay/view candidate. Mode 41 mid-returns **`0x3199A` / `0x319A4`** resolve to enclosing **`0x3187C`** (inside SSE body, not separate callers).

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
6. `call 0x2FBF0` — **4×4 matrix multiply** into `[edi+0x1C0]`  
7. `call 0x30C10`, `0x30300`, `0x30FA0` — publish / follow-up (same tail as `0x308C9`)  
8. `ret 4`

### Publish tail callees (offline disasm — corrected RVAs)

| RVA | Prologue | Role | Stereo relevance |
|-----|----------|------|------------------|
| **`0x30C10`** | mid-entry → **`0x30A60`** `56 8B F1` | Writes view-projection block **`[this+0x1C0..0x1F8]`** from stack args; **`ret 0x18`** | **High** — final VP rows on view object |
| **`0x30300`** | `55 8B EC 83 E4 F8 51 56 8B F1` | **PublishSync** — 3× **`0x2FBF0`** matrix sync + optional **`call 0x300D0`** (see below) | **Highest** — bridge to **`call [eax+0x178]`** |
| **`0x30FA0`** | `55 8B EC … sub esp,0x88` | Builds 6×4 float block at **`[this+0x308..]`** (projection/tangent publish); may call **`0x30C10`** when **`[this+0x3E0]==1`** | **Medium** — shader/canvas constants, not draw owner |

**Note:** Capstone labels **`call 0x430300`** etc. — those are **VA display noise** (image base `0x400000`). Correct **RVAs** are **`0x30300`**, **`0x30FA0`**, **`0x30C10`**.

---

### Publish sync @ **`0x30300`** (offline disasm — UNTESTED hook)

**Prologue → ret** (`fn len ~0x5D`, 30 insns):

```
0x30300  thiscall (esi=ecx) — PublishSync
  push ebp / mov ebp,esp / and esp,-8 / push ecx / push esi / mov esi,ecx
  ; --- matrix sync via 0x2FBF0 (stack args left on stack between calls) ---
  lea ecx,[esi+0xC0];  push [esi+0x80]; push [esi+0x180]; call 0x2FBF0
      → [this+0xC0]  = MatMul([this+0x180], [this+0x80])
  lea ecx,[esi+0x100]; push [esi+0x1C0]; call 0x2FBF0   ; 2nd arg still [this+0x80] on stack
      → [this+0x100] = MatMul([this+0x1C0], [this+0x80])
  lea ecx,[esi+0x200]; push [esi+0x240]; call 0x2FBF0   ; 2nd arg [this+0x1C0] on stack
      → [this+0x200] = MatMul([this+0x240], [this+0x1C0])
  cmp [0x17F583C], esi
  jne 0x30358                         ; skip replay unless active view
  mov ecx, esi
  call 0x300D0                        ; replay vtable dispatch (see 0x3010D)
  pop esi / mov esp,ebp / pop ebp
  ret
```

#### **`0x2FBF0` matrix multiply ABI** (28 static `E8` callers)

| Reg / stack | Role |
|-------------|------|
| `ecx` | Dest 4×4 matrix (`movaps` store to `[ecx]`) |
| `[ebp+8]` | Source matrix A (row-major 4×4) |
| `[ebp+0xC]` | Source matrix B |
| Prologue | `55 8B EC 83 E4 F8` — SSE `mulps`/`addps` dot-product rows |

PublishSync chains three multiplies reusing the **same stack arg** between calls (caller does not `add esp` between **`0x2FBF0`** invocations).

#### **`[0x17F583C]` — active view object pointer**

| Item | Value |
|------|-------|
| Role | Points at the **currently active** view/render-camera block (`this` for replay dispatch) |
| Static **writes** | **1** — `mov [0x17F583C], ecx` @ **`0x3006F`** (fn **`0x2FFF0`** view-activation path) |
| Static **reads** | **~116** (view switch, compare, Mode **65** `activeView=`) |
| PublishSync gate | **`0x30349`**: `cmp [0x17F583C], esi` — **`call 0x300D0` only when `this` is the active view** |
| Sibling compare | **`0x2FFD0`**, **`0x30841`** — same global, other branches |

---

### View activation @ **`0x2FFF0`** (offline disasm — UNTESTED)

**Role:** **SetActiveView** — the **only** static writer of **`[0x17F583C]`**. Runs on view **switch** (camera/UI/game paths), **before** view-const apply (**`0x3187C`**) or PublishSync (**`0x30300`**) publish matrix work on the already-active object.

**Prologue → ret** (`fn len ~0xBC`, 46 insns; **no `push ebp`** — standalone entry @ **`0x2FFF0`**):

```
0x2FFF0  cdecl SetActiveView(view, replayFlag) — view activation
  cmp [0x1B4BC50], 0          ; lazy-init guard ×4 (RTTI/name via call 0x35BB0)
  push esi
  mov esi, [0x17F583C]        ; save OLD active view → return value
  … call 0x35BB0 …            ; init [0x1B4BC50/58/54/4C] once each
  mov ecx, [esp+8]            ; arg0 = new view object
  mov [0x17F583C], ecx        ; 0x3006F — ONLY static write to activeView
  mov byte [0x105BCA7], 1     ; view-active flag
  test ecx, ecx
  je 0x300A8
  cmp byte [esp+0xC], 0       ; arg1 = replay trigger byte
  je 0x300A8
  call 0x30130                ; viewport dim sync (this=ecx)
  mov ecx, [0x17F583C]
  call 0x300D0                ; 0x30092 — replay vtable dispatch (same owner as PublishSync)
  mov ecx, [0x17F583C]
  add ecx, 0x280
  call 0x223D0                ; sub-object refresh on view block +0x280
0x300A8  mov eax, esi          ; return previous active view
  pop esi
  ret
```

#### ABI

| Reg / stack | Role |
|-------------|------|
| **`[esp+8]`** | New view object pointer (written to **`[0x17F583C]`**) |
| **`[esp+0xC]`** | **`byte`** replay flag — non-zero + valid object → **`0x30130`** + **`0x300D0`** + **`0x223D0`** |
| **Returns `eax`** | Previous **`[0x17F583C]`** (saved in `esi` at entry) |
| Calling convention | **`cdecl`** — callers **`add esp, 8`** after call |
| Typical caller args | **`push 1; push viewPtr`** or **`push 1; push 0`** (deactivate) |

**Not a CC-pad thiscall** — stack args; **not** a Mode 34 COUNT/dual candidate without ABI reclass.

#### Callees (7 static `E8` from body)

| Call @ | Target | Role |
|--------|--------|------|
| **`0x30007`**, **`0x30024`**, **`0x3003C`**, **`0x3005E`** | **`0x35BB0`** | Lazy type/name lookup (one-time init of 4 globals) |
| **`0x30087`** | **`0x30130`** | Viewport dimension sync helper (`this=ecx`; compares **`[this+0x2B0/0x2B4]`**) |
| **`0x30092`** | **`0x300D0`** | **Replay dispatch** — only when arg1≠0 (paired with PublishSync **`0x30353`**) |
| **`0x300A3`** | **`0x223D0`** | Refresh sub-block at **`[view+0x280]`** |

#### Static **`E8 → 0x2FFF0`** caller graph (**16 sites**)

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
| **`0x22CAAF`** | **`0x22C4D2`** | `1, 0` | Same fn — also calls **`0x30300`** @ **`0x22C950`** |
| **`0x4D83F4`** | **`0x4D8310`** | `1, esi` | Reads **`[0x17F5838]`** sibling global |
| **`0x4DD20D`**, **`0x4DD21A`** | **`0x4DD150`** | `1, 0` / `1, ptr` | |
| **`0x52C404`** | **`0x52C139`** | `1, 0` | |
| **`0x6DD5FF`** | **`0x6DD5C0`** | `1, esi` | After vtable **`[eax+0x60]`** |
| **`0x6DD920`** | **`0x6DD89B`** | `1, 0` | After vtable **`[eax+0x64]`** |
| **`0x9BBB91`** | **`0x9BB570`** | `1, edi` | After **`0x30D40`** setup |

**Verdict:** activation is **game/camera-thread view switching**, not replay-thread matrix upload. **Zero** static **`E8`** from **`0x3187C`** or **`0x30300`** to **`0x2FFF0`**.

#### Sibling thunks (same **`0x2FFxx`** cluster)

| RVA | Role |
|-----|------|
| **`0x2FFD0`** | **`cmp [0x17F583C], ecx; jne ret; push 1,0; call 0x2FFF0`** — idempotent re-activate current view |
| **`0x2FF97`** | Wrapper: stack setup → **`call 0x30510`** → `mov eax, esi; ret` (separate from **`0x2FFF0`** body) |
| **`0x30841`** | Inside **`0x3051B`** viewport-resize fn: **`cmp esi, [0x17F583C]; call 0x223D0 [view+0x280]`** if active — same tail as activation, **no** **`0x300D0`** |

#### Relationship to **`0x3187C`** / **`0x30300`**

```
  [view switch — 16 callers]
       │
       ▼
  0x2FFF0  SetActiveView
       ├─ mov [0x17F583C], view     @ 0x3006F  (only write site)
       └─ if replayFlag: call 0x300D0 @ 0x30092  ──┐
                                                    │
  [per-frame matrix — replay thread family]         │
       │                                            │
       ├─ 0x3187C  view-const apply                 │
       │     └─ tail: call 0x30300 @ 0x31BFF       │
       ├─ 0x308C9  view-matrix writer              │
       │     └─ tail: call 0x30300 @ 0x30A24       │
       └─ 0x30300  PublishSync                     │
             cmp [0x17F583C], this @ 0x30349        │
             call 0x300D0 @ 0x30353  ───────────────┘
                    │
                    ▼
             0x300D0 → call [device+0x178] @ 0x3010D
```

| Question | Answer |
|----------|--------|
| Does **`0x3187C`** call **`0x2FFF0`**? | **No** — **`0x3187C`** only calls **`0x30300`** (and mat helpers) |
| Does **`0x30300`** call **`0x2FFF0`**? | **No** — PublishSync assumes view already active |
| Shared caller with PublishSync? | **`0x22C4D2`** only (distant path; calls both activate + publish) |
| Two **`0x300D0`** entry points | **`0x30092`** (on switch + flag) and **`0x30353`** (on publish + active match) |
| Mode **65** `activeView=` | Live pointer written here; must match view object in Mode **64**/**66** COUNT epochs |

**Stereo implication:** replay draw fires on (a) **view switch** when callers pass **`replayFlag=1`**, and (b) **every PublishSync** on the active view object. Mode **66** COUNT @ **`0x30300`** measures publish cadence; activation cadence is separate (16 call sites, likely ≪1×/frame except camera cuts).

Mode **65** logs `activeView=0x…` (RVA of live object). When PublishSync runs on the active view, expect **`activeView` RVA == view object** passed to **`0x300D0`**.

#### **`call 0x300D0` conditions (replay dispatch)**

Two static **`E8 → 0x300D0`** sites:

| Call @ | Enclosing | When |
|--------|-----------|------|
| **`0x30353`** | **`0x30300`** | After matrix sync; **`[0x17F583C] == this`** |
| **`0x30092`** | **`0x2FFF0`** | After view activation sets **`[0x17F583C]`**; replay gate **`[0x17ED918]==0`** and flag byte checks |

Inside **`0x300D0`** (before **`call [eax+0x178]` @ `0x3010D`**):

1. **`cmp [0x17ED918], 0`** — if **non-zero**, skip all dispatch (`ret` @ `0x30126`)  
2. Optional callback **`call [0x18DE9C8]`** if **`[0x18DE9BC]`** or **`[0x18DE9C4]`** set  
3. Load device **`[0x17ED8D8]`**, **`call [vtable+0x178]`**, then paired **`call [vtable+0x1B4]`**

**Stereo implication:** replay draw/upload fires only for the **active** view object, and only when replay gate is clear.

#### Static **`E8 → 0x30300`** caller graph (**12 sites — NOT only `0x3187C`**)

| Call @ | Enclosing fn | Note |
|--------|--------------|------|
| **`0x31BFF`** | **`0x3187C`** | View-const apply tail (Mode **64** family) |
| **`0x30A24`** | **`0x308C9`** | View-matrix writer tail (Mode **41** ret **`0x30D13`** family) |
| **`0x30BC5`**, **`0x30BFB`** | **`0x30A60`** | VP block writer |
| **`0x30CC9`** | **`0x30C86`** | Publish family |
| **`0x30D2A`**, **`0x30D9F`** | **`0x30CE7`** | Publish family |
| **`0x30F0C`**, **`0x30F8B`** | **`0x30DB6`** | Publish family |
| **`0x21862D`** | **`0x218491`** | Distant game-render path |
| **`0x219084`** | ? | Distant |
| **`0x22C950`** | **`0x22C4D2`** | Distant |

**Verdict:** PublishSync is a **shared** post-matrix hook used by both **`0x3187C`** and **`0x308C9`** tails plus VP/publish helpers — not exclusive to the view-const candidate. COUNT @ **`0x3187C`** (Mode **64**) measures view-const cadence only; replay dispatch cadence may differ on other callers.

**Mode 66 (implemented — opt-in stereo=66):** COUNT-only @ **`0x30300`** PublishSync prologue to measure avg entries/EndScene (12 static **`E8`** callers). Does **not** hook **`0x300D0`** or forbidden sites. Auto-reverts to **`45`** after 45 EndScenes. Ladder step after **65**.

### Static call graph

| Direction | RVA | Note |
|-----------|-----|------|
| **E8 → `0x3187C`** | **0 sites** | Vtable / indirect only |
| **`0x3187C` → callees** | `0x66DAE0`, `0x417610`, `0x2FBF0`, `0x30C10`, `0x30300`, `0x30FA0` | |
| Shared tail with | `0x308C9` | Both call **`0x30300`** + **`0x30FA0`** after matrix write |

**Dual-call per frame?** Unknown statically (no `E8` callers). Mode **64** COUNT @ `0x3187C` measures avg entries/EndScene; want **[0.8, 4]** (~1×).

### COUNT-only hook safety (Mode 64 / Mode 66)

| Check | Mode **64** @ `0x3187C` | Mode **66** @ `0x30300` |
|-------|-------------------------|-------------------------|
| Aligned prologue | **YES** `56 57 8B F9` | **YES** `55 8B EC 83 E4 F8 51 56 8B F1` |
| CC-pad thiscall | **YES** (`thiscall-5657`) | **YES** (`thiscall-frame`; fixed anchor + byte verify) |
| Forbidden list | **Not listed** | **Not listed** |
| Dual / replay hook | **NO** — COUNT passthrough only | **NO** — does not call **`0x300D0`** |
| Crash history | **None yet** | **None yet** — new this session |

**Verdict:** **Plausible for COUNT-only** (Mode **64**); **not hook-safe for dual** until COUNT cadence + Mode 41 replay-thread + `vsPatch>0` trial.

### Sibling `56 57 8B F9` thiscall writers (`0x30000–0x34000` scan)

Only **three** aligned `push esi; push edi; mov edi,ecx` starts in this band:

| RVA | `[edi+0x100]` view loads | `[edi+0x1C0]` matrix writes | Role | COUNT? |
|-----|--------------------------|-----------------------------|------|--------|
| **`0x3187C`** | **16** `movss` loads | via `call 0x2FBF0` | View-const apply → mat @ `+0x1C0` | **Mode 64** target |
| **`0x308C9`** | 0 | **1+** stores `+0x1C0..0x1F8` | View-matrix / projection writer | secondary |
| `0x33FE6` | 0 | 0 | `[edi+6]` test — unrelated | reject |

No additional hidden view writers near **`0x3187C`**. Shared publish tail: both **`0x3187C`** and **`0x308C9`** call **`0x30300`** + **`0x30FA0`** (12 / 11 static `E8` each).

### Device singleton `[0x17ed8d8]` (offline PE)

| Item | Value |
|------|-------|
| Static code **reads** | **247** (`mov reg,[0x17ed8d8]` / `A1`) |
| Static code **writes** | **2** — init **`0x23A48`** `mov [device], 0x18DC2B8`; teardown **`0x24DA8`** `mov [device], 0` |
| Replay gate | **`[0x17ed918]`** — **`0x300D0`** skips dispatch when non-zero |
| Vtable @ **`0x3010D`** | **`call [eax+0x178]`** (slot **94**); paired **`call [ecx+0x1B4]`** @ **`0x30120`** |
| Max **`FF /2`** disp32 slot (exe-wide) | **`0x248`** (~entry **146**) |
| Live read plan | Mode **65** — EndScene read-only log of device/vtable/**`+0x178`**/**`+0x1B4`** (no hook) |

### Safe live experiment ladder (no dual — one mode per session)

Deploy latest `out-asi\gtaiv_dxvk_vr.asi` + `openvr_api.dll`. Edit **`gtaiv_dxvk_vr.stereo`** next to EXE (single integer). **Kill anytime:** **`45`** or delete file.

| Step | stereo | Duration | Exact log lines to capture | Pass criterion |
|------|--------|----------|----------------------------|----------------|
| **0 Baseline** | **`45`** | 2+ min calm | `StereoMode: 45` · `StereoSubmit: L=1 R=1 mode=45` · `Mode44: RT lock … recreates=0` | Smooth; no freeze |
| **1 COUNT** | **`64`** | ≥45 EndScenes (~1 min) | `Mode64: COUNT armed exeRva=0x3187C …` · `Mode64: COUNT es#N/45 entries=… sum=…` · **`Mode64: COUNT done exeRva=0x3187C avgEntries=… cadence=PASS\|REJECT`** | **`cadence=PASS`** (`avgEntries` in **[0.8, 4]**) |
| **2 Vtable peek** | **`65`** | ≥45 EndScenes | **`Mode65: VT178-LOG es#N/45 device=0x… rva=0x… vtable=0x… slot178=0x… slot1B4=0x… replayGate=… activeView=0x… hook=NO`** · **`Mode65: VT178-LOG done (45 EndScenes) …`** | Stable non-zero **`slot178`** RVA; note **`activeView`** |
| **3 PublishSync COUNT** | **`66`** | ≥45 EndScenes (~1 min) | **`Mode66: COUNT armed exeRva=0x30300 …`** · **`Mode66: COUNT es#N/45 entries=… sum=…`** · **`Mode66: COUNT done exeRva=0x30300 avgEntries=… cadence=PASS\|REJECT`** | **`cadence=PASS`** (`avgEntries` in **[0.8, 4]**); compare with Mode **64** |
| **4 Chain** | **`41`** | 1 calm min | **`Mode41: replay-chain epoch=… VsRetHits=…`** · **`Mode41: CHAIN slot=… ret=0x3199A\|0x319A4 fn=0x3187C abi=… hook=NO`** | Top slots resolve **`fn=0x3187C`** |
| **5 Owner edge** | **`42`** | 1 calm min | **`Mode42: CHAIN … ret=0x30D13 …`** · **`Mode42: OWNER-EDGE ret=0x30D13 enclosing=… call=FF/2@0x3010D … modrm=0x90 … hook=NO replay=NO`** | **`call=FF/2@0x3010D`** (vtable **`+0x178`**) confirmed |

After steps **1–5**: compare **`Mode65 slot178=0x…`** with **`Mode42 call=…@0x3010D`**, and **`Mode66 avgEntries`** with **`Mode64`**. File auto-reverts to **`45`** when **64**, **65**, or **66** finishes. **Do not enable dual.** **SameFrameSeamGate stays CLOSED.**

---

## View-const apply candidates (NOT forbidden — UNTESTED)

Rage has no Source `RenderView`. These are **static** candidates for per-draw / replay view-constant work distinct from CopyMat:

| RVA | Prologue / anchor | AOB / note | Why candidate | Status |
|-----|-------------------|------------|---------------|--------|
| `0x3187C` | `56 57 8B F9 8D 44 24 70` | Loads `movss` from `[edi+0x100..0x10C]` | Clean thiscall; Mode 41 slots `0x3199A/0x319A4` return here | **UNTESTED** — COUNT first |
| `0x3187C` | same | Mid returns `0x3199A`, `0x319A4` | Enclosing start **aligned**; mid-body SSE | Mode **64** COUNT |
| `0x3010D` | `FF 90 78 01 00 00` | `call [eax+0x178]` | Only `FF 90` in `0x30000–0x32000` — replay vtable | READ-ONLY |
| `0x370D0` | `56 57 8B 7C 24 08` | Helper prologue | Mode 34 helper — no dual observed | UNTESTED |
| `0x5303D0` | `55 8B EC …` stack `0xB4` | Mode 34 rare ~1×/frame | Large stack — ABI unknown | UNTESTED |
| — | string `viewProj` @ file `0xAA60A4` | Debug/RTTI hint | Near shader constant naming | data only |

**Rejected as hook targets:** `0x4D8F10` (mid SSE, avg 0.31/frame), `0x52E7C0` (stackarg), `0x4DDAD0` (dual dead), `0x37BD0` / `0x2C6AC` (crash).

---

## Indirect edge @ `0x309D0` (Mode 42 — READ-ONLY, superseded)

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
| `0x8F7461` | caller cluster | Multiple `E8→BuildRootA` from `0x8F7620` etc. | **10** static BuildRootA call sites, all inside `0x8F7xxx` |
| `0x5C2380` | FrameA path (len ~`0x866`) | Game-thread frame driver | READ-ONLY (Mode 19) — **no** direct `E8→BuildRootA` |
| `0x5C2BF0` | mid-instr in CE scan | Documented “BuildWrapB” — **verify** before hook | DRIFT risk — not a clean prologue in static PE |
| `0x118D7F0` | Global **render-phase manager** | `this` for build/exec | data |
| `0xD8D8C0` / `0xD8D9C0` | Manager **`+0xD0` / `+0x1D0`** pos rows | Live cam WORLD copies (Mode 21) | Shifting at exec did not change draws |

**BuildRootA offline disasm (`0x8F7F00`):** `this=esi=ecx` (render manager), `[ebp+8]` phase descriptor; first checks `[edi+0x387]` with `cmp/jg`; phase loop uses **`call [ecx+disp]`** (vtable) at `~0x8F7F54` → **`call 0x755070`**. **ExecRoot `0x8F73B0` has 0 static E8 callers** — reached only via vtable/indirect.

**Correction:** No `mov ecx,0x118D7F0; call BuildRootA` pair exists in static PE. Manager `this` is passed through the `0x8F7xxx` dispatch chain (135 `mov ecx,0x118D7F0` sites elsewhere call **`0x4F7000`** family helpers, not BuildRootA directly).

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

ASI stereo modes **`62`** (pattern validate + Mode 45 render) and **`63`** (seam gate log + Mode 45 render) scaffold this path; **`64`** = Mode 45 + COUNT-only @ **`0x3187C`** (opt-in, reverts to **`45`** when done); **`65`** = Mode 45 + read-only device/vtable **`+0x178`** + **`activeView`** log (opt-in, no game hook); **`66`** = Mode 45 + COUNT-only @ **`0x30300`** PublishSync (opt-in, reverts to **`45`** when done). Default remains **`45`**. Safe ladder: **`45→64→65→66→41→42`**.

---

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
0x2C6AC  0x37BD0  0x1BF010  0x4DDAD0  0x309D0(hook)  Mode34 dual @0x4DDAD0  0x309D0 blind call
```

Safe production stack: stereo **`45`**, FOV site **`0x70637C`**, CopyMat **`0x83DB90`**, VsRet observation **`0x2C73E`** only.

---

## World visual size (does not fix same-frame jump)

Three **independent** levers — do not combine aggressively in one test:

| Lever | File / key | Effect | vs perfect VR |
|-------|------------|--------|-----------------|
| **F7 leanGain** | `gtaiv_dxvk_vr.scale` (×100) | 6DoF head-lean only; `eyeDelta = hmdDelta / leanGain`; higher = smaller world when leaning | Does not change building scale or stereo epoch |
| **F6 stereoscale** | `gtaiv_dxvk_vr.stereoscale` | Soft IPD/disparity multiplier (100–130%) | Can affect fusion if too high |
| **fovadd** | `gtaiv_dxvk_vr.fovadd` | Engine FOV ADD at **`0x70637C`** site → true canvas fill | Kills black bars; wide FOV can add temporal stress |

**Rule:** F7 ≠ Halo/Luke “world scale”. Real building size needs engine FOV (`fovadd`) + eventual same-frame stereo. Mode **45** keeps all three decoupled.

**F7 preset ladder (Mode 45, 11 steps to 30.0):**  
`1.0 → 1.25 → 1.5 → 2.0 → 2.5 → 3.0 → 4.0 → 5.0 → 8.0 → 12.0 → 30.0`  
File stores **leanGain×100** (100 = normal; max 3000 = 30.0).

