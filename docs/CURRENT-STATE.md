# CURRENT-STATE — gtaiv-dxvk-vr

## Deployed 2026-07-26 — Mode **88** HITCHCUT + EyeProj DEFAULT (90-min nap session)

### Headset feedback Mode87 (`20260726-174440-mode87-eyeproj-eyert`)
| Issue | Notes |
|------|--------|
| Scale OK again | ~Mode80 letterbox look |
| **Heavy hitching** | Dual every frame + 2048² StretchRect + EyeProj log I/O |
| **Bars still present** | Honest 16:9→G2 letterbox ≈100%h / 62%v (geometry; not a bug) |
| **Game froze** | Graphics options → D3D Reset; eye RTs not released → hang |

### Mode88 changes (best balance DEFAULT)
| Path | Change |
|------|--------|
| **EyeProj** | **ON** (never Mode86). Quiet after FOV proven; still ON in dual |
| **Dual** | DrawScene×2 **every N** (default **2**) + **HOLD** last L/R on off ticks |
| **HOLD bugfix** | Mode77 dual now sets `didDualThisFrame` — EndScene no longer TemporalCapture-overwrites same-frame L/R |
| **Eye RT** | Fixed square eyert — Mode88 **default 1440** (file `gtaiv_dxvk_vr.eyert`) |
| **dualn** | Optional `gtaiv_dxvk_vr.dualn` = 2..4 (3 = lighter streets) |
| **Reset** | Hook `IDirect3DDevice9::Reset` → release POOL_DEFAULT eye RTs before Reset |
| **Log I/O** | Buffered log (~100ms flush); quieter EyeProj discover/inject |
| **Scale** | Mode80 LetterboxPrefer — honest ~100%h / 62%v |
| **Never** | live `view+0x80`; Mode83 fill-zoom; Mode86 default; kill≠45 |

### Mode89 A/B (bars / presence) — not default
| Path | Change |
|------|--------|
| Dual | Every frame (Mode77 quality) |
| Bars | Mild soft letterbox **~68%v / fillH≤108%** (not Mode84 78% groß; never Mode83) |
| Eye RT | eyert **2048** for A/B |
| FOV | Quiet FOV-proof so canvas ungates (fixed this session) |
| Tradeoff | Heavier than 88; try if bars annoy more than hitch |
| Log proof | `20260726-180837-mode89-fovproof-bars`: fill≈**108%h / 67%v**; StereoDiff≈7–8k; AppFPS≈55 |

### Mode map (edit `gtaiv_dxvk_vr.stereo`)

| stereo | Name | What |
|--------|------|------|
| **88** | **HITCHCUT + EyeProj DEFAULT** | EyeProj ON; dual 1/N HOLD; eyert 1440; Reset-safe |
| **89** | Cull-sync soft bars A/B | Dual every; ~68%v soft; eyert 2048 |
| **87** | EyeProj + eyert quality | Dual every; eyert 2048; hitchier |
| **80** | Hard letterbox | Scale reference |
| **86** | No-EyeProj | A/B only — jumping; NEVER default |
| 45 | Kill | Safe fallback |

**Skip 74**. **Never** live `view+0x80`. Kill=**45**. Remap 71→45, 73→72.

**Build:** `ASI_BUILD_ID 20260726-183112-mode88-default-final`  
**Play:** stereo **`88`** + eyert **`1440`** + dualn **`2`** already in game dir.

**Agent log proof (this session):**
- `StereoMode: 88` · Mode88 HITCHCUT + EyeProj · okDraw=1
- `Hooked Reset` · `DeviceReset: BEFORE/OK` on boot (graphics-options freeze path armed)
- `EyeRt: 1440×1440` · DualN every 2 · StretchRect POINT
- `FOVPROOF` · fill≈**100%h / 62%v** letterbox
- Dual + `hold=1` · StereoDiff≫0
- AppFPS es/s ≈**90** (apartment) with HitchHist `0-16≈540 / 51+≈0` per 5s window
- Mode89 A/B: fill≈**108%h / 67%v** soft68; StereoDiff≈7–8k
- dualn=`3` A/B: lighter dual, more HOLD

### 90-min session chronology (wall-clock)
| Time | Approach | Learned |
|------|----------|---------|
| 17:57 | Start; read Mode87 state | Hitch + bars + graphics freeze; EyeProj must stay ON |
| 18:00 | Mode88 design + HOLD bug found | Mode77 dual never set didDual → TemporalCapture overwrote L/R |
| 18:02 | Build Mode88 hitch-eyeproj | Reset hook fires on boot; 1440 eyert; dual 1/2 HOLD; StereoDiff~5k |
| 18:04 | Mode89 cullsync-bars | Dual every + soft68; FOV not proven first pass |
| 18:07 | Mode88 default-balance | FOVPROOF + fill 62%v; AppFPS~55–60 |
| 18:08 | Mode89 fovproof-bars | SteamVR IPC died once; after restart fill≈108%h/67%v |
| 18:12 | Mode88 final-default | Restored as best balance |
| 18:14 | dualn=3 A/B | AppFPS up to ~65; more HOLD |
| 18:15 | StretchRect POINT | AppFPS **~90** apartment; copy~0.02ms |
| 18:17 | HitchHist | 0-16ms dominant; 51+ rare after load |
| 18:20–18:30 | 10-min sustained | AppFPS ~37–40 under load; HitchHist still mostly ≤16ms |
| 18:31 | Mode88-default-final | SteamVR IPC blip once; after restart FOVPROOF + 90 es/s |
| — | Commits `66ca013`, `05dff3d` | No push |

**You test (short English):**
1. Headset on — Mode **88** (already set).
2. **Streets:** fewer hitches than Mode87?
3. **EyeProj feel?** Still present (not flat Mode86)?
4. **Bars:** top/bottom OK if scale realistic (~Mode80)?
5. **Graphics options:** change resolution / AA — should **not freeze** (watch log for `DeviceReset:`).
6. Want fewer bars (may hitch more) → stereo **`89`**, eyert `2048`, restart.
7. Want sharper → eyert `2048` keep stereo `88`, restart.
8. More hitch cut → dualn file `3`, restart.
9. Bad → stereo **`80`**. Kill → **`45`**. Never leave on **86**.

**Test order:** **88** → 80 (scale) → optional **89** (bars) → kill **45**. Skip 74.

**Tradeoffs (why 88 default):**
- Keeps EyeProj (Mode86 jumping fail)
- Cuts dual+StretchRect cost vs Mode87 every-frame/2048
- Honest Mode80 bars (62%v is geometric for 16:9→G2 letterbox; Mode83 zoom forbidden)
- Reset-safe for graphics options

---

## Prior deployed 2026-07-26 — Mode **87** EyeProj + fixed eyert DEFAULT

### Headset feedback Mode86 (`20260726-173233-mode86-noeyeproj`) — HARD FAIL
| Issue | Notes |
|------|--------|
| **Jumping like crazy** | Worse than with EyeProj — dual 1/2 + no mid-draw EyeProj broke look |
| **Looks worse** | EyeProj OFF feels wrong |
| **Bars still full** | Letterbox bars remained |

**Rule:** **NEVER ship without EyeProj.** Prefer EyeProj ON + documented sidewalk-hole risk over EyeProj OFF.

### Mode87 changes
| Path | Change |
|------|--------|
| **EyeProj** | **ON** mid-draw (Mode86 `WantsNoMidDrawEyeProj` not used) |
| **Eye RT** | Fixed square from **`gtaiv_dxvk_vr.eyert`** — default **2048**; tiers **1440 / 2048 / 2560 / 2880**. **NOT** SteamVR `GetRecommendedRenderTargetSize` |
| **Dual** | Mode77 DrawScene×2 **every frame** (not Mode86 1/2) |
| **Scale** | Mode80 LetterboxPrefer + aggressive CCam/PubProj toward same HMD FOV as EyeProj (cull sync) |
| **Jump** | Pose freeze + EMA reject (pos>8cm / fwdDot<0.90) + seat 8cm clamp; HMD worldlook stamps |
| **Never** | live `view+0x80`; kill≠45 |

SteamVR recommended RT path kept only for Mode **85/86 A/B** — re-integrate later when stereo/FOV solid.

### Eye RT how-to (concrete)
1. Open folder: `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\`
2. Create or edit file **`gtaiv_dxvk_vr.eyert`** (Notepad).
3. Put **one number** on the first line, then save:
   - `1440` = lighter / fewer StretchRect hitches
   - `2048` = **default** (already set)
   - `2560` = sharper, heavier
   - `2880` = max tier (VRAM / hitch risk)
4. Restart GTA. Log must show `EyeRt: …` and `eyeRT=2048×2048` (or your tier) with `src=config-eyert`.

### Mode map (edit `gtaiv_dxvk_vr.stereo`)

| stereo | Name | What |
|--------|------|------|
| **87** | **EyeProj + eyert DEFAULT** | EyeProj ON; fixed eyert; dual every frame; Mode80 letterbox |
| **80** | Hard letterbox | Scale reference A/B |
| **86** | No-EyeProj SteamVR-RT | A/B only — jumping; do not use as default |
| **85** | SteamVR-RT letterbox | A/B only — hole + hitch |
| 45 | Kill | Safe fallback |

**Skip 74**. **Never** live `view+0x80`. Kill=**45**.

**Build:** `ASI_BUILD_ID 20260726-174440-mode87-eyeproj-eyert`  
**Play:** stereo **`87`** + eyert **`2048`** already in game dir. GTA running.

**Agent log proof (this session):**
- `StereoMode: 87` · Mode87 CONFIG-EYERT + EyeProj DEFAULT · okDraw=1
- `EyeRt: 2048×2048` fixed square · `LOCKED eyeRT=2048×2048 src=config-eyert` · **0** `src=steamvr-recommended` eyeRT locks
- `eyeProj=` counter climbing (16000+) via device VS patch; `FOVPROOF: DRAWN via device VS patch`
- `DRAWSCENE-ONLY dual #` climbing (8000+) every frame
- `HmdLook: DRAW stamp #` climbing (worldlook ON)
- `PoseFreeze:` EMA reject + seat clamp ON
- Canvas fill≈**100%h / 62%v** letterbox (Mode80-ish)
- AppFPS stereo=1 ~50–70 submit/s under dual

**You test (short English):**
1. Headset on — Mode **87** (already set).
2. **Less jump than 86?** Turn head / walk.
3. **EyeProj feel back?** World FOV / presence (not flat cinema).
4. **Hole?** Sidewalk black rectangle / floating beams? (possible — report).
5. **Bars?** Top/bottom OK if scale realistic (Mode80-ish)?
6. Bad → stereo file **`80`**. Kill → **`45`**. Never leave on **86**.
7. Sharper later → edit `gtaiv_dxvk_vr.eyert` to `2560`, restart.

**Test order:** **87** → 80 (compare) → kill **45**. Skip 74. Never default 86.

---

## Prior deployed 2026-07-26 — Mode **86** SteamVR-RT SAFE DEFAULT (hole fix)

### Headset feedback Mode85 (`20260726-171853-mode85-steamvrrt`)
| Issue | Notes |
|------|--------|
| **Black sidewalk hole** | Sharp rectangular void + floating curb beams (WMR mirror screenshot) |
| **Still groß** | Characters oversized vs Mode80 |
| **Strong bars** | Expected under letterbox; OK if scale realistic |
| **Street hitches** | 2048² StretchRect + DrawScene×2 every frame |

### Glitch root cause (best evidence)
Mid-draw **EyeProj / device VS proj rewrite** during Mode77 dual: HMD asymmetric FOV stamped into VS constants while engine frustum cull / streaming still uses game FOV → missing tiles = black rectangle with stray beams. Mode74 EndScene without dual did not show this hole class. Mode86 keeps dual + PubProj HOLD + VIEW inject; **never** mid-draw EyeProj.

### Mode86 changes
| Path | Change |
|------|--------|
| **EyeProj** | **OFF** even in dual (`WantsNoMidDrawEyeProj`) — hole fix |
| **Eye RT** | `min(recommended, BB×1.25, **1440**)` — was Mode85 2048² hitch |
| **Dual** | DrawScene×2 **every other** call (1/2) |
| **Scale** | Mode80 LetterboxPrefer; CCam cap **85** (Mode85 was 88) |
| **Never** | live `view+0x80`; kill≠45 |

### Mode map (edit `gtaiv_dxvk_vr.stereo`)

| stereo | Name | What |
|--------|------|------|
| **86** | **SteamVR-RT SAFE DEFAULT** | No mid-draw EyeProj; RT≤1440; dual 1/2; Mode80 letterbox |
| **85** | SteamVR-RT letterbox | A/B — had hole + hitch |
| **80** | Hard letterbox | Scale reference |
| **77** | DrawScene dual only | Stereo baseline |
| 45 | Kill | Safe fallback |

**Skip 74** for this test pass.

**Never** live `view+0x80`. Kill=**45**.

**Build:** `ASI_BUILD_ID 20260726-173233-mode86-noeyeproj`  
**Play:** stereo **`86`** already in game dir. GTA launched OK.

**Agent log proof (this session):**
- `StereoMode: 86` · Mode86 STEAMVR-RT SAFE DEFAULT · okDraw=1
- eye RT **1440×1440** (recommended 2836×2772 capped; `noMidDrawEyeProj dual1/2`)
- **0** `EyeProj: INJECT` / `see` / `device VS patch` lines
- `DRAWSCENE-ONLY dual #` climbing (8000+)
- AppFPS es/s ~88–90 · stereo=1
- Canvas FOV may stay GATED (prove without EyeProj) → more Mode80-honest bars, less groß risk

**You test (short English):**
1. Headset on — Mode **86** (already set).
2. **Hole gone?** Walk sidewalks — no black rectangle / floating beams?
3. **Scale:** closer to Mode **80** (not groß like 84/85)?
4. **Bars:** top/bottom OK if scale realistic?
5. **Streets:** fewer hitches than 85?
6. Bad → stereo file **`80`**. Kill → **`45`**.
7. **Skip 74**.

**Test order:** **86** → 80 (compare scale) → kill **45**. Skip 74.

**Superseded by Mode 87** (EyeProj back ON; fixed eyert; Mode86 was HARD FAIL — jumping).

---

## Prior deployed 2026-07-26 — Mode **85** SteamVR-RT + Mode80 letterbox

Mode85: recommended eye RT capped 2048² + Mode80 letterbox + quiet EyeProj. Headset: black hole + still groß + hitches. Superseded by Mode **86**, then **87**.
Build was: `ASI_BUILD_ID 20260726-171853-mode85-steamvrrt`.

### Why SteamVR recommended RT helps
Game BB is **16:9**; HMD eyes are nearer square. Mode85/86 size Submit eye canvases from OpenVR `GetRecommendedRenderTargetSize`, then **letterboxes** the game BB into that canvas. Game still draws at BB — we only change the copy target.

### Why Mode84 was still groß
Soft-letterbox aimed ~78%v with fillH cap ~118% → image larger than Mode80’s honest ~100%h/62%v letterbox. Fewer bars ≠ realistic scale.

---

## Prior deployed 2026-07-26 — Mode **84** soft-letterbox (groß again)

Mode84 fill≈118%h/73%v had no bars but scale groß. Superseded by Mode **85**, then **86**.
Build was: `ASI_BUILD_ID 20260726-170915-mode84-softlb`.

---

## Prior deployed 2026-07-26 — Mode **83** fill-sweet (monitor again)

Mode83 fill≈149%h/92%v felt fat/monitor. Superseded by Mode **84**, then **85**.
Build was: `ASI_BUILD_ID 20260726-164513-mode83-fillsweet`.

### Headset feedback that drove Mode83 (Mode80 aspectsafe `20260726-163120`)
| Mode | Result |
|------|--------|
| **80** | Scale/aspect **OK**, but **too many bars** (~62%v) + **perf bad** |
| **82** | Still **giant** — fill mode claimed wider canvas than drawn |
| **81** | Looks OK, still bars + hitches |
| **74** | **Skip** — do not require testing |

### Why Mode80 had ~62%v bars
On Reverb G2, cover FOV is near-square; game BB is **16:9**. Mode80 **LetterboxPrefer** keeps `tanH = tanV × 1.778`, then uniform scale-down so neither axis exceeds cover → **max honest letterbox = ~100%h / ~62%v**. Raising CCam FOV alone cannot remove those bars under letterbox (policy undoes vertical fill). More vertical fill **requires** FillPrefer (allow slight side crop) while keeping aspect.

### Why Mode82 was giant
FillPrefer published optimistic CCam tangents **wider than measured drawn FOV** → StereoCanvas zoomed/cropped → giant monitor. Fixed: canvas publish from **measured** `sy/sx` + clamp FillPrefer to drawn.

---

## Prior deployed 2026-07-26 — Mode **80** aspect-safe FOV (letterbox 62%v)

Mode80 letterbox kept correct aspect but forced ~62%v bars on G2. Superseded by Mode **83**, then **84**, then **85**.
Build was: `ASI_BUILD_ID 20260726-163120-mode80-aspectsafe`.

---

## Prior deployed 2026-07-26 — Mode **80** FOV presence (SQUEEZE BUG)

### Headset that drove that build (user, Mode74 morning)
| Mode | Result |
|------|--------|
| **74** | Completely smooth |
| **76** | Smooth — still **giant monitor** |
| **77** | **Best feel** / smooth — **SAME** fat/huge monitor |
| **79** | **No difference noticed** |

Smoothness OK on 77. Boss: giant monitor / not inside the world. Mode80 fovprove fixed giant-monitor lie but then **squeezed** after restart — superseded by **aspectsafe** above.

### Why Mode79 did nothing (evidence)
1. Mode79 only raised CCam cap **85→90**; `hmdTarget` was already **~71.5** → **identical FOV behavior to Mode74**.
2. FOVPROOF: `active+0x180[0]=1.000` always while PubProj stamped `sx≈0.93` on a *different* held object — **slot/stamp mismatch**; `+0x308` never showed HMD tangents on active view.
3. Canvas published lied `gameTan=(1.881,1.058)` from CCam → **fill≈162%h** → StereoCanvas **cropped/zoomed** the BB into the HMD = **giant monitor**, even when drawn FOV stayed cinema.
4. `eyeProj=0` on Mode79 temporal — draw path uploaded **view+0x80**, not +0x180 proj.

**Build was:** `ASI_BUILD_ID 20260726-162039-mode80-fovprove` — do not keep as default.

---

## Prior deployed 2026-07-25 overnight marathon — Mode **74** morning-safe

### User feedback that drove that night
Mode75 denser fearvr (`20260725-180123-mode75-fearvr`): **felt even LESS like actual 3D** → **SCRAPPED**.

### Mode map (edit `gtaiv_dxvk_vr.stereo`)

| stereo | Name | What |
|--------|------|------|
| **74** | **Hitchcut DEFAULT** | Pair-hold temporal + DRAW HMD look/IPD + PubProj HOLD. Dual OFF. Morning boot. |
| **75** | Safer sparse dual | BuildRootA×2 **1/8** after long gate (NOT denser fearvr). Opt-in. Apt→city risk. |
| **76** | AER quality | One engine eye/frame + `TextureWithPose` + eye matrices. Jump risk. |
| **77** | DrawScene-only dual | **Approach A** — dual world DrawScene×2 (not full BuildRootA). Log-proven StereoDiff. |
| **78** | Shader-const stereo | **Approach C** — VIEW budget 8/ES + EyeProj; no BuildRootA×2. |
| **79** | FOV presence hammer | **Approach E** — CCam cover + FOVPROOF logs. |
| 45 | Kill | Safe fallback. |

**Never** live `view+0x80` (Mode71 freeze). Kill=**45**.

### Overnight results (log-validated, no headset)

| Approach | Result |
|----------|--------|
| **G scrap denser 75** | Done — safer 1/8 only |
| **A cheaper seam** | **Mode77 works** — `DRAWSCENE-ONLY dual #` thousands; StereoDiff ~11k–15k; DrawScene is vtable (0 static E8) |
| **B viewport/SBS** | Exhausted offline — no SBS without second world draw |
| **C shader-const** | Mode78 armed (budget 8/ES + EyeProj) |
| **D AER** | Mode76 armed — `Mode76AER: frame #` L↔R + cover tans |
| **E FOV** | CCam `hmdTarget≈71.5` applied; PubProj injects climb; FOVPROOF still sees +0x180[0]=1.0 on resolved view (slot vs stamp mismatch — headset) |
| **F RE** | `scripts/offline-world-draw-seam.py` → `docs/_re_scratch/world_draw_seam_report.txt`; BuildRootA **10** E8 callers |

**Build:** morning deploy below · prior smokes `…-marathon-*`  
**Play tomorrow:** stereo **`74`** already deployed. Optional tests: **77** (true-er same-frame cheap), **76** AER, **79** FOV, **75** only if you want sparse BuildRootA again.

**You test (short English):**
1. Headset on — boot should be Mode **74**.
2. Streets: jump / hitch vs last night?
3. Want depth experiment: put **`77`** in stereo file, Quick Restart — ask if DrawScene dual feels like real 3D (not cinema).
4. Optional: **`76`** AER, **`79`** FOV, **`75`** safer dual.
5. Bad → **`45`**, Quick Restart.

---

## Prior deployed 2026-07-25 — Mode **75** fear-vr denser — SCRAPPED (less 3D)


### Goal
Proper **3D stereo** (presence first). Studied [DR-89/fear-vr](https://github.com/DR-89/fear-vr) (MIT) → `docs/FEAR_VR_RE.md`.

### What fear-vr does (short)
Same-frame world render **twice** (hook below sim): save cam → L eye pose+FOV+`RenderCamera`+capture → R same → restore → Present stages pair. Not AER-as-main.

### What we ported into Mode 75 (then scrapped denser)
- Same-frame **BuildRootA×2** eye loop + PubProj HMD look/IPD/eye proj + canvas capture
- Denser dual **1/2** after ~90 street-stable ES — **headset: LESS 3D → SCRAPPED**
- Mode75 now = safer **1/8** only (see mode map above)

**Build was:** `ASI_BUILD_ID 20260725-180123-mode75-fearvr` — do not re-use denser path.

---

## Prior deployed 2026-07-25 — Mode74 **jump/track fix** (post-remap HARD FAIL)

### User headset (HARD FAIL on `20260725-174441-mode74-hitchcut-remap`)

| stereo | Result |
|--------|--------|
| **74** hitchcut | Jumping like crazy; bad street performance |
| **75** dual | Terrible perf; **giant cinema ~70° tilted**; **NO head tracking** |
| **76** AER | Ton of L/R jumping; fused in between |

### Root causes

1. **Mode 75 no tracking / ~70° tilt:** Dual path called `Mode74PushLiveCamEyeView` which pushed a **D3D-layout** `BuildLiveViewMatrix16` over the Rage HMD+IPD matrix. UploadFn/PubProj also applied **IPD only** (no `Mode74ApplyHmdEyeLocal`) and `OwnsEye` skipped the SetVSConstF worldlook stamp → body/follow cinema, no HMD look.
2. **Mode 74 jump:** Same UploadFn OwnsEye without worldlook (when it fired) + CamMatrix **and** inject both adding IPD (double sep) on temporal path.
3. **Mode 76 L/R jump:** Engine eye flip (`TemporalCaptureAerMode74`) — inherent 1-frame L/R. Disabled; 76 = pair-hold + TextureWithPose only.

### Fixes in this build

1. UploadFn + PubProj dual + WritePubProjSlots: **HMD look then ±IPD** (same as VIEW inject).
2. Removed dual `PushLiveCamEyeView` D3D overwrite.
3. Mode74 family outside dual: CamMatrix **skips IPD** (DRAW owns it) — no double-apply.
4. Mode **76**: pair-hold (no L↔R engine flip); still TextureWithPose.
5. Keep: pose EMA/reject, seated clamp, VIEW 1/ES, no EyeProj spam, PubProj HOLD, F8 1cm=most fused, dual never on 74/76, never view+0x80, kill=45.

**Build:** `ASI_BUILD_ID 20260725-175453-mode74-jumpfix`  
**Play:** stereo **`74`**, dual **`0`**.

**Agent verify:** `Mode74: HITCHCUT BASELINE` · `dualOptIn=0` · `viewBudg=1` · UploadFn/PubProj stamp look · F8 1=MOST fused.

**You test (short English):**
1. Headset on — game should already be up.
2. Walk **streets** on **74** — less jump than remap? Street hitch better/same?
3. Look around — world should follow head (not cinema locked to body).
4. **F8** to **1 cm** — most fused.
5. Optional: stereo **`75`** only if you want dual again (expect hitch risk; tracking should work now).
6. Bad → stereo **`45`**, Quick Restart.

---

## Prior deployed 2026-07-25 — Mode map **74/75/76** (hitchcut-remap) — HARD FAIL

### Mode map (pick via `gtaiv_dxvk_vr.stereo`)

| stereo | Name | What it does |
|--------|------|----------------|
| **74** | **Hitchcut baseline (DEFAULT)** | Pair-hold temporal + DRAW HMD look/IPD + PubProj HOLD. Dual×2 **OFF**. Smoothest + presence. |
| **75** | Sparse session dual | Gated BuildRootA×2 1/8 after ~300 street-stable ES; hitch≥45ms → SESSION OFF. True same-frame test (may hitch/crash apt→city). |
| **76** | AER experimental | One engine eye/frame + `Submit_TextureWithPose`; pose/seat freeze per ES; dual OFF. Jump/perf-tuned — not default. |
| 72 | Temporal VS inject | Older temporal-only path. |
| 45 | Kill / LKG | Safe fallback. |

**Remap hard rules:** 71→45, 73→72. Never `view+0x80`. Dual is **not** every-frame default (apt→city safe).

### Why not AER as default

User on `mode74-aer`: 3D maybe better, but **heavy jumping** and **perf way worse** than hitchcut/temporalfirst. So **74 = hitchcut**, AER moved to **76**, dual kept as **75**.

### Fixes in this build

1. **IPD floor bug:** `Mode74OffsetViewLocal` forced half-sep ≥5cm (~10cm full) — F8 lowest did **not** fuse. Removed. **F8: 1cm = MOST fused → 10cm widest.**
2. **AER jump (mode 76):** pose freeze per ES (`GetFrameHmdPoseMatrix` for Submit); seated lean frozen once/ES (no mid-inject resample); EyeToHead stays cached.
3. **AER/hitchcut perf:** VIEW budget **1/ES** (was 2); no late CamMatrix refresh on 74-family; dual never arms on 74/76 from dual file.
4. Default files: stereo **`74`**, dual **`0`**, kill **`45`**.

**Build:** `ASI_BUILD_ID 20260725-174441-mode74-hitchcut-remap`  
**Play:** stereo **`74`**, dual **`0`**.

**Agent verify:** `Mode74: HITCHCUT BASELINE` · `dualOptIn=0` · `viewBudg=1` · `StereoSep: … F8: 1=MOST fused`.

**You test (short English):**
1. Headset on — game should already be up.
2. Walk **streets** — smoother than AER build? Less jump?
3. Press **F8** until **1 cm** — should be **most fused** (smallest gap between eyes). Higher F8 = more separated.
4. Want true same-frame dual? Put **`75`** in stereo file, Quick Restart (expect hitch risk).
5. Want AER again? Put **`76`** in stereo file, Quick Restart.
6. Bad → stereo **`45`**, Quick Restart.

---

## Prior deployed 2026-07-25 — Mode74 **AER presence** (superseded)

AER-as-default on stereo=74. User: jumping + worse perf → remapped above (74 hitchcut / 75 dual / 76 AER).

### RealVR ideas still available (mode 76 + shared DRAW path)

1. AER + `Submit_TextureWithPose` (mode **76**)
2. Engine FOV = HMD cover
3. Render-time worldlook + EyeToHead/IPD + toe-in
4. Shared FrameDesc; seated 6DoF jump-clamped
5. Dual×2 = mode **75** only (not default)

---

## Prior: Research 2026-07-25 — Luke Ross RealVR RE (certainty ≥98%)

Full write-up: **`docs/LUKE_ROSS_REALVR_RE.md`** (GTA5 + CP2077 mode matrix).

**Does he only use AER?** **Yes for stereo.** No Halo / same-frame dual mode in GTA5 *or* modern RealVR64 (CP2077).

| | GTA5 folder | CP2077 folder |
|--|-------------|---------------|
| Renderer | forked `d3d11.dll` | pack-root `RealVR64.dll` |
| Stereo | `Stereo=1` = **alternate eyes** only | Overlay **Render mode**: Legacy AER / AER v2 / 1/2…1/6 rate / Mono |
| Same-frame dual | **No** | **No** |
| AER v2 (CUDA OF) | **No** | **Yes** (NVIDIA) |

- GTA5 Submit = OpenVR ×2 + `Submit_TextureWithPose` (8); eye bit `'L'`/`'R'` in `RVRGetFrameDesc`.
- **Mode74 implication:** our AER + pose Submit **is** the RealVR analog. Dual×2 is *our* Halo-class experiment — Luke has nothing to port there.

---

## Prior deployed 2026-07-25 ~17:23 — Mode74 **hitchcut**

### User (after temporalfirst)

> Still hitching a lot on **streets**, smooth in **apartment**. Jumping back but rare. **Why dual off — don't we need that for true VR?**

### Why dual was OFF (clear answer)

**Yes — dual BuildRootA×2 IS needed for true same-frame VR** (both eyes from one game tick).

We turned it **default OFF** because:
1. **Every-frame dual** crashed apartment → city (streaming).
2. **Sparse dual** made the headset go stuttery ↔ smooth whenever a hitch latched dual off/on.

So dual=`0` is the **smoothness experiment**. Fake 3D (temporal L/R) stays on. True same-frame 3D is **opt-in**.

**How to turn dual ON (test true VR):**  
1. Open game folder  
2. Edit `gtaiv_dxvk_vr.dual` → put **`1`**  
3. Desktop **GTA IV Quick Restart**  
4. After ~10s stable street walking, sparse dual 1/8 arms; any hitch ≥45ms → dual stays off for that session (no thrash).  
5. Bad → put **`0`** back in dual, or **`45`** in stereo, then Quick Restart.

### Log root (temporalfirst streets)

- Apt smooth / street hitchy = more streaming + draws  
- Hot costs: **EyeProj “see” ~45k log lines**, PubProj ~20 stamps/ES, VIEW×2, FOV refresh, disk log I/O  
- Rare jump = pose / seated 6DoF spike (not dual)

### What changed (this build)

1. **Street hitch cuts:** EyeProj path **off** when dual OFF; PubProj **HOLD 1×/self/ES**; VIEW budget **1/ES**; FOV target cache **2s**; no Mode74 CamMatrix late refresh; quieter logs  
2. **Jump clamp:** reject huge HMD pose deltas (~10cm / ~28°) + seated step clamp (~8cm/frame)  
3. **Dual:** default file **`0`**; code ready for **`1`** = sparse 1/8 after 300 street-stable ES + SESSION OFF on hitch  
4. Never `view+0x80`; kill=**45**

**Honest:** with dual=`0` you get smoother streets, not true same-frame stereo. Dual=`1` is the true-VR test (may hitch again).

**Play now:** stereo **`74`**, dual **`0`**. Kill **`45`**.  
**Build:** `ASI_BUILD_ID 20260725-172331-mode74-hitchcut`.  
**Launch:** Game already restarted by agent (DirectExe).

**Agent verify (boot log):**
- `DUAL OFF (default)` + `STREET-SMOOTH` + `optIn=0 dualEn=0` + `viewBudg=1`
- **0** `EyeProj: see` lines (was ~45k)
- `PubProj: TEMPORAL HOLD` · pubProj≪ prior (~1k/600 ES vs ~10k)
- Pose/seat spike reject armed

**You test (short English):**
1. Headset on — game should already be up.  
2. Walk **streets** — **fewer hitches** than last build?  
3. Quick look around — **rare jumps gone/less?**  
4. Still want true VR depth? Put **`1`** in `gtaiv_dxvk_vr.dual`, Quick Restart, walk street ~10s.  
5. Bad/crash → stereo **`45`**, dual **`0`**, Quick Restart.

---

## Prior deployed 2026-07-25 ~17:15 — Mode74 **temporalfirst**

Dual default OFF + first hitch cuts + full cover FOV. User: streets still hitchy; asks why dual off → superseded by **hitchcut**.  
Build was `20260725-171527-mode74-temporalfirst`.

---

## Prior deployed 2026-07-25 ~17:09 — Mode74 **sessionlatch**

HARD OFF → SESSION OFF. User: load stutter then smooth; city still hitchy; no presence → superseded by **temporalfirst**.  
Build was `20260725-170927-mode74-sessionlatch`.

---

## Prior deployed 2026-07-25 ~17:01 — Mode74 **fpsfix**

HOLD-only-while-OPEN + temporal on HARD OFF. Superseded by sessionlatch → temporalfirst.  
Build was `20260725-170139-mode74-fpsfix`.

---

## Prior deployed 2026-07-25 ~16:55 — Mode74 **sparsedual**

User: better stereo direction; desktop fluent / headset low FPS → superseded by **fpsfix**.  
Build was `20260725-165539-mode74-sparsedual`.

---

## Prior deployed 2026-07-25 ~16:41 — Mode74 **hmdfov**

FOV→HMD cover; dual OFF. User: less cinema-ish but still fake 3D → superseded by **sparsedual** above. Build was `20260725-164113-mode74-hmdfov`.

---

## Prior deployed 2026-07-25 ~16:30 — Mode74 **worldlook**

HMD look stamp on ReplayDispatch VIEW. Look fixed; scale still giant TV → superseded by **hmdfov**. Build was `20260725-163104-mode74-worldlook`.

---

## Prior deployed 2026-07-25 ~15:59 — Mode74 **toein** (presence + RE)

User later: still giant screen / world moves with head → superseded by **worldlook** above. Build was `20260725-155908-mode74-toein`. Checkpoint `d5bb0ec` / `checkpoint-mode74-contentinj-20260725` still valid fallback.

---

## Prior deployed 2026-07-25 ~15:47 — Mode74 **contentinj** (checkpoint)

Slight depth; still not true VR. Saved as checkpoint **`d5bb0ec`** / tag **`checkpoint-mode74-contentinj-20260725`**. Build was `20260725-154721-mode74-contentinj`. Superseded by **toein** above.

---

## Prior deployed 2026-07-25 ~15:00 — Mode74 **crashsafe** (BuildRootA×2 OFF)

Crash-safe dual-off baseline. Presence weak (1 inject/ES, CamMatrix `ipd=0`). Superseded by **contentinj** above. Build was `20260725-150014-mode74-crashsafe`. Kill = **45**.

---

## Prior deployed 2026-07-25 ~14:44 — Mode74 **softskip** — FAILED (crash)

Load-pause/hysteresis/soft-skip **did not fire** (0 LOAD PAUSE / 0 SOFT SKIP). Dual stayed every-frame through streaming. Superseded by **crashsafe** above.

---

## Prior deployed 2026-07-25 ~15:18 — Mode74 **dualsep** (superseded — crash)

Presence: dual VIEW ±half IPD + D3DTS_VIEW + EndScene budget/c0. Smooth better; still not true VR. **Crashed** apartment→city (sticky dual during streaming). Superseded by **softskip** above.

---

## Prior deployed 2026-07-25 ~14:25 — Mode 74 **CamIPD dual** — still monitor

**User after PubProj:** **"All smooth but still flat dude"** — need IN the game, not cardboard.

### Why still flat (parallax audit)

| Check | Evidence |
|-------|----------|
| PubProj HOOK | PASS — L/R `ox=±0.07`, `pubProj` 100k+, `HOLD +0x180` |
| StereoDiff / haveL/R | PASS — absdiff ≠0, Submit L=1 R=1 |
| Mode72 VIEW inject during dual | **FAIL** — `injects` stall (~90→stuck), `contentInj=0` (BuildRootA dual does not feed view@c0 uploads) |
| CamMatrix IPD during dual (before) | **FAIL** — Mode74 skipped CamMatrix IPD (“VS owns it”) → `ipd=(0,0,0)` while dual → **no translational parallax** |
| IPD file mid-session | F8 left `ipd=1` briefly (restored to **6**) — not the root |

**Verdict:** PubProj asymmetry alone = slight L≠R without depth. Smooth OK. Presence needs eye separation on the **drawn camera** — CamMatrix during dual.

### What changed (one primary behavior)

**During Mode74 `StereoInDualPass()`, CamMatrix applies IPD again** (was skipped so Mode74 VS could own sep — but VS inject is silent in dual). Outside dual, Mode74 VS still owns e2h (no double). Also kept from probe builds: PubProj **HOLD +0x180** until eye BuildRootA ends (not restore mid-hook).

**Play was:** stereo **`74`**. Kill **`45`**. Soft **`72`**.  
**Build:** `ASI_BUILD_ID 20260725-1425-mode74-camipd` — superseded by **dualsep** above.

**Honesty / next if still flat:** VS view inject during dual still mostly dead (`contentInj=0`) — if CamMatrix IPD does not reach the bake path, next is find **who uploads the drawn view** on the replay thread (or per-eye CCam FOV `0x706F7C` only if frustum still wrong). Do **not** crank IPD as sole fix. Kill = **45**.

---

## Prior deployed 2026-07-25 ~14:04 — Mode 74 **PubProj** (smooth PASS / presence FAIL)

PublishProj `0x31BA0` → `+0x308` with HMD `ox=±0.07`. Smooth dual OK; headset still monitor. Superseded by **CamIPD dual** above. Build was `20260725-1404-mode74-pubproj`.

---

## Prior deployed 2026-07-25 ~13:52 — Mode 74 **EyeProj** (still flat / monitor)

Per-eye `GetProjectionRaw` → write `+0x180` + push proj @c4. Log armed / writes climbed; headset still monitor-in-face. Superseded by **PubProj** above. Build was `20260725-1352-mode74-eyeproj`.

---

## Prior deployed 2026-07-25 ~07:08 — Mode 74 **stable** (flicker/perf/jump fix)

**Headset feedback on `20260725-0654-mode74-hmd6dof`:** warp slightly better (not priority); **FPS almost halved**; vision **jumpy**; **~1Hz flicker** also in fallback `20260725-0648-mode74-hmdlook`.

### Flicker root cause (log + code)

Sticky gate already fixed fused↔separated from miss-close (0 CLOSE after OPEN on hmd6dof log). Remaining ~1Hz/jumpy came from **double HMD path**:
1. CamMatrix already applies HMD look + seated 6DoF + IPD.
2. Mode74 `ApplyHmdEyeLocal` **re-applied seated 6DoF** + EyeToHead on every VS inject, and sometimes called `GetEyeToHeadTransform` live.
3. L/R dual could see **different pose samples** mid-frame → yaw/IPD fight (log showed L/R yaw diverge within one dual).

### What changed (one stability behavior set)

- **Frozen pose** once per dual (`BeginFrameHmdPoseSample`) — L and R share one sample.
- **EyeToHead cached** once (refresh on F9) — never `VRSystem` in inject / CopyMat hot path.
- **Mode74 drops seated 6DoF** (CamMatrix keeps cheap seated lean).
- **Mode74 owns IPD/e2h** on VS src-copy; CamMatrix skips IPD while stereo=74 (no double sep).
- Light EMA on pose orientation to kill single-sample spikes.
- Sticky every-frame dual + inject-only-in-`g_inDual` + never view+0x80 unchanged.

**Play was:** stereo **`74`**. Kill **`45`**. Soft **`72`**.  
**Build:** `ASI_BUILD_ID 20260725-0708-mode74-stable`

**Fallback still:** tag `fallback-mode74-hmdlook-20260725` @ `1476229` if this regresses.

**Honesty / limits:** every-frame dual still costs ~2× render. Superseded by EyeProj section above for 3D feel. Kill = **45**.

---

## Prior deployed 2026-07-25 ~06:54 — Mode 74 **HMD×EyeToHead + seated 6DoF** (REGRESSED)

**User:** current build best so far → **git fallback saved**; still **"game world warps as you move your head"** → this build. **Headset later:** half FPS, jumpy, flicker → superseded by **stable** above.

### Fallback (do not lose)

- **Commit:** `1476229` — *Save Mode74 sticky stereo + HMD look as headset fallback.*
- **Tag:** `fallback-mode74-hmdlook-20260725` (build `20260725-0648-mode74-hmdlook`)
- Restore: `git checkout fallback-mode74-hmdlook-20260725` then rebuild/deploy that tree if this build regresses.

### Warp hypothesis

Head **rotation** on the SetVSConstF src-copy without matching **per-eye EyeToHead** (and without seated position) → wrong stereo pivot / shear while SteamVR timewarps. Asymmetric **HMD projection** still missing (Mode **15** `D3DTS_PROJECTION` tested dead; no known VS proj slot for Mode74 inject) — that remains the main residual FOV-warp risk.

### What changed (one primary behavior)

Mode74 src-copy now applies **HMD pose × `GetEyeToHeadTransform`** + **seated 6DoF** delta (F9 clears baseline via `HmdPoseOnRecenter`). When EyeToHead applies, **skip crude ±half IPD** (no double separation). Still: sticky gate, every-frame dual, inject-only-in-`g_inDual`, never write view+0x80, exception→45.

**Play was:** stereo **`74`**. Kill **`45`**. Soft **`72`**.  
**Build:** `ASI_BUILD_ID 20260725-0654-mode74-hmd6dof`

**Honesty / limits:** double 6DoF + live e2h hurt FPS/jump. Superseded by stable build. Kill = **45**.

---

## Prior deployed 2026-07-25 ~06:48 — Mode 74 **noflicker + HMD look** (fallback)

**User report (build `20260725-0635-mode74-every`):** no freeze, but **"jumps between a fused picture and two separated pictures"** (alternating).

### Part A — flicker root cause (log proof)

Hypothesis **1 confirmed** — gate re-close → Mode72 temporal fallback:

Live log on `20260725-0635-mode74-every` showed **~55×** `streaming gate CLOSED` and **~56×** `streaming gate OPEN` in one session. Examples:

- `Mode74: streaming gate OPEN after 90 live EndScenes …`
- `Mode74: es#360 gate=OPEN dualN=98 liveStreak=0 miss=43 …` ← miss climbing while duals still ran
- `Mode74: streaming gate CLOSED after 60 miss EndScenes (no live inject — likely load/streaming; back to Mode72 temporal)`
- then OPEN again → CLOSE again … throughout play

When **OPEN**: every-frame same-frame dual → separated L≠R (`StereoDiff absdiff≠0`).  
When **CLOSED**: Mode72 **temporal** L/R eye flip + pair-hold → feels fused / flat vs separated → **fused↔separated flicker**.

Miss detector was a **false positive** during dual: EndScene can outpace BuildRootA / Mode72 inject counter stalls while CamMatrix still supplies L≠R. Closing mid-session was wrong for normal play.

**Fix (one primary behavior change):** Mode74 gate is **STICKY OPEN** after first open — no miss re-close; while OPEN never call `TemporalCapturePairHold` (no temporal eye flip). Dual-this-frame also counts as live. Still: inject-only-in-`g_inDual`, never write view+0x80, exception→45.

**Agent verify (`20260725-0644-mode74-noflicker`):** after gate OPEN → **0** further CLOSE; `stickyOpen=1`; `liveStreak` climbed 1000+ with `miss=0`; `dualN` tracked ES; `StereoDiff absdiff≠0`; no EXCEPTION.

### Part B — HMD look (second build, same session)

**Change:** Mode74 SetVSConstF **src-copy** now stamps **HMD orientation** onto the local matrix before IPD translate (never writes live view+0x80). CamMatrix late refresh log tagged `HmdLook: Mode74…`. Clear enable lines: `HmdLook: ENABLED` / `HmdLook: ACTIVE`.

**Play was:** stereo **`74`**. Kill **`45`**. Soft **`72`**.  
**Build:** `ASI_BUILD_ID 20260725-0648-mode74-hmdlook` — saved as git fallback above. Superseded by **hmd6dof** section.

**Honesty / limits:** still matrix IPD stereo (not asymmetric HMD projection). HMD look is orientation on the VS upload + CamMatrix — not full 6DoF room-scale. Sticky gate means dual stays on after open (mission-load empty-world risk theoretically higher than miss-close; every-frame dual already accepted). Kill = **45**.

---

## Headset 2026-07-25 ~06:38 — Mode 74 every-frame 1/1 **MAJOR PASS** (superseded by noflicker)

**User:** **"no freeze"**.

**Build confirmed:** `ASI_BUILD_ID 20260725-0635-mode74-every` · stereo **`74`** · soft **`72`** · kill **`45`**.  
(Deploy file `gtaiv_dxvk_vr.buildid` = `20260725-0635-mode74-every`.)

**PASS — every-frame same-frame BuildRootA×2 actually ran (not just “no freeze”):**
- `ASI_BUILD_ID 20260725-0635-mode74-every` · `StereoMode: 74` · cadence `every=1/1` (DualEveryN=1)
- Gate: `Mode74: streaming gate OPEN after 90 live EndScenes (… every-frame BuildRootAx2 dual 1/1 …)`
- Same-frame duals past old hang (#5): `#1` → `#5` → `#6` → `#120` … live to **`dualN≈9664`** / `SAME-FRAME dual #8880+` with `haveL=1 haveR=1` and `gate=OPEN every=1/1`
- Example late line: `Mode74: es#14880 gate=OPEN dualN=9664 … every=1/1`
- `StereoSubmit: L=1 R=1 mode=74` throughout
- `StereoDiff: … absdiff=8675` / `11551` / `21160` / `9816` / `11655` (≠0 → real L≠R)
- **0** `EXCEPTION` lines; no auto-45

**MAJOR PASS meaning:** gated + inject-only-while-`g_inDual` made continuous every-frame dual stable after the old #1–#5 hang. Sparse ladder (1/8→1/4→1/2→1/1) complete.

**Follow-up user:** fused↔separated flicker → fixed by sticky gate (section above). HMD look shipped in same session.

**Play:** keep stereo **`74`**. Kill anytime: **`45`**.

---

## Deployed 2026-07-25 ~06:35 — Mode 74 every-frame 1/1 → **PASS above**

**One behavior change:** `kMode74DualEveryN` **2 → 1** (every BuildRootA after gate = same-frame dual). Also fixed `tick % 1 == 1` never-true bug so EveryN=1 actually duals. Gate 90 live ES / inject-only-`g_inDual` / never view+0x80 / SetVSConstF src-copy / exception→45 / miss re-close / remap 73→72 / 71→45 kept. No IPD/camera/projection change.

**Build:** `ASI_BUILD_ID 20260725-0635-mode74-every` · stereo **`74`** · soft **`72`** · kill **`45`**.

**Headset:** user **"no freeze"** → MAJOR PASS section above (duals to thousands, absdiff≠0, 0 EXCEPTION).

---

## Prior deploy 2026-07-25 ~06:29 — Mode 74 sparse 1/2 → user go-ahead

**User:** **"no freeze, go ahead"** after Mode74 **1/2 PASS** → shipped every-frame above.

**Build then:** `ASI_BUILD_ID 20260725-0629-mode74-1of2` · stereo **`74`** · soft **`72`** · kill **`45`**. Superseded by every-frame 1/1.

---

## Headset 2026-07-25 ~06:27 — Mode 74 sparse 1/4 PASS (no freeze)

**User:** game running / playing fine with newest changes, **no freezes**. Treat as Mode74 **1/4 PASS**.

**Build confirmed:** `ASI_BUILD_ID 20260725-0621-mode74-1of4` · stereo **`74`** · soft **`72`** · kill **`45`**.
(Deploy files: `gtaiv_dxvk_vr.buildid` = `20260725-0621-mode74-1of4`, stereo file = `74`.)

**PASS — dual actually ran this session (not just “no freeze”):**
- Live log (still writing ~06:27+): `Mode74: … gate=OPEN dualN=… sparse=1/4`
- Same-frame duals far past old hang (#5): e.g. `SAME-FRAME dual #720` → `#1200` → `#4200` → `#4800` with `haveL=1 haveR=1` and `gate=OPEN sparse 1/4`
- Cadence examples: `es#2880 gate=OPEN dualN=654 … sparse=1/4` · later `es#17040 gate=OPEN dualN=4194 … sparse=1/4`
- `StereoSubmit: L=1 R=1 mode=74` throughout sampled lines
- `StereoDiff: … absdiff=15578` / `8045` / `8391` / `8549` (≠0 → real L≠R on sampled dual frames)
- **0** `EXCEPTION` lines; no auto-45
- `PlayGTAIV.exe` still up (started ~06:23); log LastWrite kept advancing while user played

**False alarm corrected:** earlier agent note that Steam was “LAUNCHING” / game not seen / `GTAIV.exe` gone was **wrong** — user was already playing. Do not treat that as a hang.

**Honesty:** still sparse **1/4** = ~1 true same-frame dual + **3/4** Mode72 temporal frames. Superseded by denser **1/2** deploy above.

**Play was:** stereo **`74`**. Kill=**`45`**.

---

## Deployed 2026-07-25 ~06:21 — Mode 74 denser sparse 1/4 → PASS above

**One behavior change:** `kMode74DualEveryN` **8 → 4** (1 dual + 3 Mode72 temporal frames). Gate / inject-only-in-dual / exception→45 / re-close on miss streak unchanged.

**Build:** `ASI_BUILD_ID 20260725-0621-mode74-1of4` · stereo **`74`** · soft **`72`** · kill **`45`**.

**Early agent-watch (before false “game gone” alarm):** Gate CLOSED→OPEN; dual #1→#5→#6 past hang; climbed to dual #1200+ / `sparse=1/4` / `StereoDiff absdiff≈11k` / no EXCEPTION. Superseded by headset **PASS** above (duals continued to #4800+ while user played).

---

## Headset 2026-07-25 ~06:18 — Mode 74 sparse 1/8 PASS (no freeze)

**User:** **"no freeze"**.

**Build confirmed:** `ASI_BUILD_ID 20260725-0615-mode74-sparse` · stereo **`74`** · soft **`72`** · kill **`45`**.

**PASS — dual actually ran (not just “no freeze”):**
- Gate CLOSED through load: `Mode74: es#240 gate=CLOSED dualN=0 liveStreak=76 miss=0 injects=76 … sparse=1/8`
- Gate OPEN: `Mode74: streaming gate OPEN after 90 live EndScenes (… sparse BuildRootAx2 dual 1/8 frames …)`
- Same-frame dual armed and kept climbing past the old hang point (#5):
  - `Mode74: SAME-FRAME dual #1 haveL=1 haveR=1 injects=91 … (gate=OPEN sparse 1/8 …)`
  - `#2` injects=98 → `#5` 119 → `#6` 126 (survived where continuous Mode74 hung)
  - `#120` injects=924 → `#240` 1764 → `#480` 3445
- Cadence: `es#4080 gate=OPEN dualN=479 … injects=3438 … sparse=1/8` (still live; no hang)
- `StereoSubmit: L=1 R=1 mode=74` throughout
- `StereoDiff: … absdiff=10540` and `absdiff=9896` (≠0 → real L≠R on sampled dual frames)
- No `EXCEPTION` / no auto-45; PedHide `dead=0`

**Honesty — was temporal-heavy at 1/8:** only ~1 of 8 BuildRootA ticks true same-frame; superseded by denser **1/4** above.

**Play was:** stereo **`74`**. Kill=**`45`**. Soft=**`72`**.

---

## Prior — Mode 74 freeze AFTER gate OPEN (2026-07-25 ~06:14) — FIXED by sparse

**User:** Freeze very shortly after loading screen; process hung (not crash).

**Log evidence (build `20260725-0609-mode74-gated`):**
- Gate CLOSED through load OK: `Mode74: es#240 gate=CLOSED … liveStreak=74 miss=0 injects=74`
- Gate **did** open: `Mode74: streaming gate OPEN after 90 live EndScenes …`
- Dual ran 5× then silence (no EXCEPTION / no auto-45):
  - `Mode74: SAME-FRAME dual #1 … injects=239`
  - `… dual #2 … injects=537` → `#3 835` → `#4 1133` → `#5 1431` (~+298 injects/dual frame)
- Last lines = dual #5; PlayGTAIV hung. Freeze was **AFTER** gate OPEN, **not** Mode72 CLOSED path.

**Root cause:** Continuous BuildRootA×2 every frame after gate still hang-prone in-world.
**Fix shipped:** sparse **1/8** after gate + inject only while `g_inDual` → **PASS** above.

---

## Prior test — Mode 74 GATED same-frame (2026-07-25 ~06:10) — FROZE

**Build:** `20260725-0609-mode74-gated` · stereo **`74`**  
Superseded by sparse fix. Continuous dual after gate hung at dual #5.

---

## Headset 2026-07-25 ~06:02 — Mode 73 empty world (DISABLED)

**User:** no crash, but world objects never load (empty + load spinner).  
**Cause:** BuildRootA×2 every frame breaks Rage streaming/load.  
**Action:** Mode **73** remaps → **72**; killed process; redeploy Mode72 temporal (world loads OK).

**Play now:** stereo **`72`**. Kill=**`45`**.  
**Next same-frame:** must NOT double BuildRootA during load — e.g. dual only after in-world gate, or dual at Present with a safer seam.

---

## Headset 2026-07-25 ~05:59 — Mode 73 SAME-FRAME live (agent watch)

**Build:** `20260725-0558-mode73-sameframe` · stereo **`73`**  
**Log:** `Mode73: SAME-FRAME dual #… haveL=1 haveR=1` · `StereoDiff absdiff=425/131/433/180` (≠0 = real L≠R)  
sometimes `absdiff=0` (identical pair). Process stayed up. No auto-45.

**Meaning:** First **same-frame** dual path that is stable + proves image difference. Uses BuildRootA×2 + SetVSConstF src-copy (no live view write).

**User:** Check headset — should feel more “real stereo” than Mode72 temporal. Kill=**`45`**.

---

## Next test — Mode 73 SAME-FRAME dual (2026-07-25 ~05:58)

**Build:** `20260725-0558-mode73-sameframe` · stereo **`73`**  
**What:** BuildRootA ×2 (L then R) + Mode72 SetVSConstF **src-copy** inject. No live `view+0x80` write.  
**Expect:** `Mode73: SAME-FRAME dual #… haveL=1 haveR=1` + `StereoDiff` absdiff≠0.  
**Kill:** stereo **`45`** (auto on dual exception).

---

## Headset 2026-07-25 ~05:54 — user: still “monitor”, barely different

**Honest status:** Mode **72** is **not** true stereo yet. It only nudges the VS view matrix
±IPD/2 on alternating frames (temporal). Motion-guard was often copying **one** BB to both
eyes → looks mono. Small IPD (~2 cm) made any leftover parallax nearly invisible.

**Fix now:** Mode72 motion-guard **OFF** + inject halfSep **floor 5 cm** (10 cm total) so L≠R
is visible enough to judge. Still temporal; SameFrameSeamGate=CLOSED.

---

## Headset 2026-07-25 ~05:53 — Mode 72 PASS (stable + some depth)

**User:** Spiel läuft gut, kein Hängen; **etwas Tiefe** sichtbar.  
**Build:** `20260725-0549-mode72-vscopy` · stereo **`72`** (SetVSConstF src-copy @ `0x220D0`).  
**Follow-up:** raised `gtaiv_dxvk_vr.ipd` **2→5** cm for clearer parallax (still safe vs 25 cm half-cap).

**Meaning:** First working true-ish stereo path that does not freeze. SameFrameSeamGate still CLOSED (temporal L/R).  
**Kill:** stereo **`45`**. Next options: tune IPD/F8 further, or later same-frame dual without mutating view object.

---

## Headset 2026-07-25 ~05:50 — Mode 72 VSCONST src-copy (live)

**Build:** `20260725-0549-mode72-vscopy` · stereo **`72`**  
**Approach:** MinHook SetVSConstF @ **`0x220D0`** — offset a **local copy** of src when `src==activeView+0x80`; **never** writes the live view object (Mode71 freeze root).

**Agent watch:** armed OK; in-world ~1 inject/ES (`es#960 injects=791`); process still up; no EXCEPTION / no auto-45.

**User check:** Freeze gone? Any depth/parallax? (IPD still ~2 cm — weak.) Kill=**`45`**.

---

## EMERGENCY 2026-07-25 ~05:45 — Mode 71 DISABLED (freeze×2)

**User:** Bild kurz besser → Freeze, Sound kurz weiter, Prozess hängt. Auch nach SAFE-Build.

**Log (SAFE):** injects climbed ~1/ES (`es#720 injects=552`) then hard hang — no EXCEPTION line.

**Root cause (working theory):** mutating live `activeView+0x80` in place (even 1×/frame + restore) is unsafe — engine/other readers see torn/wrong cam → hang. Mode **68/69/70** read-only hooks were stable; **writes** are the difference.

**Action taken:**
- Killed hung `PlayGTAIV`
- stereo → **`45`**
- Mode **71** hard-disabled: `RemapLoadedStereoMode(71)→45` + `Mode71InstallInjectHook` never arms
- buildid: disable stamp below

**Next (freeze fixed first):** inject via **copy** (hook `0x220D0` SetVSConstF and offset the *src buffer arg* without writing the view object) — or same-frame dual elsewhere. Do **not** re-enable in-place `view+0x80` writes.

**Kill / play:** stereo **`45`**.

---

## Headset 2026-07-25 ~05:43 — Mode 71 SAFE after freeze

**Problem:** first Mode71 freeze after brief “looks better” image. Cause: ~200 injects/frame + bad matrices (`right` length ≫1, e.g. 10000) → camera teleport; also dirty matrix if call faulted before restore.

**Fix build:** `20260725-0543-mode71-safe`
- 1 inject per EndScene only
- require unit-length right + finite + `t²≥10`
- always restore after call
- half-sep capped at 25 cm

**Live log:** in-world `injects` ≈1/ES (e.g. +120 over 120 ES), not 16k/ES. Process stayed up in agent watch. stereo stays **71**.

**User:** load in, check freeze + parallax. Kill=**45**. IPD file still ~2 cm (weak); raise if stable but flat.

---

## Headset 2026-07-25 ~05:38 — Mode 71 TEMPORAL-INJECT live

**Build:** `20260725-0537-mode71-inject` · stereo stays **`71`** (no force-45)  
**Log:** `INJECT armed @0x30CD0` → in-world `injects` climbing (e.g. 16k+/ES burst), `skip` = non-active/identity  
**What it does:** on `self==activeView` + live matrix, offset `view+0x80` by ±IPD/2 along right, upload, restore. Mode-45 temporal L/R capture. SameFrameSeamGate=CLOSED.

**User check:** In headset, do near objects show **depth/parallax** vs flat Mode 45?  
If too weak: raise `gtaiv_dxvk_vr.ipd` (currently ~2cm in log). Kill = stereo **`45`**.

---

## Headset 2026-07-25 ~05:36 — Mode 70 ACTIVE-PEEK = PASS

**Build:** `20260725-0535-mode70-active`  
**Log:** `ACTIVE+LIVE view=… active=0x… t=(892…)` → `cadence=PASS hit=5985 miss=0 hitRate=100.0%`  
**stereo file stayed `70`** (no force-45 on success).

**Meaning:** Every live `view+0x80` upload was the activeView object. Dual inject filter = `self == *[0x17F583C]` + live matrix.

**Next:** Mode **71** temporal L/R inject at that filter (Mode-45 capture path). Kill still **`45`** on hook death only.

---

## Headset 2026-07-25 ~05:31 — Mode 69 WAIT+LIVE = PASS

**Build:** `20260725-0530-mode69-wait`  
**Log:** WAIT through menu/load (identity) → **`LIVE matrix t=(892.519,-20.271,499.430)`** once in world → `cadence=PASS` `maxDelta2` huge · auto→**`45`**.

**Meaning:** `view+0x80` **does** carry a live camera translation in-world (not only identity).  
Mixed with identity peeks in the same frame burst (~200–250 entries/ES) — dual inject must filter the live view object, not blindly patch every ReplayDispatch entry.  
SameFrameSeamGate still **CLOSED**. Kill=**`45`**.

**Next:** offline dual plan / filter (activeView match or non-identity gate) — one step, still no dual until reviewed.

---

## Headset 2026-07-25 ~05:26 — Mode 69 PEEK PASS (identity caveat)

**Log:** `ASI_BUILD_ID 20260725-0525-mode69-peek`  
`Mode69: PEEK armed exeRva=0x30CD0` → `ok=180 fail=0 cadence=PASS`  
Readable `float[16]` at view+0x80 every entry. Auto→**`45`**.

**Caveat:** all samples were **identity** (`t=(0,0,0)` `r0=(1,0,0)` `maxDelta2=0`) — likely menu/load or not the live cam translation layout.  
**Next:** one calm **in-world** retest of **69** (walk/turn — expect nonzero `t` / `maxDelta2`), OR offline dump more of the view object to find the live cam floats. Still **no dual**. Kill=**`45`**.

---

## Next test — Mode 69 MAT-PEEK (ready)

**Build:** `20260725-0525-mode69-peek` in `out-asi\`  
**What:** READ-ONLY copy of `float[16]` at **view+0x80** inside ReplayDispatch (same site as 68). No dual.  
**Expect:** `Mode69: PEEK armed` → `PEEK view=… t=(…)` → `cadence=PASS|PARTIAL|REJECT` → auto **`45`**.

---

## Headset 2026-07-25 ~05:24 — Mode 68 PASS

**Log:** `ASI_BUILD_ID 20260725-0359-mode68-count`  
`Mode68: COUNT armed exeRva=0x30CD0` → `avgEntries=4.00 cadence=PASS`  
Stable `entries=4` every EndScene over 45 ES. Auto-wrote stereo **`45`**.  
**Meaning:** ReplayDispatch is an owner-like seam (bridge → slot178 `0x220D0`).  
SameFrameSeamGate stays **CLOSED** — no dual yet. Kill = **`45`**.

**Next:** Mode **69** matrix peek (read-only).

---

## Overnight offline 2026-07-25 ~03:59 — Mode 68 COUNT ready

**Shipped to `out-asi\` only (NOT deployed to game):**
- Mode **68** `ReplayDispatchCountProbe` — COUNT-only MinHook @ mapped **`0x30CD0`**
- Fixed parse caps (was rejecting stereo>67) in `stereo_config.cpp`
- buildid: **`20260725-0359-mode68-count`**
- Dual=OFF · SameFrameSeamGate=CLOSED · auto→**45**

**Identity chain (headset-proven):**
- Mode **65** OK: live `slot178=0x220D0` (DXVK SetVSConstF)
- ReplayDispatch **`0x30CD0`** → `lea arg,[view+0x80]` → `call [eax+0x178]` @ **`0x30D0D`** → **`0x220D0`**

**Next headset test (one mode):**
1. Quit GTA if running
2. `.\scripts\deploy-asi.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"`
3. Set stereo file to **`68`**
4. Expect: `Mode68: COUNT armed exeRva=0x30CD0` then `cadence=PASS|SHARED|REJECT`
5. Kill / auto = **`45`**. **No dual.**

---

## Headset ladder done 2026-07-25 ~03:51

| Mode | Result |
|------|--------|
| **66** | **PASS** `avgEntries=3.00` @ `0x30F00` |
| **67** | armed OK, COUNT **REJECT** `avg=0.16` (sparse ViewMat) |
| **65** | **OK** stable `slot178=0x220D0` |
| **42** | playable; `VsRetHits=0` — no OWNER-EDGE |
| **64** | armed @ `0x32470`, COUNT **REJECT** `avg=0.00` (gate dead, as predicted) |

Stereo file = **45**. Next headset: stereo **68** ReplayDispatch COUNT. SameFrameSeamGate stays CLOSED.

## Headset 2026-07-25 ~03:48 — Mode 42 playable, OWNER-EDGE dry

**Log:** Mode **42** ran long (`epoch` thousands), no freeze.  
`VsRetHits=0 nodes=0` entire session — **no** `OWNER-EDGE` lines (VsRet stack probe collected nothing).  
Useful negative: Mode **65** `slot178=0x220D0` remains the live upload target evidence. Next optional: **64**.

## Headset 2026-07-25 ~03:45 — Mode 65 OK

**Log:** stable `slot178=0x220D0` bytes `8B 54 24 04 57 8B 7C 24`, `replayGate=0`, later `activeView` non-zero.  
No hook/crash. Next: **42** OWNER-EDGE triad.

## Headset 2026-07-25 ~03:40 — Mode 67 armed, COUNT REJECT

**Log:** `Mode67: COUNT armed exeRva=0x314C0` then `avgEntries=0.16 cadence=REJECT`  
Hook/AOB OK, no freeze; ViewMatWriter rarely entered (dirty-rebuild path). Next: **65**.

## Headset 2026-07-25 ~03:38 — Mode 66 PASS

**Log:** `Mode66: COUNT done exeRva=0x30F00 avgEntries=3.00 cadence=PASS`  
Armed at mapped PublishSync; auto-reverted to **45**. Next: **67**.

---

## Offline continue 2026-07-25 ~03:07+ (no headset / no deploy)

**Shipped to `out-asi\` only:**
- Mode **41/42** OWNER-EDGE covers full ReplayGate +178 triad:
  - `ret=0x30D13` → call `0x30D0D` (ReplayDispatch)
  - `ret=0x2A2183` → call `0x2A217D` (UploadA in `0x2A1E10`)
  - `ret=0x2A25FF` → call `0x2A25F9` (UploadB in `0x2A1E10`)
- Log: `path=ReplayDispatch|UploadA|UploadB` + `match=0/1`
- Still READ-ONLY — SameFrameSeamGate=CLOSED, no dual
- Prefer later test order: **66 → 67 → 65 → 42 → 64**

---

## While you were playing (2026-07-25 ~01:36–03:36 — offline, no deploy)

**Root cause of Mode 66 REJECT:** PE section skew — CE `.text` **mapped RVA = file offset + 0xC00**.
Doc/Mode used file offset `0x30300` as RVA; live PublishSync is **`0x30F00`**.

**Shipped to `out-asi\` only (NOT deployed — you were in-game):**
- `ResolveReSite()` AOB resolver (`re_validate.h/cpp`)
- Mode **64/66/67** AOB + mapped RVAs: ViewConst **`0x32470`** (TRUE; mid `0x3247C` unsafe) / PublishSync **`0x30F00`** / ViewMat **`0x314C0`**
- Mode **66** no longer requires CC-pad (PublishSync follows epilogue)
- Mode **41/42** PUBLISH-RET notes; Mode **65** logs slot178 first bytes
- `scripts/offline-seam-mapped.py` — PE-mapped deep scan
- MatMul **`0x307F0`** (docs had typo `0x307BF0`); ReplayDispatch **2** E8 callers; ViewConst TRUE **1** E8 (gated)
- PublishProj **`0x31BA0`**: **11** callers; **11/12** PublishSync sites immediately call it — Mode **66** covers proj tails
- 12th PublishSync path: activeView thunks **`0x32B40`/`0x32B60`** → copy helper **`0x31940`** (×**58** E8) → PublishSync-only `@0x3199F`
- Mode **66** COUNT: avg in `[0.8,4]` = PASS; **avg>4** = **SHARED** (still useful, not REJECT); avg<0.8 = REJECT
- ViewConst gate **`[0x1797694]`**: BSS, **no static writer** — Mode **64** COUNT may always be 0 (gate starts 0; needs nonzero; no writer)
- **12** exe-wide `call [reg+0x178]` — stereo path is **`0x30D0D`**; VsRet obs stays **`0x2D33E`** (do not confuse with mapped `0x2C73E` epilogue after another +0x178 call)
- Viewport **`0x31110`** / apply **`0x22FD0`** documented
- Ladder: **`45 → 66 → 67 → 65 → 64 → 41 → 42`**
- Cheat sheet: `docs/MAPPED_RVA_CHEATSHEET.md`
- buildid in `out-asi\`: `20260725-0354-gate178ABAB` (Mode66 PASS/SHARED/REJECT; PublishSync 12-site table)
- Upload fn **`0x2A1E10`**: TWO MatMulx3→+178 paths (@0x2A217D after helper; @0x2A25F9 second); sibling **`0x2A1D50`** helper-only
- Mode **42** OWNER-EDGE (`ret=0x30D13`) may **miss** `0x2A1E10` uploads — Mode **65** slot178 still sees all paths
- Only **3** +178 sites are ReplayGate-guarded: `0x30D0D` + upload `@0x2A217D/@0x2A25F9`
- Publish chain: **PublishSync → (if activeView) ReplayDispatch → `call [eax+0x178]`** — Mode **66** counts Sync; Mode **42** edges slot178
- ResolveReSite prefers **expected mapped RVA** when prologue matches (guards multi-hit short AOBs)
- Helper **`0x31940`**: copy→`0x30720`→PublishSync; thunk **`0x32B40`** pushes global **`0x1110090`**
- **AGENT_LOOP_TICK_re2h killed** (pid 37992, terminal 515193 failed) — continuous work only, no 15m ticks; do not re-arm

**When you return:**
1. Quit GTA (or finish session)
2. `.\scripts\deploy-asi.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"`
3. Optional restore: `fovadd=18`, `ipd=3`, `scale=100`
4. One test first: stereo **`66`** → must see `Mode66: COUNT armed exeRva=0x30F00` then real `avgEntries` (not REJECT at 0x30300)
5. Then **`67`** → **`65`** → **`64`** (64 may stay 0) one at a time

**Do not dual.** Kill = **`45`**.

---

## Session 2026-07-25 ~01:33 — Mode 66 headset: renderer OK, COUNT REJECT

**User:** Mode **66** ausprobiert — **funktioniert** (kein Freeze/Blackscreen).

**Log:**
- `StereoMode: 66` → Mode-45 renderer path (`countHook=0`)
- `ReValidate: anchor DRIFT PublishSync 0x30300`
- `Mode66: REJECT … bytes=FF 83 F8 FF 74 0C` (need `55 8B EC 83 E4 F8 51 56 8B F1`)
- `Mode66: COUNT done … avgEntries=0.00 cadence=REJECT` (hook never armed — zeros)
- Auto-wrote stereo **`45`**; live submits `StereoSubmit … mode=45` (long session OK)

**Meaning:** Mode **45** family is playable again. Fixed RVA **`0x30300`** does **not** match
live bytes (offline PE still has correct prologue there; live read ≠ file — patch/module
skew; same class of drift as FovSite logging `0x706F7C` vs doc `0x70637C`). **No dual.**
SameFrameSeamGate stays **CLOSED**.

**Config now:** stereo **`45`**, `fovadd` **MISSING**, `ipd=1`, `scale=3000` (leanGain **30**
from F7 max — very small lean). Optional restore: `fovadd=18`, `ipd=3`, `scale=100`.

**Next (one at a time):** Mode **65** (vtable peek, no hook) · or fix PublishSync via **AOB**
before retrying 66/67 COUNT · Mode **64** only after AOB/RVA confirm.

---

## EMERGENCY 2026-07-25 ~01:25 — post-load blackscreen/freeze report

**User:** blackscreen + freeze after load screen (self-started after deploy).

**Log fact (same session):** Mode **45** DID arm and keep submitting after load —
`StereoMode: 45`, `mode 45 HEAD-OWNED … ok=1 fovSite=1`, `StereoSubmit … mode=45`,
`CamMatrix: FP lock`, `FovSite 45→63`, through `MonoSubmit #840` / PedHide#300.
**Not** a Mode-46 hard hang (those stop EndScene). Soft hang / HMD-black / control lock
still possible; treat as fail-safe.

**Kill applied on disk now:**
- stereo **`30`**
- **`fovadd` deleted** (Mode-30 comfort kill)
- ASI redeployed; buildid stamp `20260725-0125-kill30`

**User next:** Start GTA from Steam yourself. Expect Mode **30** (pair-hold, no late
head-owned CopyMat). Log: `StereoMode: 30`, `StereoSubmit … mode=30`.

If **30** OK → 45 late-cam/FOV path is the suspect. If **30** also freezes → not today's
Mode-67 work (shared stack / DXVK / SteamVR).

---

**Built:** `out-asi\gtaiv_dxvk_vr.asi` + `openvr_api.dll` — Mode **45** default path, RE probes **64/65/66/67** (COUNT-only / read-only peek), F7 **11-step** leanGain to **30.0**. No new risky hooks. SameFrameSeamGate **CLOSED**.

**Config on disk:** stereo **`45`**, **`ipd=3`**, scale **`100`** (leanGain **1.0**), **`fovadd=18`**.  
**Hotkeys:** F9=SteamVR recenter · F6/F7/F8 stereo scale / world lean / IPD.

### Deploy (before headset)

1. Copy `out-asi\gtaiv_dxvk_vr.asi` + `openvr_api.dll` → GTA IV folder — or run `.\scripts\deploy-asi.ps1`
2. Start game; log should show `StereoMode: 45` and `StereoSubmit: L=1 R=1 mode=45`

### Test ladder (one mode per session — edit `gtaiv_dxvk_vr.stereo`, restart each step)

| # | stereo | Pass log line |
|---|--------|---------------|
| 0 | **45** | `StereoSubmit: L=1 R=1 mode=45` — smooth baseline |
| 1 | **66** | `Mode66: COUNT done … cadence=PASS` (@**`0x30F00`**) — prefer first |
| 2 | **67** | `Mode67: COUNT done … cadence=PASS` (@**`0x314C0`**) |
| 3 | **65** | `Mode65: VT178-LOG … slot178=0x…` (+ bytes) |
| 4 | **64** | `Mode64: COUNT done …` (@**`0x32470`**) — may be sparse (gated) |
| 5 | **41** | `Mode41: CHAIN …` (ignore mid-SSE `0x3259A`; watch `PUBLISH-RET`) |
| 6 | **42** | `Mode42: OWNER-EDGE … ret=0x30D13` |

**F7 leanGain (11 steps):** `eyeDelta = hmdDelta / leanGain` — higher = smaller world when you lean.  
Cycle: **1.0 → 1.25 → 1.5 → 2.0 → 2.5 → 3.0 → 4.0 → 5.0 → 8.0 → 12.0 → 30.0** (file stores ×100).

**Kill anytime:** write **`45`** to stereo file (modes **64/65/66/67** auto-revert when done).

**RE map:** [`docs/RE_OFFSETS.md`](RE_OFFSETS.md) · [`docs/MAPPED_RVA_CHEATSHEET.md`](MAPPED_RVA_CHEATSHEET.md) · `scripts/verify-mapped-sites.py`.

---

### Session 2026-07-25 ~01:15 (ViewMatWriter 0x308C0 + Mode 67 — no headset)

**Shipped:**
- **Correction:** ViewMatWriter start is **`0x308C0`** (CC-pad frame thiscall), **not** mid **`0x308C9`**
- **Mode 67** — COUNT-only @ **`0x308C0`** (9 static `E8` callers; builds `[this+0x1C0]` then PublishSync); auto-reverts to **45**
- **`re_validate.cpp`** + **`offline-re-scan.py`** — prefer frame prologue; Mode 67 anchor
- Ladder now **`45→64→65→66→67→41→42`**

**Still blocking perfect VR:** temporal L/R; replay owner not proven; COUNT results pending headset.

---
**As of (prior):** 2026-07-25 ~05:00–07:30 (deep offline RE — no headset)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — full call graphs for **`0x2FFF0`/`0x3187C`/`0x30300`/`0x300D0`**, safe ladder table, forbidden list
- **`scripts/offline-re-scan.py`** — disasm, E8 graphs, VS-region + COUNT candidate scans
- **`re_validate.cpp`** — anchor bytes for Mode **64/66** targets + summary logging
- **Modes 64/65/66** — COUNT-only @ **`0x3187C`**, read-only vtable peek, COUNT-only @ **`0x30300`** (all on Mode 45 renderer; auto-revert to **45**)
- **F7** 11-step leanGain ladder to **30.0**

**Still blocking perfect VR:** temporal L/R; replay owner not proven. Best COUNT lead now **PublishSync `0x30F00`** (Mode **66**); ViewConst TRUE **`0x32470`** is gated.

---
**As of (prior):** 2026-07-25 ~04:30 (RE offset map + Mode 62/63 scaffold; no game launch)
**Deployed now:** stereo **`45`**, **`ipd=3`**, scale **`100`** (leanGain **1.0**, F7 **8 steps**),
**`fovadd=18`**. New offline docs: **`docs/RE_OFFSETS.md`**. Optional RE test modes **`62`** /
**`63`** (same renderer as 45; disabled by default).

**Hotkeys:** F9=SteamVR recenter · F6/F7/F8 stereo scale / world lean / IPD.

**F7 leanGain (8 steps):** `eyeDelta = hmdDelta / leanGain`. **Higher leanGain = smaller world**.
F7 cycles: **1.0 → 1.25 → 1.5 → 2.0 → 2.5 → 3.0 → 4.0 → 5.0**. File stores **leanGain×100**.

**RE session (offline):** Full PE scan of CE `GTAIV.exe` — see **`docs/RE_OFFSETS.md`**. Key
findings: **`VsRet=0x2C73E`** and **`0x2C6AC`** share wrapper **`0x2C180`**; **zero** static
`E8→VsRet` (indirect-only); CopyMat **`0x83DB90`**, FovSite **`0x70637C→0x706A00`**, BuildRootA
**`0x8F7F00`**. **SameFrameSeamGate=CLOSED** — no safe replay owner yet. Stereo file **`46–61`**
remap to **`45`** on load.

**Mode 62/63 (optional, not deployed):** **`62`** = Mode 45 + `ReValidate:` AOB log at startup.
**`63`** = Mode 45 + seam gate log (`SameFrameSeamGateOpen=0`). Run `py scripts/offline-re-scan.py`
anytime without launching the game.

**Kill:** stereo **`45`** · delete `fovadd` for Mode 35 read-only · **`30`** + no fovadd for legacy.

### Session 2026-07-25 ~04:30 (RE map + scaffold — no headset)

**Shipped (build OK, not game-tested this session):**
- **`docs/RE_OFFSETS.md`** — RVA/AOB table, forbidden list, VsRet chain, cross-mod targets
- **`scripts/offline-re-scan.py`** — read-only PE scanner for local `GTAIV.exe`
- **`re_validate.cpp`** — ASI startup AOB validation (`ReValidate:` log lines)
- **Mode 62/63** enum + renderer path (Mode 45 family); default remains **45**
- **F7** restored to **8-step** leanGain preset (1.0…5.0)
- **ParseModeFile** remaps **46–61 → 45** with warning

**Headset check when back:** Confirm Mode **45** still smooth (no jump regression). Optional: write
**`62`** to stereo file → restart → log should show `ReValidate: OK …` for CopyMat/FovSite/phase
AOBs. Optional **`41`** for 1 min → paste `Mode41: CHAIN` rows if continuing same-frame RE.

**Still blocking perfect VR:** temporal L/R (one game tick between eyes); replay-thread draw owner
not hookable yet; independent VR cam deferred.

---

**As of (prior):** 2026-07-25 ~03:00 (Mode 45 LKG restored + F7 leanGain fix)
**Deployed now:** stereo **`45`**, **`ipd=3`**, scale **`100`** (leanGain **1.0**), **`fovadd=18`**. No
`worldsize` file. Source restored from git **`872ab11`** (Mode 45 head-owned camera — last known good).

**Hotkeys:** F9=SteamVR recenter (resets seated 6DoF baseline) · F6/F7/F8 stereo scale / world lean / IPD.

**F7 leanGain (fixed):** `eyeDelta = hmdDelta / leanGain`. **Higher leanGain = smaller world** when you
lean. F7 cycles **up**: **1.0 → 1.25 → 1.5 → 2.0 → 2.5 → 3.0 → 4.0 → 5.0** (8 steps). File
`gtaiv_dxvk_vr.scale` stores **leanGain×100** (100 = normal). Log:
`F7: world smaller — leanGain=X.XX (step N/8)`.

**Bug found:** Old code used **`eye += delta * worldScale`** (multiply). Higher scale = **more** movement =
**bigger** world — opposite of comments and user intent.

**Kill:** stereo **`45`** · delete `fovadd` for Mode 35 read-only · **`30`** + no fovadd for legacy pair-hold.

### Session 2026-07-25 ~03:00 (872ab11 restore + F7 divide fix)

**Restored from `872ab11`:** `cam_matrix`, `stereo_config`, `stereo_render`, `vr_display`, `hmd_look`,
`openvr_mono` — pure Mode 45 path (no Mode 46–61 experiments). F9-only recenter from 872ab11 (no F10 split).

**F7 fix:** Replaced multiply-with-pct with **divide-by-leanGain**. Eight preset steps; each F7 press
makes head-lean move the camera **less** in game space.

**Headset check:** Restart game. Confirm smooth Mode 45 (no bars, no jump). Press **F7** repeatedly —
log should show leanGain rising 1.0→1.25→… and world should feel **smaller** when you lean. Too big at
spawn → F7 toward 2.0–3.0. Reset lean origin → **F9**.

---

**As of (prior):** 2026-07-25 ~02:45 (HARD ROLLBACK — Mode 45 good baseline restored)

**User report:** Recent uncommitted patches destroyed performance and reintroduced jitter/jump.
F7 still made world **bigger** instead of smaller.

**Reverted (discarded uncommitted work, restored `src/asi/*` from git HEAD):**
- **`100/pct` WorldScale invert** — back to linear **`pct/100`**
- **F7 cycle down** (100→10) — back to **cycle up** (50→300, higher = smaller feel)
- **Spawn auto 6DoF baseline** at first cam apply — removed
- **Mode 45 soft Y floor** clamp — removed
- **F9 resetting 6DoF** (coupled with F10) — back to **F9 heading only**, **F10 = 6DoF only**
- **`worldsize` preset** file + Mode 61 renderer-only path — removed from build
- Default scale file reset to **`100`** (was 50 in bad patch)

**Still active (unchanged in HEAD):** Mode 44 RT lock, motion guard ON for Mode 45,
true-FOV `fovadd=18`, pair-hold temporal stereo, ped-eye CopyMat + HMD orient + late
head-owned CopyMat refresh.

**Headset check:** Restart game. Smooth again? Press **F7** — scale number should **rise**
(100→125→150…) and world should feel **smaller** when you lean. Underground spawn → **F10**
once. Still too big → keep pressing F7 toward 150/200/300.

---

**As of (prior):** 2026-07-25 ~01:30 (Mode 61 renderer + worldsize knob — camera frozen at 45)
**Deployed now:** stereo **`45`** (default kill) + **`worldsize=110`** (→ fovadd **20**, stereoscale **130**),
**`ipd=3`**, scale **`100`**, stereoscale file ignored when worldsize present. Mode **61** shipped in
ASI (renderer-only test — write **`61`** to stereo file). Modes **46** and **59** remap to **45**.
**Hotkeys:** F9=view recenter · F10=6DoF reset · F6/F7/F8 stereo scale/world/IPD (worldsize overrides
fovadd+stereoscale at load; F6 still live-tunes stereoscale).
**Kill:** stereo **`45`** primary · **`30`** + delete `fovadd` + delete `worldsize` · protected **`36`/`35`**.

### Session 2026-07-25 ~01:30 (Mode 61 renderer-soft + worldsize preset — shipped)

**Track A — same-frame (honest):** No new safe replay-thread hook. VsRet `0x2C73E` chain remains
observation-only (forbidden `0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`, `0x309D0`). **Mode 61**
is **not** same-frame stereo — it keeps **Mode 45 camera math** unchanged and improves the **renderer**:
RT lock + pair-hold + **AER `Submit_TextureWithPose`** + **soft motion guard** (8°/6 cm → keep distinct
L/R + AER; 15°/12 cm → mono) instead of Mode 45's aggressive **1.5°/2 cm** flatten.

**Track B — world size (visual):** New file **`gtaiv_dxvk_vr.worldsize`** (75–125, **100** = baseline
fovadd **18** + stereoscale **125**). Deployed **`110`** → fovadd **20**, stereoscale **130**. Does **not**
change F7 WorldScale (6DoF lean). FusionFix menu FOV stays **0**.

**Automated post-load freeze-test PASS (Mode 45, build `20260724-232550`):** `StereoMode: 45`,
`Mode44: RT lock … recreates=0`, continuous `StereoSubmit: L=1 R=1 mode=45` through `MonoSubmit #3600+`,
`CamMatrix … sep=3cm` (simple Mode-45 log), `Mode45: late head-owned … pedCoupled=0 forcedWalk=0`. No
ASI exception during unattended run.

**Headset check (Mode 61 optional):** write **`61`** to `gtaiv_dxvk_vr.stereo`, restart. Expect:
`StereoRender: mode 61 RENDERER-SOFT … SameFrame=0`, `AERPose: … submitWithPose=1`, fewer
`MOTION-GUARD mono pair` lines than Mode 45 on calm turns. Kill: **`45`**.

**Headset check (worldsize on Mode 45):** restart with `worldsize=110` — log
`Config: worldsize=110 -> fovadd=20 stereoscale=130`. World should feel slightly less “monitor gigantic”
without look-up warp. Too big → try **`115`**; too small/warpy → **`100`** or delete file.

---

**User report:** Extreme jumping and performance dips returned after Mode 46–60 camera experiments
(forced walk, split-view, VS translate fights, pedCoupled, quadratic scale, under-map clamps).

**Reverted for Mode 45 default path:**
- Removed Mode 59/60 **2.5 m lean cap** and **ped-eye anchor clamps** from `ApplyHmdToCam` (Mode 45
  never used them; they changed feel for all modes).
- Restored **F9/F10 split:** F9 = SteamVR zero + ped/veh heading only; F10 = 6DoF translation
  reset (emergency fix had coupled both on F9).
- Mode 45 logging back to simple `CamMatrix … sep=… eyeFwd=…` (no experimental flags in log).
- Config on disk: **`stereo=45`**, **`scale=100`**, **`fovadd=18`**, **`ipd=3`**.

**Still active on Mode 45 (unchanged — the user-loved stack):** Mode 44 RT lock + motion guard,
true-FOV `fovadd=18`, pair-hold temporal stereo, ped-eye CopyMat + HMD orient + linear WorldScale,
late head-owned CopyMat refresh at FOV site.

**Next work (NOT camera):** VR renderer (same-frame distinct eyes) + honest world-size levers
(engine FOV / stereoscale — F7 lean ≠ building size). No new cam ownership modes this session.

**DO NOT USE without explicit test:** stereo **46** (freeze), **59** (under-map), **54–58**
(forced walk / VS translate / split-view cam fights). Kill stays **`45`**.

---

**As of (prior):** 2026-07-25 ~00:15 (EMERGENCY ROLLBACK)

**User report:** Mode 59 + scale=1000 → camera moved way too fast, not attached to character;
F9 recenter placed view **under the map** (underside of Liberty City); game **froze**.

**Root cause:** Mode 59 shipped **quadratic** WorldScale: effective = (pct/100)². With
scale=**1000** → **100×** 6DoF lean. Combined with F9 recenter (yaw only, 6DoF baseline kept)
+ forced head-walk (not ped-coupled) → runaway camera origin detached from ped eye → under terrain.

**Fix shipped:**
- Config on disk: **`stereo=45`**, **`scale=100`** (restart game to load).
- Mode **59** parser remaps → **45** with log warning (same pattern as disabled Mode 46).
- WorldScale reverted to **linear** pct/100, **cap 20×**, F7/file max **500**.
- **Ped eye anchor** every frame: 6DoF lean capped 2.5 m; camera Y floor pedEye−1 m.
- **F9** now resets ped/veh heading **and** 6DoF translation baseline together.
- Mode **60 SAFE-VR** added (optional): Mode 45 motion-guard + Mode 58 polish, ped-coupled,
  NOT forced-walk. Primary kill remains **45**.

**DO NOT USE:** stereo **59** · scale **1000** · quadratic WorldScale builds.

---

**As of (prior):** 2026-07-24 ~23:55
**Previously deployed:** stereo **`59`** (Mode-59 SMOOTH-VR — **FAILED**, see above) + scale **`1000`**.

**One behavior change:** Mode **59** = restore **Mode 45/49 motion-guard** (fast mono on rapid
head turn) while keeping Mode **58** VR polish (pitch-stable, pre-capture, no mouse fight,
forced head-walk). Drops Mode **57/58** BotW **center-eye + VS translate split** — uses **full
HMD CopyMat + IPD** like the smooth modes. **WorldScale formula fixed:** effective =
**(pct/100)²** — F7 now cycles to **2000** (1000→100×, 2000→400× lean). Logs
`WorldScale6DoF: pct=… applied=…m` each ~300 frames.

**Deployed:** stereo=`59`, fovadd=`18`, ipd=`3`, scale=`1000`, stereoscale=`125`.

**Headset check:** head turn — smooth like 45/49 (guard may flatten briefly, no stutter fight)?
Lean forward 20 cm — log `applied=…m` should be large at scale=1000; world feel smaller?
Walk forward — goes where you look? Kill: **`58`**, **`57`**, **`45`**.

---

**As of (prior):** 2026-07-24 ~23:30
**Previously deployed:** stereo **`58`** (Mode-58 NATIVE-VR: Mode 57 unified full HMD CopyMat + VS translate
1.10× parallax + pitch-stable look-up + pre-capture refresh + no mouse-look fight; forced
`vr_move` head-walk) + **`fovadd=18`**, **`ipd=3`**, scale **`500`**, stereoscale **`125`**.
Mode **57**/**56**/**55**/**45** are kill baselines. Mode 46 stays disabled (post-load freeze).
**Hotkeys:** F9=view recenter · F10=6DoF reset · F6/F7/F8 stereo scale/world/IPD.
**Kill:** stereo **`57`**, **`56`**, **`55`**, **`45`**, **`54`**, **`51`**. Protected: **`36`/`35`**.

### Session 2026-07-24 ~23:30 (Mode 58 NATIVE-VR + tiny world scale — shipped)

**One behavior change:** Mode **58** = Mode 57 HMD-walk path, plus native VR polish:
- **Pitch-stable** look-up (Mode 48 fix — no snap/warp near vertical)
- **Pre-capture** CopyMat refresh (blocks late follow-cam overwrite before each eye)
- **No mouse-look** injection before cam arm (Mode 58 only — stops HMD/mouse fight)
- **VS translate 1.10×** (render parallax only — not IPD×WorldScale)
- Walk-where-you-look **retained** (forced `vr_move`)

**WorldScale (F7):** cycle extended to **50…1000** (adds 400, 500, 750, 1000). File accepts
**25–1000**. Rule unchanged: **HIGHER WorldScale = SMALLER world** (6DoF translation only; F8
IPD separate). Log on F7: `HIGHER = SMALLER world`.

**Deployed:** stereo=`58`, fovadd=`18`, ipd=`3`, scale=`500`, stereoscale=`125`.

**Headset check:** world feel smaller at scale=500? Press **F7** to cycle 750/1000 if still huge.
Look up — horizon stable (pitchStable)? Walk forward — goes where you look?
Log: `VrMove: Mode58 FORCED head-walk ON`, `Mode58: nativeVr=1 pitchStable=1 preCapture=1`,
`WorldScale: 5.00 (file) — F7: HIGHER = smaller world`. Kill: **`57`**, **`55`**, **`45`**.

---

**As of (prior):** 2026-07-24 ~23:00
**Previously deployed:** stereo **`57`** (Mode-57 HMD-WALK: unified full HMD CopyMat + VS translate parallax;
forced `vr_move` ped heading from HMD yaw; no split-view level-lock; no pedCoupled orbit) +
**`fovadd=18`**, **`ipd=3`**, scale **`175`**, stereoscale **`125`**. Mode **56**/**55**/**45** are kill
baselines. Mode 46 stays disabled (post-load freeze).
**Hotkeys:** F9=view recenter · F10=6DoF reset · F6/F7/F8 stereo scale/world/IPD.
**Kill:** stereo **`56`**, **`55`**, **`45`**, **`54`**, **`51`**. Protected: **`36`/`35`**.

### Session 2026-07-24 ~23:00 (Mode 57 HMD-WALK — shipped)

**One behavior change:** Mode **57** = Mode 55 BotW VS-translate parallax, but **drops Mode 56
split-view** (gameplay level-lock broke pitch). Unified full HMD orientation on CopyMat (ped eye
+ 6DoF). **Forces** `vr_move` ped heading from HMD yaw every frame (ignores `movemode=1`). No
pedCoupled orbit (Mode 49/55). Motion-guard OFF.

**Deployed:** build `20260724-225128`, stereo=`57`, fovadd=`18`, ipd=`3`, scale=`175`.

**Headset check:** look up — horizon tilts naturally? Walk forward — goes where you look?
Log: `VrMove: Mode57 FORCED head-walk ON`, `forcedWalk=1`, `Mode57: StereoAudit … forcedWalk=1`.
Kill: **`56`**, **`55`**, **`45`**.

---

**As of (prior):** 2026-07-24 ~22:33
**Previously deployed:** stereo **`55`** (Mode-55 BotW-parallax: center-eye CopyMat + VS translate-only
render path; no D3DTS_VIEW hijack) + **`fovadd=18`**, **`ipd=3`**, scale **`100`**, stereoscale
**`125`**. Mode **54** (full view replace + BeginScene push) superseded — broke controls.
Mode **50**/**49** remain kill baselines. Mode 46 stays disabled (post-load freeze).
**Hotkeys:** F9=view recenter · F10=6DoF reset · F6/F7/F8 stereo scale/world/IPD.
**Kill:** stereo **`50`**, **`49`**, **`45`**, **`54`**, **`51`**. Protected: **`36`/`35`**.

### Session 2026-07-24 ~22:33 (Mode 55 BotW-parallax + control fix — shipped)

**One behavior change:** Mode **55** = Mode 50 ped-coupled head-owned temporal stereo, but
CopyMat stays **center eye** (no IPD in gameplay cam — restores aim/look like 49/50). Real
parallax via replay-thread **SetVSConstF view translate only** (Mode 54 insight: CopyMat IPD
is fake for draws). Removed Mode-54 paths that broke controls: **no** BeginScene D3DTS_VIEW
push, **no** manager-cam IPD sync, **no** full view matrix replace.

**Deployed:** build `20260724-223346`, stereo=`55`, fovadd=`18`, ipd=`3`, scale=`100`.

**Headset check:** controls feel like 49/50 again? More 3D than 50 (VS translate)? F9/F10 OK?
Kill: **`50`** (CopyMat-only parallax) or **`45`** if regress.

---

**One behavior change (deployed):** Mode **51** = Mode 50 always-distinct temporal L/R +
capture-time `Submit_TextureWithPose` (Luke AER lesson). IPD bumped **`2` → `3` cm** for stronger
cam-matrix parallax (`CamMatrix ipd≈±0.016` vs ±0.009 at 2 cm).

**Also shipped (not deployed):** Mode **53** = Mode 51 + soft motion guard: **8°/6 cm** → keep
distinct L/R + AER (no flatten); **15°/12 cm** → full mono fallback only. Mode **52** unchanged
(swap-eyes test).

**Automated post-load freeze-test PASS (Mode 51, build `20260724-221207`):** `StereoMode: 51`,
`mode 51 STEREO-AER … ok=1 fovSite=1`, `StereoSep: 3 cm`, continuous
`StereoSubmit: L=1 R=1 mode=51` through `MonoSubmit #14400+`, `Mode50: StereoAudit pairs=6960
avgDiff=11037 zeroRate=1.0% sep=3cm scale=1.25 motionGuard=0`, `CamMatrix … ipd=(-0.0166,…)
sep=3cm pedCoupled=1`, `AERPose: eye=R submitWithPose=1 err=0`, `Mode44: RT lock … recreates=0`,
and **zero** `MOTION-GUARD mono pair` lines.

**Automated post-load freeze-test PASS (Mode 53, build `20260724-221516`):** `StereoMode: 53`,
`mode 53 STEREO-SOFT-GUARD … ok=1`, `Mode53: StereoAudit pairs=7080 avgDiff=6596 zeroRate=1.1%
softGuard=0 hardMono=0` (calm session — guard did not fire), continuous submits, no exception.

**Headset check:** compare depth to Mode 50 — ipd=3 should feel stronger near-field parallax.
AER should reduce turn jump without killing 3D. If jump still bad → try **`53`**. If depth
inverted → **`52`**. Kill: **`50`**, **`49`**, **`45`**.

### Session 2026-07-24 ~22:10 (Mode 50 stereo-always: motion-guard OFF — shipped + freeze-test PASS)

**One behavior change:** Mode 50 = Mode 49 visuals/stability, but **never** runs the Mode-40
motion-guard mono flatten. Temporal L/R pair-hold always promotes distinct eye captures when
`ipd>0`. Adds periodic `Mode50: StereoAudit` logging (avg L-vs-R canvas diff, zeroRate,
motionGuard=0).

**Also shipped (not deployed):** Mode **51** = Mode 50 + `Submit_TextureWithPose`; Mode **52** =
Mode 50 + swapped L/R submit (depth inversion test).

**Automated post-load freeze-test PASS:** build `20260724-220655` logged `StereoMode: 50`,
`StereoRender: mode 50 STEREO-ALWAYS … ok=1 fovSite=1`, continuous
`StereoSubmit: L=1 R=1 mode=50` through `MonoSubmit #3600+`, `Mode50: StereoAudit pairs=1560
avgDiff=13176 zeroRate=5.4% sep=2cm motionGuard=0`, `CamMatrix … ipd=(±0.009…) pedCoupled=1`,
`Mode44: RT lock … recreates=0`, and **zero** `MOTION-GUARD mono pair` lines. No ASI exception
or submit error during 2+ minute gameplay run.

**Headset check:** compare depth/presence to Mode 49 — near objects should show parallax again.
If depth feels inverted → try **`52`**. If turn jump worse than flatness → **`49`**. Optional:
**`51`** for AER reprojection comfort.

### Session 2026-07-24 ~22:00 (F9/F10 hotkey split: recenter vs 6DoF reset)

**One behavior change:** F9 and 6DoF translation reset were coupled; user wanted them split.
- **F9** — view recenter: SteamVR seated zero, mouse-look baseline, ped heading baseline (Mode 49
  body-turn coupling), vehicle yaw baseline. Does **not** zero head translation / lean origin.
- **F10** — 6DoF reset only: zeros the seated translation baseline (F10-relative lean/strafe anchor
  × WorldScale). Does **not** recenter yaw or SteamVR.

**Log on press:** `Recenter: F9 — SteamVR zero + ped/veh heading baseline (6DoF unchanged)` and
`SixDofReset: F10 — head translation origin zeroed`.

**Headset check:** seated, lean forward — press **F10** (world should snap lean to zero, facing
unchanged). Turn body — press **F9** (forward recenters, lean offset kept). Kill still **`45`**.

**Automated post-load freeze-test PASS:** build `20260724-215659` logged continuous
`StereoSubmit: L=1 R=1 mode=49`, `CamMatrix … pedCoupled=1`, `Mode49: late head-owned …
pitchStable=1`, through `CamMatrix: FP lock #96000+` with no ASI exception or submit error.

### Session 2026-07-24 ~21:54 (Mode 49 ped-coupled + pitch sign fix: shipped + freeze-test PASS)

**One behavior change:** Mode 49 = Mode 48 visuals/stability, but (1) **turns OFF** Mode 47/48
`leveledPitchFlip` (headset said flip=1 was still reversed — Mode 45 pitch sign restored), and (2)
**yaw-couples** to the live ped: camera heading = HMD heading + (pedHeading − pedHeading@F9), so
body turns carry the view while HMD yaw remains free look on top. Position stays ped-eye +
F10-relative 6DoF × WorldScale (Mode 45 anchor). Pre-capture CopyMat refresh retained.

**Automated post-load freeze-test PASS:** build `20260724-215101` logged `StereoMode: 49`,
`StereoRender: mode 49 PED-COUPLED HEAD-OWNED … ok=1 fovSite=1`, continuous
`StereoSubmit: L=1 R=1 mode=49` through `MonoSubmit #8940+`, `CamMatrix … leveledPitchFlip=0
pedCoupled=1`, `Mode49: late head-owned … leveledPitchFlip=0 pitchStable=1 pedCoupled=1`, and
`Mode44: RT lock … checks=8400 recreates=0`. No ASI exception, OpenVR submit error, or RT
recreation during the 2+ minute run.

**Headset check:** look up/down — correct sign (not reversed)? Walk/turn — view follows body?
Lean/strafe with scale=100 still OK? Kill: **`45`** (uncoupled baseline), **`48`** (flip+no couple),
**`47`** (flip only).

### Session 2026-07-24 ~21:18 (Mode 48 pitch-stable head-owned: shipped + freeze-test PASS)

**One behavior change:** Mode 48 extends Mode 47 on the safe leveled path. When HMD forward
is near vertical (`fwdHoriz < 0.26`), the world-up cross product degenerates and previously
snapped to `rx=(1,0,0)` — the look-up warp. Mode 48 keeps the last stable horizontal right
vector and re-orthogonalizes up from the current forward (no Mode-46 full HMD-basis write).
It also calls `RefreshLiveCamForStereoEye()` immediately before each temporal eye capture so
a late follow-cam overwrite cannot stale the backbuffer view.

**Automated post-load freeze-test PASS:** build `20260724-211554` logged `StereoMode: 48`,
`StereoRender: mode 48 HEAD-OWNED PITCH-STABLE … ok=1 fovSite=1`, continuous
`StereoSubmit: L=1 R=1 mode=48` through `MonoSubmit #15180+`, `Mode48: late head-owned …
pitchStable=1`, `leveledPitchFlip=1`, and `Mode44: RT lock … checks=14400 recreates=0`. No
ASI exception, OpenVR submit error, or RT recreation during the 2+ minute run. The startup
`CreateDevice FAILED hr=0x8876086c` is still the rejected preliminary device attempt.

**Headset check:** look up at sky/buildings — warp reduced? Lean/strafe with scale=100 still
OK? Kill: write **`47`** (Mode-47 pitch flip only) or **`45`** if anything regresses.

### Session 2026-07-24 ~21:10 (6DoF WorldScale correction: `50` → `100`, freeze-test PASS)

**One behavior change:** kept the proven Mode 47 binary and all stereo/FOV/RT settings intact,
but corrected `gtaiv_dxvk_vr.scale` from `50` to **`100`**. The implementation maps the
OpenVR absolute tracking translation directly through the established Ovr→GTA world-axis mapping,
then multiplies only that translation by `WorldScale`; it does not rotate translation with the
head and does not multiply eye separation. Thus `100` gives twice the prior physical lean/strafe
distance and is expected to make the previously huge-feeling world smaller. It is a headset
comfort test, not a substitute for real same-frame stereo.

**Automated post-load freeze-test PASS:** the fresh Mode-47 run logged
`WorldScale: 1.00 (file)`, continuous paired `StereoSubmit: L=1 R=1 mode=47` through
`MonoSubmit #15600`, repeated `leveledPitchFlip=1` late refreshes, and
`Mode44: RT lock ... checks=15000 recreates=0`. The motion guard also fired safely during
HMD movement (`directSubmit=1`). No ASI exception, OpenVR submit error, or RT recreation was
logged during the more-than-two-minute run. Physical validation still needed: F9 while seated,
then lean left/right, forward/back, and up/down; if head motion becomes too strong, restore
`gtaiv_dxvk_vr.scale` to **`50`** and restart.

### Session 2026-07-24 ~20:52 (Mode 47 leveled-pitch flip: deployed and freeze-test PASS)

**Actual deployed Mode 47 behavior:** build `20260724-205222` reverses only the
vertical component of the HMD forward vector before Mode 45's existing world-up right/up
reconstruction. It is active for Mode 47+ and logs `leveledPitchFlip=1`; Mode 45 remains
unchanged. The safe late refresh retains `fullHmdBasis=0`, so it does **not** revive Mode 46's
unsafe OpenVR right/up full-basis write. RT lock, true-FOV `fovadd=18`, temporal motion guard,
normal IPD, and no replay/VS/collision write remain unchanged.

**Automated post-load freeze-test PASS:** after more than two minutes, the live
`20260724-205222` log reached `MonoSubmit #12960` / `StereoSubmit: L=1 R=1 mode=47`, with
`CamMatrix ... fullHmdBasis=0 leveledPitchFlip=1`,
`Mode47 ... fullBasis=0; leveledPitchFlip=1`, and
`Mode44: RT lock 1536x1536 checks=12600 recreates=0`. No post-load stall, exception, RT
recreation, or OpenVR submit error was observed. The startup `CreateDevice FAILED
hr=0x8876086c` is followed immediately by `CreateDevice OK` and is a rejected preliminary
device attempt, not a runtime failure. Headset direction/comfort still needs the user's
physical up/down check; write **`45`** to restore the committed safe baseline if it feels wrong.
Mode 47 does not enable physical roll because a full roll basis is the Mode-46 freeze path.

### Session 2026-07-24 ~20:30 (Mode 46 full HMD-basis / roll test: FAILED and disabled)

**Mode 46 result:** Mode 45 was preserved as the kill baseline. Mode 46 changed only the orientation
part of the existing late post-CCam CopyMat refresh: instead of reconstructing camera right/up from
HMD forward plus GTA world-up (which levels out roll), it maps OpenVR's HMD right, up, and forward
columns through the established Ovr→GTA basis, then applies controller/vehicle yaw to all three
axes together. Despite being mathematically rigid, that non-level camera matrix freezes GTA IV
after the loading screen. The most likely seam is a RAGE follow-camera/render consumer that assumes
a world-up-derived right/up pair; a blocking/infinite loop was not established from the ASI log.
Do not retry the direct full-basis write without an isolated post-load freeze test.

**Recovery shipped:** `stereo=45` is restored and launched with `fovadd=18`. The config parser now
maps a requested `46` to Mode 45 and logs `requested 46 is DISABLED`; the deployment file itself
must stay `45`. Mode 45 log proof after recovery must show continuing
`StereoSubmit: L=1 R=1 mode=45` after load.

**Do not test Mode 46.** Test Mode 45 only: F9 while seated, then look/lean normally. If it ever
regresses, write **`44`**, **`37`**, or **`30`** + delete `fovadd`; Mode 45 is the primary kill.

### Session 2026-07-24 ~20:25 (Mode 45 head-owned view spike: shipped)

**Shipped Mode 45:** Mode 44 is intact, with one added call at the verified FusionFix-style CCam
FOV recompute site: after the game camera process completes, it re-applies the exact existing safe
CopyMat calculation (ped eye + F9-relative HMD translation × WorldScale, HMD orientation, eyefwd,
camoff, and normal IPD). It does not layer a second camera/VS translation; it writes the same tracked
CopyMat matrix later in the game thread. **Deployed + launch-log verified**
(`ASI_BUILD_ID 20260724-201131`): `mode 45 HEAD-OWNED CAM SPIKE ... ok=1 fovSite=1`,
`Mode45: late head-owned CopyMat refresh #1`, `FovSite ... 45.000 -> 63.000`, paired
`StereoSubmit: L=1 R=1 mode=45`, and the Mode-44 RT counter stayed `recreates=0`.

**Honest risk:** gameplay camera collision remains authoritative, so a wall can still constrain the
body anchor. Because this is render-only, collision, sound, AI/aim, weapons/HUD, shadows, and culling
can disagree with a late head pose. If any snap, wall issue, bad audio/aim, or crash occurs, write
**`44`** (exact Mode 44 visual path without late refresh); broader kills: **`37`** or **`30`** +
delete `fovadd`. No unattended build can establish headset comfort.

### Session 2026-07-24 ~20:05 (Mode 44 RT lock: shipped)

**Shipped Mode 44:** Mode 43's visual behavior is unchanged: the bars-gone true-FOV canvas,
Mode-37 temporal pair-hold, and rapid-motion direct-submit mono guard stay enabled. The one
behavior change is RT/publish stability. Once the four 1536×1536 submit+hold textures exist on
the same device, Mode 44 keeps them instead of considering a later requested canvas size; only a
lost/replaced resource can allocate again. Its true-FOV publisher now requires a 5% tangent
change rather than 2.5%, filtering ordinary CCam jitter while retaining meaningful camera/zoom
changes. It periodically reports `Mode44: RT lock ... recreates=...`.

**Deployed + launch-log verified** (`ASI_BUILD_ID 20260724-200503`): `StereoMode: 44`,
`mode 44 RT-LOCK ... ok=1 fovSite=1`, `eye RTs 1536x1536 OK`, and
`Mode44: RT lock 1536x1536 initialized recreates=0`; paired
`StereoSubmit: L=1 R=1 mode=44` followed. The automatic launch did not include a headset stress
test, so a sustained live session must still confirm the periodic counter remains `recreates=0`
and assess actual FPS. **Kill:** `43`, `40`, `37`, or `30` + delete `fovadd`.

### Session 2026-07-24 ~20:00 (Mode 43 fast motion guard: shipped)

**Shipped Mode 43:** the guard trigger/threshold and submitted visual result are Mode 40's exact
temporary current-frame mono pair; the sole behavior change is its copy route. Mode 40 first copies
current R to `holdR`, then re-canvases both hold textures and promotes both to submit textures
(five full canvas copies in the guarded R epoch). Mode 43 keeps the required `holdR` pose/cadence
copy but re-canvases directly to the submit L/R textures and skips promotion (three copies).
Calm temporal pair-hold, 1536×1536 locked true-FOV canvases, FOV latch, and `fovadd=18` remain
unchanged. It cannot improve normal FOV render cost, cannot create distinct eyes, and needs a
rapid HMD-turn headset check for its fast path to fire.

**Deployed + launch-log verified** (`ASI_BUILD_ID 20260724-200006`): `StereoMode: 43`,
`mode 43 FAST MOTION-GUARD ... 3 copies ... 5`, `eye RTs 1536x1536 OK`, locked true-FOV canvas,
and repeated `StereoSubmit: L=1 R=1 mode=43`. No guard firing was logged while unattended, so
`directSubmit=1` is not yet live-motion proven. **Kill:** `40`, `37`, or `30` + delete `fovadd`.

### Session 2026-07-24 ~20:15 (Mode 41 replay-chain probe: shipped)

**Why no new dual pass:** same-frame Phase execution is stable, but its view constants are already
baked; the actual `SetVSConstF` uploads happen on a separate replay thread. The known replay starts
are forbidden or disproven (`0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`), so blindly calling another
candidate would not be a defensible true-VR test.

**Shipped Mode 41:** the headset behavior is exactly Mode 40's Mode-37 true-FOV, pair-held stereo plus
rapid-motion temporary mono guard. On the live `VsRet=0x2C73E` replay-thread path only, Mode 41 retains
each valid stack return **with its stack slot**, statically recovers a preceding direct/indirect CALL
when possible, resolves a candidate function start/ABI for logging, and ranks the chain by EndScene
epochs. It never MinHooks a game function, replays draws, changes the camera, or claims distinct eyes.
Expected log proof is `Mode41: replay-chain ... SameFrame=0 distinctEyes=0` followed by
`Mode41: CHAIN ... hook=NO`.

**Deployed + live-log verified** (`ASI_BUILD_ID 20260724-194401`): `StereoMode: 41`,
`StereoSubmit: L=1 R=1 mode=41`, `Mode41: replay-chain epoch=630 VsRetHits=142077 ...
SameFrame=0 distinctEyes=0`, and `hook=NO` chain rows. The original stack facts were confirmed
without a new crash: slot 10 `0x22187 → E8@0x22182 → 0x2C6A0`; slot 15 `0x37C01 → 0x37BD0`;
slot 22 `0x37520 → 0x372B0` (stackarg); and slot 27 `0x32BD7 → 0x32A40` (stackarg).
New high-frequency nodes include `0x30D13 → 0x309D0` (thiscall-56) but have no statically recoverable
CALL at that return, while `0x3199A/0x319A4` do resolve direct calls but their enclosing start is
unaligned. Neither is safe to hook or replay yet.

**Next decision from the log:** probe only a chain node after its actual call ABI and object lifetime
are established—currently `0x309D0` is observationally interesting but unproven, and `0x30720`
is not an approved target. Keep Mode 40/41 as the safe interim; do not re-arm Mode 34 dual.

### Session 2026-07-24 ~19:50 (Mode 42 indirect-call decode: shipped)

**Shipped Mode 42:** Mode 40's safe true-FOV pair-hold/motion guard plus a read-only decoder for
x86 `FF /2` indirect CALLs that Mode 41 could not identify. No game function is hooked or called,
no camera/view/RT behavior changes, and it explicitly logs `SameFrame=0 distinctEyes=0`.

**Deployed + live-log verified** (`ASI_BUILD_ID 20260724-195512`): `StereoMode: 42`,
`StereoSubmit: L=1 R=1 mode=42`, `Mode42: OWNER-EDGE ret=0x30D13 enclosing=0x309D0
abi=thiscall-56 call=FF/2@0x30D0D modrm=0x90 len=6 ... hook=NO replay=NO`.
`modrm=0x90` proves the call is a six-byte **indirect** `call [eax+disp32]`, not a static
call to a safely reusable function. That explains why Mode 41 could not resolve an owner caller.
The initial Mode-42 artifact omitted modes 41/42 from the stereo-submit gate; the deployed
`195512` correction restores the proven paired OpenVR submit without changing the probe.

**Decision:** no Mode-43 replay hook yet. The required next seam evidence is the live `eax` object and
the displacement/vtable target at that exact call, gathered without calling `0x309D0`. Mode 42 stays
safe and is not true VR stereo.

### Session 2026-07-24 ~19:38 (Mode 40 motion guard: shipped)

**Same-frame investigation result:** no new safe replay seam exists in the present mapping. Real draws /
`SetVSConstF` execute on the replay thread (`VsRet=0x2C73E`); `BuildRootA`/`ExecRoot` are game-thread
paths. The earlier same-frame Phase dual is stable but camera/view constants are baked before its second
pass; the replay hook and false-prologue options are forbidden/crash-prone (`0x2C6AC`, `0x37BD0`,
`0x1BF010`, and Mode-34's `0x4DDAD0`). Therefore Mode 40 does **not** claim a true stereo pass.

**Shipped Mode 40:** Mode 37's locked 1536 true-FOV canvas and pair-held stereo remain unchanged while
still. When the L/R capture poses differ by at least **1.5°** or **2 cm**, Mode 40 rebuilds both canvases
from the current R backbuffer and promotes them together. This gives both submitted eyes one game/render
epoch for the turn, at the deliberate cost of temporary mono depth. No new replay/render hook, no
CCam+VS stack, no CanvasZoom, and no IPD×WorldScale.

**Deployed + live-log verified** (`ASI_BUILD_ID 20260724-193720`): `StereoMode: 40`,
`StereoRender: mode 40 MOTION-GUARD … ok=1 fovSite=1`, `StereoSubmit: L=1 R=1 mode=40`, and live
guard firings such as `Mode40: MOTION-GUARD mono pair #1 SameFrame: L+R from current R frame
rot=2.05deg move=0.24cm`. The first deployment briefly classified Mode 40 as non-temporal and redundantly
dual-built lists; it was immediately corrected and redeployed in the verified build above.

### Session 2026-07-24 ~19:45 (reject Mode 39 / restore Mode 37)

**User feedback:** black bars returned in Mode 39; performance remained poor; jumping remained strong.
The existing live log proves the configuration: `StereoMode: 39`, `StereoSubmit ... mode=39`, and
`FovSite ... add=12` with requested `fovadd=18`. Mode 39 therefore reduced the true engine FOV
that removed the bars, while leaving the temporal pair-hold schedule intact.

**Deployed configuration:** changed `gtaiv_dxvk_vr.stereo` from **39** to **37**; kept
`fovadd=18`, IPD/scale files untouched, CanvasZoom off, and no forbidden hooks. Mode 37 uses the
locked 1536 canvas and pair-latched true FOV, so it is the honest no-bars baseline. **Fresh log
proof** (`ASI_BUILD_ID 20260724-192011`): `StereoMode: 37`, Mode 37 `ok=1 fovSite=1`,
`Config: fovadd=18`, 1536×1536 submit+hold RTs, promoted L/R pairs, and
`StereoSubmit ... mode=37`. Steam launched more slowly than the restart script's process-wait
window, but the new ASI session did start and logged cleanly.

**Research conclusion:** L4D2VR/HL2VR own an engine `RenderView` and render both eye views within
one simulation tick. UEVR similarly treats alternating/AFR as a last resort because game time
advances between eyes. Their transferable requirements (one pose epoch, coherent per-eye
view/projection/texture geometry, stable RTs) are already applied where Rage permits. The missing
Rage same-frame render seam—not another pose-submit/FOV cap—is the root work before an independent
VR camera.

### Session 2026-07-24 ~19:30 (Mode 39 low-motion FOV comfort)

**User:** Mode 38 logged `Submit_TextureWithPose` success (`err=0`) but still jumped severely.
**Root-cause decision:** do **not** repeat another compositor reprojection variation: successful Mode 38 proves the API path runs, while temporal L/R remains one rendered frame apart. Mode 36/37's wider true FOV also correlated with lower FPS and stronger jump.
**Shipped Mode 39:** Mode 37's fused, pair-held, true-FOV canvas path with the same locked 1536 canvas, but the existing `fovadd` is capped to **12°** (file can remain `18`; log records requested/effective cap). No AER pose submit, no CanvasZoom, no IPD×WorldScale, no new render hooks.
**Expectation:** a safer comfort/FPS experiment, not a temporal-stereo cure. If no clear improvement, return to Mode 30 and stop spending experiments on temporal reprojection; same-frame remains the only root fix.
**Deploy verified (`ASI_BUILD_ID 20260724-192011`):** `StereoMode: 39`; `StereoRender: mode 39 LOW-MOTION COMFORT … ok=1 fovSite=1`; `Mode39: fovadd requested=18 capped=12`; `FovSite` writes `45.152 → 57.152 (add=12)`; pair promotion and `StereoSubmit: L=1 R=1 mode=39` are live.
**Kill:** write `38` to restore AER pose/full FOV, `37` for full FOV without pose, or `30` then delete `gtaiv_dxvk_vr.fovadd` for the smoother protected baseline.

### Session 2026-07-24 ~19:10 (Mode 38 AER pose submit)

**User:** Mode 37 still huge/monitor, jump, bad FPS; F7 doesn’t feel like being in the world; read Luke AER v2 Patreon.  
**Takeaway:** AER v2 = optical-flow intermediates (skip). Transferable = stamp each eye with capture-time pose (`Submit_TextureWithPose`, OpenVR #1253).  
**Shipped Mode 38:** Mode 37 path + pose cache on L/R capture + pose promote with pair-hold + `Submit_TextureWithPose`. Fallback to `Submit_Default` if pose missing/err.  
**Deploy verified (`ASI_BUILD_ID 20260724-191333`):** `StereoMode: 38`, `mode 38 AER-POSE SUBMIT … ok=1`, pair promote `poseL=1 poseR=1`, `AERPose: eye=L/R submitWithPose=1 err=0`, `StereoSubmit … mode=38`. Kill **37** / **30**.  

### Session 2026-07-24 ~18:45–19:00 (Mode 37 comfort — Mode36 bars-gone + Mode30 smooth)

**User feedback (Mode 36):** bars completely gone (keep); still screen-on-face; **FPS much worse**; **jump/jitter very strong** (Mode 30 was smooth); world **too close/huge**. Continue.  
**Diagnose:** Mode 36 pair-hold path was intact. Jump/FPS from (1) fovadd=22 wide engine FOV + temporal edge disparity, (2) FOV wobble recreating 4 eye RTs every few frames, (3) scale=50 = huge 6DoF feel.  
**Shipped Mode 37:** same true-FOV canvas + Mode30 pair-hold; **fovadd=18**; canvas **maxDim=1536 LOCKED**; pair-latched rect tangents; publish gate ~2.5%; **WorldScale→100** (one size lever; IPD untouched). No CanvasZoom / no IPD×scale / no forbidden hooks.  
**Deferred:** independent VR cam; Mode 34 dual.

### Session 2026-07-24 ~18:25–18:45 (Mode 36 true-canvas FOV — fill HMD / kill bars)

**User feedback (Mode 35):** better presence / less warp, still screen-on-face + black bars; protect as baseline; push harder.  
**Root cause:** engine `fovadd` wrote CCam+0x60, but canvas still used stale `D3DTS_PROJECTION` tangents (Rage ignores that transform — Mode 15 dead) → bars stayed.  
**Shipped Mode 36:** Mode 35 pair-hold + FOV site + `PublishGameFovFromCCamDegrees` (Mode 16 factor 58.7/45) → canvas/rect track TRUE FOV. No CanvasZoom.  
**Also fixed:** `ParseModeFile` capped at 35 (file `36` became mode 3!); FOV ADD idempotent (no 67→89 compound); ADD only when base FOV ≤55° (skip cutscene/menu spikes).  
**Deployed:** stereo=`36`, fovadd=`22` (toward G2 ~94°; vertical fill ~90%). Mode **35** unchanged as kill/protect.  
**Headset check:** smaller/no bars? Still fused? No look-warp? If bad → stereo **35** or **30**. Try fovadd **18–25** if needed.

**Deferred:** independent VR cam; Mode 34 dual; soft TextureBounds fill; theater quad.

### Session 2026-07-24 ~18:10–18:25 (Mode 35 FOV recompute — research + spike)

**User ask:** feel inside the game with real 6DoF; Mode 30 fusion OK but warping + screen-on-face.  
**Research:** Luke Ross / Halo / L4D2 / UEVR / FusionFix — kill monitor-on-face with **true engine FOV**, not canvas zoom / theater. FusionFix `fixes.ixx` = cam process CALL then `*(CCam+0x60) += n*5` = our Mode 17 recompute site.  
**Shipped Mode 35:** Mode 30 pair-hold + chain-hook that CALL; `gtaiv_dxvk_vr.fovadd` ADD degrees (deployed **15**). Additive preserves aim zoom.  
**Log proof:** 45→60 stable; thousands of FovSite calls; submits mode=35; game stayed up.  
**Headset check:** less black-bar / monitor feel? No look-warp? If warp/bad → kill to 30 + delete fovadd. Keep FusionFix menu FOV at **0**.

**Deferred:** independent VR cam vs wall collision; Mode 34 dual; theater quad.

### Session 2026-07-24 ~17:50–18:05 (kill canvas zoom + research)

**Headset feedback:** `zoom=85` claimed-FOV warp — every head move warps environment (same class as FusionFix FOV look-up warp).  
**Shipped:**
- Canvas zoom **forced OFF** in `vr_display.cpp` (always true FOV; file ignored). Game `zoom=100`.
- Mode **30** + **`ipd=1`** redeploy (preserve scale/stereoscale/eyefwd/pedhide).
- Read-only `VsRetStatic:` PE scan E8→`0x2C73E` on Mode 30 arm — live **`callSites=0`** (indirect callers only; dual stays off).
- Docs: REFERENCES (OpenVR/Meta/parallel proj/Luke Ross), INSPIRATION_NOTES (R.E.A.L. pack), PLAN_NEXT (deferred independent cam).

**Opinion on independent VR cam:** **Agree — defer.** Game cam collision/pitch clamps fight HMD look near walls; Luke Ross uses pitch overrides + render-time view fixes. Invasive; not this session.

### Session 2026-07-24 ~17:30–17:40 (canvas zoom — REJECTED)

**Problem:** Mode 30 felt **too zoomed in** (world size / window feel). Not a 6DoF WorldScale issue (`scale=50` = less head-move only).

**Lever tried:** soft **`gtaiv_dxvk_vr.zoom`** on Mode 14/30 angle-correct canvas only.
- Multiplied claimed game half-tangents by `100/zoom%` inside `GetGameFovTangents`.
- Deployed **`zoom=85`** → warp on head move → **KILLED** next session.

**Preserved after kill:** stereo=30, ipd=1, scale=50, stereoscale=125, eyefwd=20, pedhide=1.

### Session 2026-07-24 ~17:15–17:25 (consolidate: Mode34 dual dead → Mode30)

**Live log proof (buildId `20260724-171146`, stereo was 34):**
- `Mode34: DUAL armed fnRva=0x4DDAD0 avgEntries=2.00`
- Thousands of `Mode34: SAME-FRAME … vsPatch=0 vsCallsR=0 sep=7cm fnRva=0x4DDAD0`
- StereoDiff gate did **not** fall back (canvas noise ≥200). Half-armed dual = same-cam, no 3D lever.

**Shipped harden:**
- Forbid `0x4DDAD0`. COUNT may still probe; **never arm Dual**. COUNT-ok → write stereo=**30**.
- Dual safety net: `vsPatch=0` after dualN≥5 → fallback (if old ASI left Dual on).
- Deployed knobs: stereo=**30**, ipd=**1**, pedhide=**1**, eyefwd=**42**, scale/stereoscale preserved.
- F8 prime + early `ReloadIpdScale` (before CamMatrix arm) — stops sticky-F8 overwriting ipd file (6→7).

### Session 2026-07-24 ~16:55–17:10 (Inspiration FP + PedHide)

**Inspiration read** (`inspiration/firstperson mod/`):
- ini: `FPX/FPY/FPZ=0`, `HMD=1`, FOV=`111` (not adopted — look-up warp), phone/sens ignored
- ASI: ScriptHook `NativeThread` + `SET_DRAW_PLAYER_COMPONENT` for head/hair hide

**Shipped:**
- **PedHide ON** via CE `SetDrawPlayerComponent` helper (unique AOB) — same effect as Inspiration, **no** EndScene natives / ScriptHook. Components 0/7/9/10.
- Optional `gtaiv_dxvk_vr.camoff` = `x y z` cm (FPX/FPY/FPZ style). Absent = 0.
- Kept Mode **30** + `eyefwd=42`. Do not touch FusionFix FOV.

**Log proof (buildId `20260724-170626`):**
- `Config: PedHide=ON (gtaiv_dxvk_vr.pedhide)`
- `PedHide: SetDrawPlayerComponent @ … (Inspiration-style, NO natives/ScriptHook)`
- `PedHide: head/hair/teeth/face off via SetDraw# 1 ok=4 dead=0` … later `ok=404 dead=0`
- `StereoSubmit: L=1 R=1 mode=30` + `eyeFwd=42cm` — game stayed up

**Headset check for user:** hair/face gone in FP? If yes, try `eyefwd` **12–20** (less turn-swing). If crash/weird → `pedhide=0`.

### Session 2026-07-24 ~16:25–16:45 (comfort + Mode 34)

**Shipped for headset:**
- F8 floor **1 cm**; file still allows 0–500; warn if file=0.
- `eyefwd=42` (default/file) — cam past skull/hair. `PedHide: OFF` (natives from EndScene/CopyMat crash CE).
- Mode **30** + `sep=1cm` + `eyeFwd=42cm` live (buildId `20260724-164438`).

Log quotes (playable):
- `StereoSep: 1 cm (file gtaiv_dxvk_vr.ipd) — F8 cycles 1,2,3,4,6,7,10`
- `Config: eyeForward=42 cm … PedHide: OFF …`
- `CamMatrix: FP lock #1 … sep=1cm eyeFwd=42cm`
- `StereoSubmit: L=1 R=1 mode=30`

**Mode 34 RESULT (buildId `20260724-164125`):**  
Goal: when HookSetVSConstF ret==`0x2C73E`, resolve stack → **function starts**, CC-pad thiscall0, count ~1×/frame, dual+pair-hold. Fail → stereo=30.  
- EndScene brace bug (Mode34 nested under Mode33) fixed mid-session.
- `Mode34: DISCOVER armed … rareAgg=24 sampleHits=1392 vsRetHits=276`
- Rare starts with avg≈1.0 e.g. `0x52E7C0`, `0x5303D0`, `0x4DDAD0`
- `Mode34: COUNT exeRva=0x1BF010` (thiscall-frame) → **process hard-kill** (no SEH FALLBACK write)
- Hardened after: forbid `0x1BF010`, reject `thiscall-frame`, prefer avg∈[0.8,2.5] closest to 1.0
- Dual **not armed**. Next Mode34 try only with hardened filter; kill → **30/26**.

### “No 3D” on Mode 30 — ROOT CAUSE (2026-07-24 ~16:00)

Pair-hold was **not** broken: L/R eye flip + `StereoPairHold: promoted pair` + `StereoSubmit L=1 R=1 mode=30` all healthy.  
Live log had **`sep=0cm`** because `gtaiv_dxvk_vr.ipd` = **`0`** (F8 preset included 0).  
CamMatrix with ipd=0 → `ipd=(±0.000…)` → flat image, comfort intact.  
**Fix:** restored IPD; F8 presets no longer include 0 (now start at **1**). Log warn if file=0.

### CE vs version downgrade (verdict)

**Stay on CE.** Classic/1.0.7–1.0.8 + VorpX/C06alt FP offsets would not give us a replay-thread draw walker: those mods are cam/FP/ScriptHook (and often SBS cinema), not Rage dual-draw. Downgrade would invalidate FusionFix + all CE AOBs/RVAs we already mapped (Mode 18–26), force a parallel glue rewrite, and still leave the same “find the VsRet walker” problem on a different binary. Singleplayer/offline OK, but not worth a dedicated spike unless CE same-frame is abandoned. **Full steam ahead on CE.**

---

## Architecture (proven)

```
GTAIV.exe
  → d3d9.dll          = Stock DXVK 3.0.2 x32
  → FusionFix         = ASI loader + CE fixes (Graphics API = DX9 / our d3d9)
  → gtaiv_dxvk_vr.asi + openvr_api.dll
       → EndScene: WaitGetPoses → Mono-Submit L+R (same texture)
       → CopyMat hooks: FP cam on ped + HMD orientation + 6DoF (F9)
       → vr_move: walk in look direction + right-stick yaw (inverted sign fixed)
  → SteamVR → HP Reverb G2
```

| Path | Role |
|------|------|
| Game folder | `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV` |
| Log | `...\GTAIV\gtaiv_dxvk_vr.log` |
| Stereo kill switch | `...\GTAIV\gtaiv_dxvk_vr.stereo` → **`30`** playable / **`26`** baseline / **`0`** mono |
| IPD / scale / stereoscale | `gtaiv_dxvk_vr.ipd` / `.scale` / `.stereoscale` — preserved across mode probes; **ipd ≥1 for 3D** (F8: 1,2,3,4,6,7,10) |
| Eye-forward | `gtaiv_dxvk_vr.eyefwd` — default **42** cm; live **20** with PedHide |
| PedHide | `gtaiv_dxvk_vr.pedhide` — **`1`** ON (Inspiration SetDraw) / **`0`** OFF |
| camoff | `gtaiv_dxvk_vr.camoff` — optional `x y z` cm (Inspiration FPX/FPY/FPZ) |
| Canvas zoom | **DISABLED** (warp) — file ignored; was `gtaiv_dxvk_vr.zoom` |
| Build | `.\scripts\build-asi.ps1` → `out-asi\gtaiv_dxvk_vr.asi` |
| Deploy + start | `.\scripts\build-deploy-run.ps1` (Build→Kill→Deploy→Steam start→close dashboard) |
| Deploy only | `.\scripts\deploy-asi.ps1 -GameDir "<GTAIV>"` (`-Launch` starts the game) |

**Currently deployed:** stereo=**`30`**, ipd=**`1`**, eyefwd=**`20`**, pedhide=**`1`**, zoom=**OFF**.  
**Mode 30 (2026-07-24):** PAIR-HOLD = Mode 14/26 CCam temporal, but submit textures  
update **only when a complete L→R pair is ready** (both eyes promote together).  
User: jumps **definitely less**, barely noticeable — keep as playable. With ipd≥1: try fusion.  
Log: `StereoPairHold: promoted pair #N`, `StereoSubmit … mode=30`, `CanvasZoom: DISABLED`, `sep=1cm`, `eyeFwd=20cm`.

**Mode 34 RESULT (2026-07-24) — dual armed then DEAD for parallax:**  
VsRet(`0x2C73E`) stack → fn starts; DISCOVER armed; COUNT `0x4DDAD0` avg=2.0 → DUAL armed;  
live `vsPatch=0` / `vsCallsR=0` for 40k+ frames (no parallax). Earlier COUNT `0x1BF010` hard-kill.  
**Hardened:** forbid `0x4DDAD0` + `0x1BF010`; **never arm Dual** (COUNT-ok → stereo=30). Kill → **30** or **26**.

**Mode 33 RESULT (2026-07-24) — wait worked, dual not armed:**  
Goal: WAIT until live VsParent samples (Mode32 Soft was empty), then only CC-pad  
zero-arg thiscall → count → dual+VS. Never `0x2C6AC`/`0x37BD0`/`0x32A40`/`0x372B0`.  
Fail → write stereo=30.

Log quotes (buildId `20260724-161039`):
- `StereoSep: 6 cm (file …)` / `mode 33 … ok=1`
- `Mode33: still waiting … es=60 rareAgg=0` (correct — no early seed)
- `Mode33: VsParent sample#1 slot=10 exeRva=0x22187` (+ `0x37520`/`0x32BD7`/`0x4D901B`)
- `Mode33: DISCOVER armed (live VsParent appeared; rareAgg=4 sampleHits=8; no early seed)`
- Mid bytes prove **epilogues**, not walkers: `0x22187=5F C2 10 00` (pop/ret 0x10),  
  `0x32BD7→0x32A40` REJECT forbidden stackarg, `0x37520→0x372B0` REJECT forbidden
- `Mode33: COUNT exeRva=0x4D8F10` → `avgEntries=0.31` REJECT
- `Mode33: FALLBACK pair-hold — wrote stereo=30`
- After: `StereoSubmit: L=1 R=1 mode=30` (alive). F8 no longer cycles to 0.

**Hard finding:** VsParent slots 10–32 are **return addresses inside callees** (epilogues /  
mid-instr), not ~1×/frame thiscall0 draw walkers. Next same-frame spike must find the  
**caller** that drives VsRet ~1×/frame (static E8 / deeper stack), not re-hook these mids.

**Mode 32 RESULT (earlier) — safe fallback, dual not armed:**  
Soft ran before VsRet traffic → empty hist → early seed of stackarg. Mode 33 fixed the wait.

**Next RVA / ABI (do NOT blind-hook):**

| RVA | ABI | Notes |
|-----|-----|--------|
| `0x2C73E` | VsRet | Upload site; not a walker |
| `0x22187` | mid epilogue `5F C2 10 00` | VsParent slot — not a start |
| `0x37520` / `0x32BD7` / `0x4D901B` | mid | VsParent slots — resolve to stackarg or none |
| `0x370D0` | stdcall `56 57 8B 7C` | Helper — no dual |
| `0x32A40` | thiscall+stackarg | `[ebp+8]`, ret 4 — **FORBIDDEN** Mode33 |
| `0x372B0` | thiscall+stackarg | CC-pad but stackarg — **FORBIDDEN** Mode33 |
| `0x4D8F10` | thiscall-83EC | Safe; count avg≈0.3 (not ~1×/frame) |
| `0x1BF010` | thiscall-frame | Mode34 COUNT **hard-kill** — **FORBIDDEN** |
| `0x4DDAD0` | thiscall-56 | Mode34 COUNT ok + DUAL `vsPatch=0` — **FORBIDDEN** (no parallax) |
| `0x52E7C0` / `0x5303D0` | ~1×/frame starts | Mode34 rare (0x52E7C0 stackarg forbidden; others untested) |
| `0x37BD0` / `0x2C6AC` | | **FORBIDDEN** |

**Next session:** headset-check Mode **30** + **ipd=1** + zoom OFF (no warp?). Stay on **30**.  
Do not re-arm Mode34 dual. Use `VsRetStatic` safe RVAs for next same-frame COUNT-only probe.  
Playable stays **`30`** + **`ipd=1`** + **`eyefwd=20`** + **`pedhide=1`**. Kill stereo → **`26`**.

**Mode 31:** discover failed (`frameSlots=0`, callee too deep). Superseded by Mode 32/33.

**Mode 30 dual probe (earlier) — DEAD for parallax:**  
device-VS dual `deviceVsPatches=0`. Do not re-arm without replay-thread evidence.

**Mode 29 FAILED (2026-07-24):** `FindFnStartNear(Cand37C01)` → `0x37BD0` crash.  
Modes **27/29** alias Mode 26. **Mode 28:** wrap@0x2C6AC crashed — aliases 26.  
**SteamVR dashboard:** `restart-gtaiv.ps1` closes it after GTAIV is up (default).  
**Quick restart (pinable):** `scripts\install-restart-shortcut.ps1`.

---

## What works (keep)

| Feature | Status | Details |
|---------|--------|---------|
| Stock DXVK 3.0.2 + Interop | Done | `ID3D9VkInteropDevice` / Texture → OpenVR Vulkan submit |
| Same-thread WaitGetPoses + Submit | Done | Extra pose thread → `AlreadySubmitted=108` / freeze — **do not repeat** |
| Mono image in headset | Done | Same BB to L+R; `errL=0 errR=0` |
| FP cam on ped (`FindPlayerPed` + CopyMat) | Done | Armed after ~360 submits; 4/4 call sites |
| HMD orient + 6DoF relative F9 | Done | Only **F9** recenter (no look toggles) |
| Eye-Forward ~0.42 | Done | Default/file 42 cm; with PedHide try 12–20 |
| PedHide (Inspiration) | **ON (test)** | CE SetDraw helper AOB — NOT EndScene natives. Kill: `pedhide=0` |
| camoff FPX/Y/Z | Optional | `gtaiv_dxvk_vr.camoff` = `x y z` cm |
| Locomotion + stick yaw | Done | Stick-yaw sign negated (L/R was swapped) |
| HMD info log | Done | G2: recommended ~2836×2772, FOVh~94°, IPD~60 mm |
| BuildRenderList mid-AOB | Done | Finds fn despite FusionFix prologue patch |

### Known good AOB (DrawScene::BuildRenderList)

- Mid-body (survives FusionFix SafetyHook):  
  `83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C`
- Function start = mid − **13**
- File-offset reference (exe): mid ~`0x527EDE` → start ~`0x527ED1`  
  Runtime example: `@ 00EAD200` (ASLR)

---

## Stereo modes (`gtaiv_dxvk_vr.stereo`)

| Mode | Meaning | Result (tested) |
|------|---------|-----------------|
| **0** | Off (default) | Mono submit, best FPS (~90) |
| 1 | Probe: log BuildRenderList | OK |
| 2 | Dual BuildRenderList, same cam | Stable; **does not draw twice** — lists only |
| 3 | Dual + IPD in CopyMat | IPD had effect; no real dual draw |
| 4 | Temporal L/R: frame L / frame R, StretchRect → 2 RTs, submit | **Real parallax** (user: cup) — fusion incomplete; **FPS ~46** |
| 5 | Phase probe (log order) | Order: **PhaseA → PhaseC → DrawScene** |
| 6 | Same-frame PhaseA→C→Draw L then R + BB submit | Stable, submit L=R=1; **no fusion**; ~50 FPS |
| 7 | Dual BuildRenderList + RT copy + cam IPD | Cam matrix L/R correct (logged up to 200 cm) — **BB still does not change** |
| 8 | Mode 7 + L/R submit swapped | ~45 FPS / worse — do not use |

### Hard finding (2026-07-24)

- F8 sep 0→200 cm: log `camDist=200cm`, user sees **no** image difference → capture is not a real dual draw.
- `BuildRenderList` (PhaseA/C/DrawScene) ≠ RenderView. Building lists 2× + StretchRect = often **the same** image.
- Mode 4 (temporal) had real parallax — there a **full frame** is drawn with the cam.
- VSConstF hooks → ~45 FPS, fusion no → turned **off** again.

**Mode 9 result (VTable, 2026-07-24):**

| Phase | BuildRenderList slot | Execute candidate (next slot) |
|-------|----------------------|----------------------------------|
| PhaseA | `[7]=00928AC0` | `[9]=0053B750` (`[8]`=Stub) |
| PhaseC | `[8]=00D76950` | `[9]=0053E300` |
| DrawScene | `[8]=00ADD200` | `[9]=0049C250` |

Mode 9 FPS hole: AOB on every phase call — fixed.  
**Mode 10:** Execute-only dual → L≠R, F8 sep had no effect. Build+Exec from ExecA = freeze — forbidden.  
**Mode 11:** Build+Exec from PhaseA build → **black screen** — forbidden.  
**Mode 13 spike:** temporal stereo + **native-FOV inset** Submit bounds (no D3D cover widen).  
Cover-crop without cover render caused zoom; Mode **0** = daily ~90 FPS. Mode 11 dead. Kill **0**.  
**Mode 14 — CONFIRMED WORKING (headset test 2026-07-24): FUSION + CORRECT PROPORTIONS.**
Temporal stereo + **angle-correct per-eye canvas**. Root cause of all previous diplopia:
nullptr-bounds Submit stretched the 16:9 game image over each eye's full asymmetric G2 frustum
→ per-eye different angular error → double vision despite correct parallax (mono fused because
both eyes shared the identical error). Mode 14 places the game image at its TRUE angular position
inside a larger black canvas per eye (one StretchRect, no engine FOV write, no Submit UV tricks).
Black border is EXPECTED (game covers ~100% h / ~53% v of the frustum). User: image correct,
performance good. Optional `gtaiv_dxvk_vr.fov` (horizontal degrees 30–150) calibrates world size.
Log: `StereoRender: mode 14 GeometryCanvas`, `StereoCanvas: L/R canvas=… rect=…`, `mode=14`.

**Key measurement:** `gameTan=(1.000,0.562)` at 16:9 → tanH = tanV × aspect exactly →
**GTA IV keeps vertical FOV fixed**; square resolution would SHRINK the image, not fill the
frustum. Black-bar fix must raise the SOURCE vertical FOV.

**Mode 15 — TESTED DEAD (2026-07-24):** SetTransform projection widen produced vertical stretch
→ **Rage ignores `D3DTS_PROJECTION` for its shader passes.** Black-bar fix must change the
engine FOV itself.

**Modes 16/17 (built 2026-07-24):** two-stage engine-FOV plan, both render like Mode 14:
- **16 CamFovProbe** — READ-ONLY: samples floats at mat−0x80..+0xFC around the CopyMat camera
  matrix over 240/2400 frames, logs stable FOV-plausible candidates
  (`FovProbe: off=… DEG/RAD …`). Target value ≈ 58.7 deg vertical (or ≈1.02 rad) at 16:9.
  Cannot freeze (no writes).
- **17 CamFovWrite** — writes `value × scale` at the verified offset, on the game thread right
  after CopyMat (NOT the old blind CCam+0x60 write). Config `gtaiv_dxvk_vr.fovpatch` =
  `"<offsetBytes> <scalePercent>"`. Guards: only writes when the current value is FOV-plausible;
  clamps at 130 deg / 2.27 rad. Log: `FovPatch: #n off=… old -> new`. Canvas adapts
  automatically (reads live projection). Kill: delete file / stereo=14/0.

**Mode 16 probe RESULT (2026-07-24):** all cam mats show a stable, exact **45.000 at mat+0x50
(off=+80)** and mat+0xD0 (+208) — GTA IV's camera FOV field (default 45°). Other candidates were
noise (at.z, player height). Note: rendered vertical FOV is 58.7° = 45 × ~1.304 (engine multiplier),
so `scale` in fovpatch maps ~1:1 onto the render FOV ratio.

**Regression found & fixed (2026-07-24):** trusting `m[1][1]` in `UpdateGameFovFromDevice`
picked up a SQUARE side-pass projection (mirror/envmap) → gameTan flapped 0.562↔1.0 → no bars,
wrong proportions, RT-recreate stutter in Mode 16. Reverted to backbuffer-aspect derivation
(also correct under the Mode 17 FOV patch).

**Mode 17 test 1 (2026-07-24):** bars smaller, image better — offset +80 CONFIRMED as the
engine FOV. But multiplicative per-call writes compounded (45→58.5→76→…112 across call sites)
→ frame-to-frame FOV oscillation = jitter + FPS loss. **Fixed:** baseline captured once per cam,
absolute idempotent target write (`FovPatch: baseline mat=… base=45.000`).

**Mode 17 test 2 (2026-07-24):** jitter gone (idempotent write works), but objects jumped
left/right and FPS 30–50. Cause: at scale 130 the game renders tanH≈1.4 > eye frustum (~1.157)
— GTA IV couples horizontal = vertical × aspect — and `CopyBbToEyeCanvas` clamped only the DEST
rect → per-eye horizontal squeeze → alternating displacement in temporal stereo. **Fixed:**
proper overlap mapping (crop SOURCE where game FOV exceeds the eye frustum, inset DEST where it
is smaller). Log now shows `dst=… src=…`.

**Mode 17 test 3 (2026-07-24):** still jumping at `80 115`. Log proof: +80 is a DERIVED,
animated value — game rewrote our 51.75 to 63.49 next call, later back to 45 → per-frame FOV
oscillation (each temporal eye rendered with different FOV). **New approach:** patch **+208**
(the second constant-45.000 field = likely the base/desired FOV the game derives +80 from),
plus a 30-frame stability gate on gameTan updates in `vr_display.cpp` (canvas rect can never
flap per-frame again, no matter what the engine does).

**Mode 17 test 4 (2026-07-24) — FOV patch CLOSED for now:** offset `+208` accepts the write
(single `FovPatch: #1 45.000 -> 51.750`, game does not fight it) but is **inert** — gameTan
stayed (1.000, 0.562), rendering unchanged. So: **+80 = live FOV but recomputed per frame by
the game (oscillates against writes), +208 = unused field.** A working engine-FOV lever needs
to hook the code that RECOMPUTES +80 (found via the render-root RE below). `fovpatch` file
deleted.

**KEY DIAGNOSIS (2026-07-24):** test 4 was geometrically identical to the confirmed-good
Mode 14 run (same rects, stable tangents, no FOV change) — and the user STILL saw objects
jumping left/right. → **The jumping is the inherent temporal alternating-eye artifact**, not a
bug: at 30–50 game FPS each eye updates at 15–25 Hz, one frame apart; any motion (driving,
walking, head turns) displaces the two eyes' images against each other. The first Mode 14 test
was calm/standing, so it went unnoticed. Luke Ross needs high FPS + own motion compensation
for exactly this reason. Mitigation: more FPS (lower resolution/settings). **Real fix: render
both eyes in the SAME frame.**

**Mode 18 — RenderRootProbe RESULT (2026-07-24):** all phase-hook callers found in seconds
(GTA IV CE 1.2.0.59, main module PlayGTAIV.exe = byte-identical copy of GTAIV.exe, base
0x400000, no ASLR). Static PE analysis pinned the three caller functions — **all thiscall
(this in ecx), NO stack args (plain `C3` ret)** → hookable AND re-callable with our standard
`BuildRenderList_t` pattern:
- **0x8F73B0** (len 0x1C2, prologue `53 55 56 57 8B F1`) — exec dispatcher: calls vt[9]
  Execute on ALL phases from two sites (0x8F7435/0x8F744A); ExecPhaseA ~9×/frame,
  ExecPhaseC/DrawScene 1×/frame from here. Also contains the BuildPhaseA call site 0x8F7FB3…
  region.
- **0x8F7F00** (len 0x10A, prologue `56 57 8B F9`) — PhaseA build root (1×/frame).
- **0xA87680** (len 0x392, prologue `83 EC 20 53 56 57 8B F9`) — loop that BUILDS
  (site 0xA8784B) and EXECUTES (site 0xA876AB) PhaseC + DrawScene, 1×/frame each.
Other DrawScene-exec sites: 0xAD37F3 (1×/frame), 0xA7C7BA (~17×/frame subpasses), 0xA876AB.

**Mode 19 — RootDispatchProbe RESULT (2026-07-24):** per gameplay frame, exactly:
`ExecRoot 1× → BuildRootA 1× → BuildExecCD 13×` (order string `012222222222222`). So
**BuildRootA (0x8F7F00) is the frame render root**: it walks all 13 render phases via an
indirect per-phase call (one site, ret 0x8F7FB3 — PhaseA build and the shared 0xA87680
build+exec wrapper both return there). Menu/loading does not use this path (counters 0).
Caller chain: BuildRootA is invoked from a tiny wrapper 0x5C2BF0 that does
`mov ecx, 0x118D7F0` (global render-phase manager = `this`) before the call; ExecRoot is
called from the adjacent function 0x5C2380 (len 0x866).

**Mode 20 — SameFrameRootDual (built 2026-07-24) — THE fix candidate for temporal jumping:**
hook BuildRootA and run the ENTIRE 13-phase frame build+draw twice per frame:
Left eye → `CopyBbToEyeCanvas(L)` → Right eye → `CopyBbToEyeCanvas(R)`, eye switch via
`RefreshLiveCamForStereoEye()` between passes. Both eyes share one game tick → the temporal
L/R jumping is impossible by construction. Costs ~2× render time (expect roughly half FPS).
Non-reentrant (the dual-call happens at top level after a full walk), unlike dead mode 11.
Log: `StereoRender: mode 20 SAME-FRAME ROOT dual`, `StereoRootDual: #n haveL=1 haveR=1`.
Known open question: the Left capture happens mid-frame (before post-process/UI of the
native pass) — per-eye post-processing mismatch is possible; headset test will show.
Kill: `gtaiv_dxvk_vr.stereo` = 14 (stable daily) or 0.

**Mode 20 RESULTS (2026-07-24):** v1 (build-only dual): jumping GONE, ~50 FPS, but NO fusion
— Rage double-buffers draw lists, the second build overwrote the first → both captures showed
the same image. v2 (build+ExecRoot per eye) and v3 (build+FrameA+ExecRoot per eye): CRASH —
build (thread A) and exec (thread B) run on DIFFERENT threads; calling exec-side code from the
build hook is a cross-thread violation. SEH guard added (falls back to native mono).

**Mode 21 — ViewMatrixScan RESULT (2026-07-24, user moved/drove during scan — hits held):**
the render-phase manager (0x118D7F0) holds **two live camera WORLD matrices**; their position
rows are at **0x118D8C0 / 0x118D9C0** (exe RVAs 0xD8D8C0/0xD8D9C0, matrix base = pos − 0x30).
No inverse/view-translation copies anywhere → Rage derives the view matrix at draw time from
these. Thread IDs confirmed: BuildRootA thread ≠ ExecRoot thread.

**Mode 22 — ExecViewDual RESULT (2026-07-24): DEAD.** ExecRoot run twice on the render thread
with the manager matrices shifted between passes → AV stage 5 (2nd ExecRoot call, fault
0x6E75E3): **ExecRoot CONSUMES the frame's draw lists — a second root call is use-after-free.**
Left capture from the backbuffer inside ExecRoot context worked fine (that part is reusable).

**Mode 23 — ExecViewPhaseDual (headset test 2026-07-24): STABLE, jumping FIXED, fusion still
missing.** Per-phase Execute ×2 (mode 10's crash-free path) + manager-matrix shift between
passes, captures via current-RT → angle-correct canvas. 3300+ dual frames, zero exceptions,
`haveL=1 haveR=1`, 45 FPS (exactly half of the user's stable 90 → the second pass REALLY
renders). But user sees "two separate pictures": near objects double → both passes almost
certainly render from the SAME camera. **Conclusion: the view transform is baked into the
draw lists / shader constants at BUILD time; the manager matrices are bookkeeping copies —
shifting them at exec time changes nothing.**

**Mode 23 headset test (2026-07-24):** confirmed by user — no jumping (same-frame works!),
45 FPS (= half of stable 90 → second pass really renders), but NO fusion ("two separate
pictures" = near objects double → both passes render from the same camera, images land at
infinity in the per-eye canvases).

**Mode 24 — ExecViewConstDual (DEAD for fusion):** phase re-exec never sees the real draws
(`vsCallsR=0`). All VS-constant uploads go through one Rage wrapper (exeRva `0x2C73E`) on a
**dedicated render/replay thread**, while BuildRootA + ExecRoot share the game thread.

**Mode 25 — BuildDualViewShift (DEAD for fusion):** BuildRootA ×2 + manager matrix shift +
VS patch during walk 2. Stable, but `vsCallsR≈1` — same wrong-thread problem.

**Mode 26 — RecordDualReplayShift:**
- Submit fix: `openvr_mono` always calls `StereoTrySubmitEyes` (was stale mode list).
- Headset after submit: fusion YES, jump NO, 90 FPS — but "monitor on face", wrong world
  size, black bars. Cause: VS-only shifted identity-world draws + stale `scale=50`.
- Now: Mode 14 CCam temporal only (VS stack removed — doubled IPD on static geo).
  Entering 14/26 resets scale=150% + HMD IPD. IPD is NOT multiplied by WorldScale
  anymore (150%×IPD broke fusion + violent jump). F7 = 6DoF size only; F8 = stereo cm.
  Black bars = canvas. Desktop L/R jump + look-smear = temporal (same-frame next).
**Kill-switch baseline:** `gtaiv_dxvk_vr.stereo` = **`26`**. Kill → `14` or `0`.

**Mode 30 — PhaseDualDeviceVs → PAIR-HOLD (2026-07-24):**
- Tried: Mode 23 dual + device Get/SetVSConstF before R (safer than prologue hooks).
- Result: dual stable but `deviceVsPatches=0` — no parallax lever at exec.
- Shipped: CCam temporal + **pair-hold** (promote L+R submit textures together).
- User feedback: jump barely noticeable — **keep as playable**. Needs **`ipd≥1`** (was 0 → no 3D).
**Currently deployed:** stereo = **`30`**, ipd = **`1`**, pedhide = **`1`**. Kill → **`26`**.

**Mode 31 — SameFrameReplayDual (2026-07-24):** discover failed (stack too deep).  
Superseded by Mode 32/33. Kill → **`30`** or **`26`**.

**Mode 32 — SameFrameVsParentDual (2026-07-24):** HookSetVSConstF-depth VsParent +  
ABI classify + count gate. Soft empty → early seed. Dual not armed; wrote stereo=30.  
Kill → **`30`** or **`26`**.

**Mode 33 — SameFrameLateVsParentDual (2026-07-24):** WAIT for live VsParent samples,  
then CC-pad thiscall0 only. Wait **worked**; mids are epilogues not walkers;  
`0x4D8F10` count avg=0.31. Dual not armed; wrote stereo=30. See header.  
Kill → **`30`** or **`26`**. Next: find ~1×/frame **caller** of VsRet (not mid slots).

**Comfort pack / Mode 27 (2026-07-24):** Best 3D so far = Mode 26 + user IPD. F6 subtle.
Jitter = temporal. Mode 27 ReplayRootProbe: Cand37BD0 wrong ABI (exception), Cand4D8F85
never called; VS wrapper `0x2C6AC` has **no static E8 xrefs** (indirect only). Back on
**Mode 26**; FusionFix `MotionBlur=0` + `FieldOfView=7` (+35°); **IPD/scale files no longer
overwritten** on mode enter. Next: Mode 28 dynamic caller of `0x2C6AC` for same-frame.

**Insight for full vertical fill later:** at 16:9 the horizontal hits the frustum limit long
before the vertical fills. The proper endgame is SQUARE aspect (`commandline` 1080×1080) +
engine FOV — Luke Ross' square+FOV combo now makes sense for us; canvas adapts automatically.

**New config toggles (all OFF by default, read once at start):**
- `gtaiv_dxvk_vr.eyefwd` (cm, default 38) — smaller = less camera swing on head turns (comfort).
- `gtaiv_dxvk_vr.movemode` = 1 — do not force ped heading; strafe/backpedal via game mapping.
- `gtaiv_dxvk_vr.vehfollow` = 1 (or 2 = inverted) — camera yaw follows vehicle heading, HMD
  free look on top; baseline on entry + F9.

See `docs/HANDOFF_GROK.md` for the remaining plan (HUD positions, aim-cam hook, third-person
toggle).

**Default in code:** Mode **0**. Mode 4/6/7 not for daily use.

---

## Stereo findings (important)

1. **Parallax ≠ fusion.** Mode 4 delivers different L/R angles, but images do not merge cleanly → double vision remains.
2. **`BuildRenderList` ≠ RenderView.** Calling twice + copying BB = often **the same** finished image. Not a BotW-equivalent dual draw.
3. **Temporal alternating-eye** (frame L, frame R):
   - Per eye ~half update rate → user saw **~46 FPS** (was ~90).
   - BotW deliberately rejects alternating; for us only a spike, not a goal.
4. **OpenVR IPD raw values are correct:**  
   `L=(-0.030,…)` `R=(+0.030,…)` sepX≈60 mm.
5. **IPD via cam basis (right/up/−forward) was wrong** — right eye got negative X + large Y.  
   Fix: head rotation × EyeToHead translation, then `OvrToGta`. After fix L/R opposite — fusion still not good (FOV/projection missing).
6. **HMD FOV → `CCam+0x60` (~94°)** → freeze/black screen after load — **do not repeat** without a new plan.
7. Missing **per-eye projection** (view offset only) is the most likely remaining cause of diplopia with correct parallax.

### Ovr→GTA (position/directions)

```
gx = ox;
gy = -oz;
gz = oy;
```

Rage cam matrix: `right`=X, `up`=Y=**Forward**, `at`=Z=**Up**.

---

## Explicitly failed / forbidden

| Attempt | Effect | Rule |
|---------|--------|------|
| Script natives from EndScene (`SET_DRAW_PLAYER_COMPONENT` via NativeContext) | Crash | Use CE SetDraw helper AOB (`pedhide`) instead |
| FOV write ~94° at CCam+0x60 | Freeze after load | Do not repeat this way |
| OpenVR calls from CopyMat (live `GetEyeToHead`) | Freeze | Cached offsets only |
| Pose thread separate from submit | AlreadySubmitted / freeze | WaitGetPoses+Submit same thread |
| BuildRenderList prologue AOB | MISS (FusionFix JMP) | Use mid-body AOB |
| Mode 4 daily use | ~half FPS, fusion incomplete | Default 0; search for dual draw |

---

## Hotkeys / UX

- **F9** — view recenter (SteamVR zero + ped/veh heading baseline; 6DoF unchanged)  
- **F10** — 6DoF translation origin reset (lean/strafe anchor only)  
- **F6** — stereo scale · **F7** — WorldScale · **F8** — IPD cycle  
- SteamVR dashboard auto-closes on restart / after OpenVR init (manual close still OK)  
- Quick restart: `scripts/restart-gtaiv.*` (optional `-DirectExe` after RGLess)  
- Faster start/launcher: `docs/STARTUP_SPEED.md`  
- Inspiration (VC VR / C06alt): `docs/INSPIRATION_NOTES.md`  


- ntfy phone push on agent stop: topic `cursor-henning-atldv3m1iqosbzh2` @ `https://ntfy.sh`  
  Hook: `~/.cursor/hooks.json` → `hooks/ntfy-on-stop/run.cmd`

---

## Three BuildRenderList-like phases (CE exe, unique AOBs)

All share `cmp [edi+0x938], -1`, but different continuations:

| Label | File start | Mid AOB (unique) | Mid−start | Note |
|-------|------------|------------------|-----------|------|
| **DrawScene** | `0x6DC600` | `…FF 0F 84 DE 09 00 00 6A 00 6A 0C` | 13 | hooked so far |
| **PhaseA** | `0x527EC0` | `…FF 0F 84 76 03 00 00 80 3D` | 30 | large stack `0x6D8` → **SceneToGBuffer candidate** |
| **PhaseC** | `0x975D50` | `…FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | 39 | third phase |

RTTI strings in exe: `.?AVCRenderPhaseDrawScene@@`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@`.  
Virtual calls (no direct `E8` to the fn) → dual draw needs correct `this` per phase.

**Mode 5** = phase probe (log only). **Tested 2026-07-23 — success.**

### Mode 5 result (log)

- All 3 hooks OK: DrawScene `@00ADD200`, PhaseA `@00928AC0`, PhaseC `@00D76950`
- Stable order per frame: **PhaseA → PhaseC → DrawScene**
- Stable `this` pointers: PhaseA=`13CA5E60`, PhaseC=`13CB6580`, DrawScene=`10F352D0`
- Occasional duplicate triplets in same `frameSeq` (likely 2nd viewport / reflection)
- `stereo` back to **0** afterward; Mode **6** built as next spike

### Mode 6 — Same-frame dual (new)

File `gtaiv_dxvk_vr.stereo` = **`6`**

Flow (per frame, after warmup of `this` pointers + eye RTs):

1. Natural: PhaseA + PhaseC with **Left** IPD  
2. DrawScene hook: DrawScene Left → StretchRect RT_L → PhaseA+C+DrawScene **Right** → RT_R  
3. EndScene: submit L/R when both RTs filled; otherwise mono fallback  

Kill switch: `0`. Log: `StereoSameFrame:`, `StereoSubmit: ... mode=6`.

**Risk:** BuildRenderList may not immediately fill the backbuffer → L/R may still look similar; then we need real GBuffer/draw execute. Freeze → immediately `0`.

### Mode 6 test (2026-07-23)

| | |
|--|--|
| Freeze | No |
| Fusion (single image) | No — still double vision |
| FPS | ~50 (Mono ~90) |
| Stability | OK enough to walk around |
| `stereo` afterward | back to **0** |

**Interpretation:** Pipeline runs (no crash), but StretchRect from **backbuffer** after BuildRenderList likely does not yield a real L/R pair (list build ≠ draw), and/or projection is missing. ~50 FPS = cost of double chain — expected.

## Sensible next steps

1. **Find GBuffer / execute** — not BB after BuildRenderList; RTs of deferred phase or real draw call.  
2. Per-eye **projection** (without old FOV-freeze path).  
3. Optional: Mode 6 for debug only; daily use = Mode **0**.

Stereo success criteria: parallax **and** fusion, acceptable FPS.

---

## Quick start checklist

1. SteamVR on, dashboard closed  
2. `gtaiv_dxvk_vr.stereo` = `0`  
3. Start GTA → log: `CamMatrix: 4/4 hooked`, `armed after 360`, `MonoSubmit … errL=0`, **no** `StereoTemporal`  
4. Expect FPS ~90  

Kill switch on freeze: kill game → set stereo file to `0` → restart.

