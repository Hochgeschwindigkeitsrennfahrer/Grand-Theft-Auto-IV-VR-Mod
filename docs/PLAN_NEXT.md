# Plan — where we are and what to do next (2026-07-24)

Read with `docs/CURRENT-STATE.md` and `docs/HANDOFF_GROK.md`.

---

## Session note 2026-07-24 ~18:20 — Mode 35 FOV recompute spike

Research (Luke/Halo/L4D2/UEVR/FusionFix): **true engine FOV** kills monitor-on-face; canvas zoom / claimed FOV warp = REJECTED.  
**Shipped:** Mode **35** = Mode 30 pair-hold + chain-hook FusionFix Custom FOV CALL → ADD at `CCam+0x60` via `gtaiv_dxvk_vr.fovadd`.  
Playable fallback stays **30**. Independent VR cam still deferred.

---

## Session note 2026-07-24 ~18:00 — canvas zoom KILLED

Headset: `gtaiv_dxvk_vr.zoom` claimed-FOV warp made every head move warp the world (same class as FusionFix FOV look-up warp).  
**Code:** zoom path forced no-op (true FOV). **Deploy:** Mode **30**, `ipd=1`, `zoom=100` (ignored).  
Do **not** re-enable canvas zoom or FusionFix FieldOfView for size.

---

## Deferred (do not implement this session)

### Independent VR camera vs game-camera wall collision

**User ask:** look sideways/up near walls → game cam collides / stops; want VR view independent of game cam.  
**Verdict:** **Yes, sound idea — deferred.** See `docs/INSPIRATION_NOTES.md` (Luke Ross pitch override + render-time view fix).  
Invasive: culling, weapons, shadows, HUD. Keep PedHide + eyefwd for head embed; revisit after same-frame or when wall-look is the #1 complaint.

### Mode 34 dual

COUNT may probe; **dual stays DISABLED** (`0x4DDAD0` vsPatch=0). New read-only `VsRetStatic:` E8→`0x2C73E` scan logs safe CC-pad starts for a future same-frame attempt. Playable = **30**.

### Mode 35 theater/quad A/B

Research: theater = menus/cutscenes only (Luke/VC VR). Not the presence fix. Skip unless FOV site fails.

---

## 1. What the last tests proved

| Setting / build | World size | Fusion | Jump | Notes |
|-----------------|------------|--------|------|-------|
| Mode 26, scale 100%, IPD×scale | too big | near-perfect with F8 | mild | proportions OK after removing VS+CCam double |
| Mode 26, scale 150%, IPD×scale | **top** | **gone** | **violent** | effective eye sep ≈ 9 cm |
| Mode 26, scale 150% 6DoF only, IPD = HMD ~6 cm | too big again | jump less | better | size feel was mostly from **extra parallax**, not 6DoF |
| Mode 30 + canvas zoom 85 | less telephoto? | — | **warp on head move** | **REJECTED** — kill zoom |
| Mode 35 + fovadd | TBD headset | keep Mode30 path | — | engine FOV ADD at recompute |

**Conclusion:** F7 “WorldScale” was doing two jobs at once (L4D2-style). Cranking it for size also cranked stereo disparity → temporal stereo cannot fuse that. Decoupling was correct for fusion; size must be fixed by **another lever** (real engine FOV — Mode 35 — not claimed canvas FOV).

---

## 2. Why the world still feels huge (even with “correct” IPD)

Three separate effects stack:

1. **Letterbox / window (Mode 14 canvas)**  
   Game FOV (~59° vertical) inside G2 (~94°+). Black bars top/bottom = you look through a smaller window. Brain often reads that as “giant world / drone / monitor on face”, even when stereo geometry is right.

2. **Temporal stereo**  
   L and R from different frames. Motion (look around) → smear. Large disparity → jump. Halo explicitly disables **motion blur** for the same “stereo echo” reason.

3. **No same-frame dual eye** yet  
   Halo MCC VR, L4D2VR, BotW BetterVR all render **both eyes in one tick** with per-eye pose/FOV. We still alternate frames (Mode 14/26/30). That is why jump/smear/“half FPS feel” remain.

IPD alone cannot fix (1). Scale×IPD “fixed” (1) by fake size cues and broke fusion. Canvas zoom “fixed” (1) by lying about FOV and broke head stability.

---

## 3. What Halo-MCC-VR teaches us (relevant parts only)

Repo: https://github.com/pancreations/Halo-MCC-VR (OpenXR, D3D11 — different stack, same VR physics).

| Halo practice | Meaning for GTA IV |
|---------------|--------------------|
| **True per-eye stereo same-frame** | Our #1 remaining stereo bug is temporal. Same-frame is mandatory for “real 3D” without jump. |
| **Engine FOV ≈ 120** (user setting) | Their #1 display rule: wrong FOV = wrong scale + edge pop-in. For us: **raise Rage vertical FOV** (Mode 35 / FusionFix site), not square resolution / not canvas zoom. |
| **`resolution_scale` ≠ world/IPD** | Separate knobs. We must keep **F7 size** and **F8 IPD** separate (already started). |
| **Motion blur off** | Reduces stereo echo / smear. Check FusionFix / GTA blur settings. |
| **FSR off (breaks VR scale)** | Analog: don’t let post-process stretch fight the canvas. |
| Stereo proof toggle (F11 alternate) | They treat temporal L/R as a **dev check**, not the product. |

Do **not** copy their OpenXR/D3D11 code 1:1. Copy the **priority order**: FOV fill → same-frame stereo → HUD/input polish.

---

## 4. Recommended roadmap (one change per session)

### Phase A — Size without breaking fusion

**A1. Soft stereo scale** — shipped (`stereoscale`). Keep.

**A2. Engine FOV (Halo lesson)** — Mode **35** FOV recompute site + `fovadd`. Headset-verify.  
**A2-REJECTED:** canvas `zoom` claimed-FOV (warp).

**A3. Motion blur off** — FusionFix. Keep.

### Phase B — Kill jump (main stereo milestone)

**B1. Same-frame on replay thread** — Mode 34 dual disabled; use `VsRetStatic` E8→`0x2C73E` safe starts for next probe. Never `0x2C6AC`/`0x37BD0`/`0x1BF010`/`0x4DDAD0`. Fail → **30**.

**B2. Desktop mirror** — left eye only later.

### Phase C — After fusion + size + no jump

- Independent VR cam vs game-cam collision (**deferred** — see above).  
- HUD inward, aim-cam, third-person, OpenXR only if OpenVR solid.

---

## 5. What we will *not* do next

- Re-enable canvas zoom / FusionFix FieldOfView for “less telephoto”.  
- Stack VS patch + CCam IPD again.  
- Multiply F8 IPD by F7 WorldScale.  
- Square `commandline` without FOV fix.  
- `D3DTS_PROJECTION` widen / blind `CCam+0x60`.  
- Mode 34 dual arm without a proven VS-upload walker.  
- Invasive independent VR cam this session.

---

## 6. Suggested next session (pick ONE)

→ **Headset:** Mode **35** + `fovadd=15` — less monitor-on-face? No look-warp?  
→ Kill → stereo **30**, delete `fovadd`.  
→ If FOV good: keep `fovadd` and return stereo to **30** (hook can be enabled for 30 later) OR stay on 35.  
→ **Same-frame:** pick one `VsRetStatic safe=1` RVA, COUNT-only.

Protect playable: **`stereo=30`** fallback, **`ipd=1`**, pedhide/eyefwd/scale/stereoscale unchanged.

---

## 7. Current knobs (for the user)

| File / key | Role now |
|------------|----------|
| `gtaiv_dxvk_vr.stereo` = **`30`** (test **`35`**) | Pair-hold / FOV site test |
| `gtaiv_dxvk_vr.fovadd` | Degrees ADD at CCam+0x60 (Mode 35). Kill = delete/`0` |
| `gtaiv_dxvk_vr.ipd` = **`1`** | Closest F8 preset (cycles 1,2,3,4,6,7,10) |
| `gtaiv_dxvk_vr.scale` = `50` | 6DoF head move only |
| `gtaiv_dxvk_vr.stereoscale` = `125` | Soft disparity |
| `gtaiv_dxvk_vr.eyefwd` = `20` | With PedHide |
| `gtaiv_dxvk_vr.pedhide` = `1` | Kill → `0` |
| `gtaiv_dxvk_vr.zoom` | **DISABLED** in code (write `100` or delete) |

Baseline to protect: **Mode 30 pair-hold** + Mode 14 geometry canvas (true FOV).
