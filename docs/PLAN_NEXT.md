# Plan — where we are and what to do next (2026-07-25)

Read with `docs/CURRENT-STATE.md`, **`docs/RE_OFFSETS.md`**, and `docs/HANDOFF_GROK.md`.

---

## Session note 2026-07-25 ~04:30 — RE offset map (offline; no game launch)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — durable CE 1.2.0.59 RVA/AOB map, forbidden sites, VsRet chain,
  re-find procedure
- **`scripts/offline-re-scan.py`** — PE scan without Steam/GTA
- **`re_validate.cpp`** — runtime AOB drift check (`ReValidate:` in log)
- **Mode 62** (`RePatternValidate`) / **Mode 63** (`SameFrameSeamGate`) — Mode 45 renderer +
  RE logging; **default stays 45**
- **F7** 8-step leanGain (1.0…5.0); stereo file **46–61 → 45** remap

**RE conclusion:** Same-frame distinct eyes still need a **replay-thread draw owner** (~1×/frame,
hookable thiscall). VsRet **`0x2C73E`** is observation-only. **`0x309D0`** indirect edge needs
live **`eax`/vtable** capture (Mode 42 follow-up) — do not hook yet.

**When user returns — pick ONE headset test:**
1. **Baseline:** Mode **45** — smooth? F7 steps 1.0→5.0 make world smaller when leaning?
2. **RE validate:** stereo **`62`** — log shows `ReValidate: OK` for all required AOBs?
3. **Chain probe:** stereo **`41`** — 1 calm minute; save `Mode41: CHAIN` lines for next RE pass
4. **World size (visual, not F7):** F6 stereoscale 125→130; or try `fovadd` **20** (not both at once)

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`.

---

## Session note 2026-07-25 ~01:30 — Mode 61 renderer-soft + worldsize preset

**Shipped:**
- **Mode 61** (`HeadOwnedCamRendererSoft`) — Mode **45** camera unchanged; renderer = RT lock +
  pair-hold + AER pose submit + soft guard (no Mode-40 1.5° flatten). **SameFrame=0** (replay seam
  still blocked).
- **`gtaiv_dxvk_vr.worldsize`** — single visual-scale knob (75–125). **100** = fovadd 18 +
  stereoscale 125. Deployed **110** with default stereo **45**.

**Default deploy:** `stereo=45`, `worldsize=110`, `ipd=3`, `scale=100`.

**Test Mode 61:** write `61` to stereo file, restart, headset-check jump vs 45. Kill **`45`**.

**Test worldsize:** restart after editing worldsize file; verify log line. Kill: delete worldsize +
  restore `fovadd=18` / `stereoscale=125` individually if needed.

**Still deferred:** safe same-tick distinct-eye replay seam — no new hooks this session.

---

## Session note 2026-07-25 ~00:45 — Camera frozen at Mode 45; next = renderer + world size

**Decision:** Stop camera ownership experiments for this phase. Mode **45** is the only default
deploy. Modes 46/59 remap to 45; 47–60 stay in code for manual tests only.

**What “VR renderer + world size” means next (honest scope):**

| Track | Goal | Not a substitute for |
|-------|------|----------------------|
| **Same-frame stereo** | Find a safe replay-thread seam (`VsRet=0x2C73E` chain) so L/R eyes render from **one game tick** with distinct view constants | Another CopyMat / VS-translate / forced-walk cam mode |
| **Renderer comfort (Mode 61)** | AER + soft guard on Mode 45 cam — less flatten, not true stereo | Same-frame or cam rewrite |
| **World size (visual)** | **`gtaiv_dxvk_vr.worldsize`** (75–125) sets **fovadd + stereoscale** together; deployed **110** | F7 WorldScale alone — that only scales **6DoF lean**, not building scale |
| **World size (6DoF)** | F7 linear scale 100–500 at user discretion | Quadratic scale, scale=1000, or cam-anchor clamps |

**One change per session.** Headset proof: `StereoMode: 45`, paired `StereoSubmit: L=1 R=1 mode=45`,
`Mode44: RT lock … recreates=0`, **no** `Mode5x` / `forcedWalk=1` / `VrMove: Mode57` lines unless
you explicitly set an experimental stereo file.

**Kill:** `45` · fallback `30` + delete `fovadd`.

---

## Session note 2026-07-24 ~20:25 — Mode 46 full HMD-basis / tilt test

### Correction ~20:30 — Mode 46 froze after loading and is disabled

The only headset test of Mode 46 froze after the loading screen. Mode 45 immediately recovered
and then passed a two-minute post-load run with advancing `StereoSubmit: L=1 R=1 mode=45`.
The direct OpenVR right/up/forward matrix passed to the late RAGE camera path is therefore unsafe
despite its nominally correct rigid basis. `46` now maps to protected `45` in the config parser;
the deployed stereo file remains `45`. Do not use or re-arm 46, and do not start a Mode 47
translation/WorldScale experiment from that broken pose path.

For every later mode, deploy with its intended stereo file, restart, and require continuing
post-load paired submits over at least two minutes. If the log stalls or the game hangs, write
`45`, restart, verify continuing Mode-45 submits, and document the failure before any new work.

Mode 45 is the protected user-tested stability baseline: no bars, no jitter/jumping, stable
performance. The remaining immediate camera symptom is head tilt appearing to move the world in
the inverse direction. Inspection found that `ApplyHmdToCam` preserved only the HMD forward vector,
then rebuilt right/up against GTA's world-up; it necessarily deletes physical roll.

**Shipped Mode 46:** retain Mode 45's exact Mode-44 RT lock, FOV path, motion guard, and safe
post-CCam late refresh. Only replace that orientation reconstruction with the full OpenVR
right/forward/up basis, converted through the already-proven Ovr→GTA mapping. Controller/vehicle
yaw rotates all three axes together. No CCam+VS stack, no canvas zoom, no change to IPD, no replay
hook/draw, and no forbidden address. F7 remains translation-only: higher WorldScale produces a
larger game-unit translation for the same real lean, i.e. a smaller-feeling world.

**Headset check:** F9; tilt left/right slowly. Horizon must tilt with the head and the world must
remain spatially stable. Then look up/down and lean; confirm no inverse counter-motion. Expected
log: `StereoMode: 46`, `fullHmdBasis=1`, `Mode46 ... fullBasis=1`, `recreates=0`, and paired
submits. If any regression, write **`45`** and restart; do not modify the Mode-45 baseline.

**Still not true VR:** Mode 46 improves the head-owned mono/temporal render view but does not create
same-tick distinct eyes or decouple collision/culling. The next root milestone remains a proven safe
Rage replay-thread seam for two distinct camera views in one game tick.

---

## Session note 2026-07-24 ~20:05 — Mode 44 RT lock

**Shipped Mode 44:** this is the Mode-43 fast current-frame mono guard with exactly one
additional behavior: after the first 1536×1536 submit+hold RT allocation, later canvas/tangent
requests on the same D3D9 device cannot release and recreate those four default-pool textures.
The true-FOV publish gate is also 5% for this mode (rather than the normal 2.5%), filtering
ordinary CCam FOV jitter before it can alter canvas geometry. Bars-gone `fovadd=18`, pair-hold,
and the Mode-43 three-copy guarded path remain unchanged. No game function is hooked, replayed,
or probed.

**Launch proof:** build `20260724-200503` logged `StereoMode: 44`, the RT-LOCK install,
one `eye RTs 1536x1536 OK`, `Mode44: RT lock 1536x1536 initialized recreates=0`, and paired
`StereoSubmit: L=1 R=1 mode=44`. The unattended launch cannot prove a performance gain or a
long-session `recreates=0` periodic counter. **Kill:** `43`, `40`, `37`, or `30` + delete
`fovadd`.

**Next true-VR work:** do not mistake resource stability for same-frame stereo. The open
root problem remains a safe replay-thread seam that renders distinct L/R views in one game tick;
the forbidden/disproved addresses and the 0x309D0 indirect edge stay off limits. Get a headset
FPS/rapid-turn check of Mode 44 before a new visual experiment.

---

## Session note 2026-07-24 ~20:00 — Mode 43 fast motion guard

**Shipped Mode 43:** keep Mode 40's thresholds, temporary-mono result, pair-held true-FOV geometry,
and all replay seams untouched. On a guard firing only, the current R frame still first lands in
`holdR` to preserve the pose/cadence decision, but its L/R current-frame canvases go directly to
the submit textures. This skips Mode 40's extra recanvas to `holdL`/`holdR` plus the two-texture
promotion: **3** full canvas copies in a guarded R epoch instead of **5**. Calm frames use the
unchanged Mode-37 temporal pair-hold path.

**Launch proof:** build `20260724-200006` deployed `StereoMode: 43`, installed the FOV site,
allocated locked `1536x1536` submit+hold RTs, and repeatedly logged `StereoSubmit: L=1 R=1
mode=43`. The unattended launch did not produce HMD motion, so `directSubmit=1` is intentionally
not claimed as live-motion proof yet. It is a rapid-turn overhead reduction, not a same-frame
distinct-eye solution and not a normal-frame FPS cure. **Kill:** `40` (conservative guard), `37`,
or `30` + delete `fovadd`.

---

## Session note 2026-07-24 ~20:15 — Mode 41 replay-chain probe

**Shipped Mode 41:** retain Mode 40's Mode-37 true-FOV pair-hold and its rapid-motion temporary-mono
guard, but add a **read-only** trace only when the real replay-thread VS upload returns to
`VsRet=0x2C73E`. It records each valid stack return with its slot, detects the direct/indirect CALL
immediately before that return where possible, resolves a candidate function start/ABI for logging, and
ranks the result by EndScene epochs. It uses the already-installed D3D9 `SetVSConstF` observation hook:
**no game-function hook, no draw replay, no camera write, and no claimed same-frame stereo.**

**Live result:** build `20260724-194401` reached `VsRetHits=142077` with `SameFrame=0
distinctEyes=0`, paired eye submits (`mode=41`), and no new hook. The trace reconfirmed the forbidden
`0x37BD0` chain and the stackarg chains. It also exposed a frequent `0x30D13 → 0x309D0`
`thiscall-56` node, but its caller is not statically recoverable; direct-call nodes
`0x3199A/0x319A4` resolve only to an unaligned enclosing start. Therefore neither is a safe next
count/replay target yet.

The next probe must establish `0x309D0`'s actual caller ABI/object lifetime without calling it, or map
the indirect edge around it. Do not call it yet. Keep Mode 40/41 as the safe interim and do not revive
Mode 34 dual.

**Kill:** write **`40`** (same headset behavior, no trace), **`37`** (always temporal stereo), or
**`30`** then delete `fovadd`. Mode 41 is a seam-finding step, not the true-VR finish.

---

## Session note 2026-07-24 ~19:50 — Mode 42 resolved the indirect edge

Mode 42 deployed as another **read-only** Mode-40-compatible build. The live replay chain now proves
that `0x30D13` returns from `FF 90 disp32`: `FF /2@0x30D0D`, ModRM `0x90`, length 6. In other words,
the frequent `0x309D0` chain node is reached through `call [eax+disp32]`, not a directly callable
owner function. `mode=42` paired submit remained healthy, and the log honestly reports
`SameFrame=0 distinctEyes=0`, `hook=NO`, and `replay=NO`.
The deployed `20260724-195512` artifact also restores modes 41/42 to the explicit stereo-submit
gate; its log confirms `StereoSubmit: L=1 R=1 mode=42`.

**Next safe probe:** observe `eax` plus the resolved `[eax+disp32]` vtable target at that exact
indirect call site, without invoking it. Do not count-hook/replay `0x309D0` based on its outer
thiscall-looking prologue alone. The target's object lifetime and ABI must be proven before a
same-tick dual attempt.

---

## Session note 2026-07-24 ~19:45 — Mode 39 rejected; Mode 37 restored

**User feedback:** Mode 39 brought black bars back and did not improve jumping or FPS. This is
consistent with the log: Mode 39 actively capped the requested `fovadd=18` to **12°**
(`FovSite ... add=12`), so it necessarily reduced the true-FOV fill while keeping the same
alternating-eye pair-hold renderer.

**Deployed next headset baseline:** `stereo=37`, `fovadd=18`. It restores the no-bars geometry
that Mode 39 removed, while retaining the locked 1536 canvas, pair-latched tangents, and no
CanvasZoom. This is a geometry restoration, **not** a jump or FPS cure.

**Research decision:** L4D2VR/HL2VR render both engine views in one simulation tick; UEVR also
labels alternating/AFR as its last-resort, nausea-prone path. GTA's remaining true-VR gap is
therefore a safe same-tick Rage replay/render seam with distinct L/R view/projection data. Do
not spend another build on a compositor-pose or FOV-cap variation. Independent VR camera stays
after that seam is understood and has a hard fallback to 30/37/38.

---

## Session note 2026-07-24 ~19:38 — Mode 40 motion guard shipped

The safe same-frame **distinct-eye** seam is still blocked: replay-thread VS/draw work is separate
from the game-thread BuildRootA/ExecRoot paths, and the known replay candidates are either forbidden
or proven ineffective/unsafe (`0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`). Do not describe the
previous Phase dual as a solution: it is same-frame but has identical camera images because its view
constants were baked before replay.

**Shipped Mode 40:** keep Mode 37's no-bars FOV/canvas and pair-hold. If the cached L/R HMD poses differ
by ≥1.5° rotation or ≥2 cm translation, rebuild both eye canvases from the current R game frame and submit
them together. This is a deliberate temporary-mono safety valve for rapid HMD turns, not true VR stereo.
It should reduce the worst stale-eye jump while retaining normal stereo when calm. Live log proof:
`StereoSubmit: L=1 R=1 mode=40` and `Mode40: MOTION-GUARD mono pair … SameFrame: L+R from current R
frame rot=…`.

**Headset test:** turn your head quickly, then stand still. Expect less jump during the turn and depth to
return after it. If the temporary mono is worse than the jump: write **`37`** to
`gtaiv_dxvk_vr.stereo`. Hard fallback: **`30`**, then delete `gtaiv_dxvk_vr.fovadd`.

**Next concrete true-VR probe:** a read-only replay-thread call-chain trace around `VsRet=0x2C73E` that
records only validated return/caller relationships and call cadence—no function hook—until it identifies
a replay owner that can safely replay a draw batch twice with distinct view constants. Do not retry any
forbidden RVA or Mode-34 dual.

---

## Session note 2026-07-24 ~19:15 — Mode 38 AER pose submit

User: Mode 37 still monitor/huge/jump/FPS; F7 ≠ presence; Luke AER v2 Patreon.  
**Shipped Mode 38:** Mode 37 + capture-time HMD pose + `Submit_TextureWithPose`. Not AER v2 OF.  
Kill → **37** or **30**. Presence still deferred (independent cam / same-frame).

---

## Session note 2026-07-24 ~19:30 — Mode 39 low-motion FOV comfort

Mode 38 logged successful `Submit_TextureWithPose` but user still reports severe jumping. That rules out
another pose-submit variation as the best next move; it cannot make the temporally stale eye newly rendered.
**Shipped Mode 39:** Mode 37 true-FOV canvas/pair-hold, but cap the effective FOV ADD at **12°** even when
the preserved `fovadd` file says `18`. This is one comfort/FPS behavior change only: less FOV fill/crop stress,
no CanvasZoom, no new hooks, and no AER v2 optical flow.
**Test:** check `Mode39: fovadd requested=18 capped=12` plus pair promotion and mode-39 submits. Compare the
same head turns/walk against 38. Kill → **38** (restore pose/full FOV), **37** (full FOV), or **30** + delete
`fovadd` (smoothest protected baseline). Same-frame remains the sole root-cause path if this does not help.

---

## Session note 2026-07-24 ~19:00 — Mode 37 comfort retune

Mode 36 killed bars but cost **jump + FPS + huge world**. Pair-hold was still active.  
**Shipped Mode 37:** true-FOV canvas kept + Mode30 pair-hold; fovadd **18**; canvas **1536 locked** (no RT flap); pair-latch rects; WorldScale **100**. Protect 36/35; fallback 30.

---

## Session note 2026-07-24 ~18:45 — Mode 36 true-canvas FOV

Mode 35 engine FOV worked but **canvas still used stale D3DTS_PROJECTION** → black bars remained.  
**Shipped Mode 36:** publish post-`fovadd` CCam FOV → canvas tangents (no CanvasZoom). Deployed **fovadd=22** (~90% v fill). Mode **35** protected as kill.  
Also: ParseModeFile max 36; idempotent ADD; ADD only if base FOV ≤55°.

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
| Mode 35 + fovadd | better presence | keep Mode30 path | — | engine FOV ADD; bars remain (canvas ignored ADD) |
| Mode 36 + fovadd=22 | bars gone | keep Mode30 path | **strong jump + FPS hit** | true-canvas; RT flap from FOV wobble |
| Mode 37 + fovadd=18 + scale=100 | F7 smaller lever | Mode30 pair-hold | RT locked | comfort retune; headset-verify |

**Conclusion:** F7 “WorldScale” was doing two jobs at once (L4D2-style). Cranking it for size also cranked stereo disparity → temporal stereo cannot fuse that. Decoupling was correct for fusion; size must be fixed by **another lever** (real engine FOV — Mode 35/36/37 — not claimed canvas FOV).

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

→ **Headset:** Mode **37** + `fovadd=18` + `scale=100` — bars still gone? Less jump than 36? Better FPS? World less huge?  
→ Soft kill → stereo **36** or **35** (keep fovadd). Hard kill → **30** + delete `fovadd`.  
→ If still huge: F7 to **125/150** (6DoF only) — do **not** touch IPD.  
→ If still jump: try fovadd **15**; same-frame remains the real jump kill.

Protect playable: **`stereo=30`** fallback, Mode **36/35** protect, **`ipd≥1`**, pedhide/eyefwd/stereoscale.

---

## 7. Current knobs (for the user)

| File / key | Role now |
|------------|----------|
| `gtaiv_dxvk_vr.stereo` = **`37`** (protect **`36`/`35`**, fallback **`30`**) | Comfort true-canvas / Mode36 / Mode35 / pair-hold |
| `gtaiv_dxvk_vr.fovadd` = **`18`** | Degrees ADD at CCam+0x60. Kill = delete/`0` |
| `gtaiv_dxvk_vr.ipd` = **`1`+** | F8 cycles 1,2,3,4,6,7,10 |
| `gtaiv_dxvk_vr.scale` = **`100`** | 6DoF; HIGHER = smaller world |
| `gtaiv_dxvk_vr.stereoscale` = `125` | Soft disparity |
| `gtaiv_dxvk_vr.eyefwd` = `20` | With PedHide |
| `gtaiv_dxvk_vr.pedhide` = `1` | Kill → `0` |
| `gtaiv_dxvk_vr.zoom` | **DISABLED** in code |

Baseline to protect: **Mode 30 pair-hold** + Mode 36 no-bars true FOV.
