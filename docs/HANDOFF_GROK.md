# HANDOFF — Fable 5 session 2026-07-24 → next agent (Grok)

Read this together with `AGENTS.md`, `docs/CURRENT-STATE.md`, `docs/CONSTRAINTS.md`.
User is not a programmer — always give exact click/command steps and log lines to check.

---

## 0. UPDATE 2026-07-25 — PE +0xC00 RVA skew (CRITICAL)

CE `.text` **mapped RVA = file offset + 0xC00**. Mode **66** REJECT at `0x30300` was this bug
(PublishSync is **`0x30F00`**). ASI in `out-asi\` uses `ResolveReSite()` AOB + mapped RVAs.
**Do not deploy while user is in-game.** Prefer headset test order: **66 → 67 → 65 → 64**.
ViewConst (`0x32470`) has one gated caller — may be sparse. SameFrameSeamGate stays **CLOSED**.
Mode **66** may log **SHARED** (avg>4) because helper **`0x31940`×58** also calls PublishSync — not a failure.
`ResolveReSite` prefers **expected mapped RVA** when prologue matches (short AOB lookalikes).
ViewConst gate `[0x1797694]` is cmp-only / starts 0 / needs nonzero → Mode **64** often dead.
Upload fn `0x2A1E10` (vtable): **two** MatMul×3→+178 paths (`@0x2A217D` / `@0x2A25F9`). Sibling `0x2A1D50` helper-only.
Mode **41/42** OWNER-EDGE logs triad rets **`0x30D13` / `0x2A2183` / `0x2A25FF`** (`path=` + `match=`). SameFrameSeamGate **CLOSED**.
Prefer test order: **66 → 67 → 65 → 42 → 64**.
**No AGENT_LOOP tick shells.** Details: `docs/RE_OFFSETS.md` CRITICAL; `docs/MAPPED_RVA_CHEATSHEET.md`; `scripts/verify-mapped-sites.py`.

---

## 1. BREAKTHROUGH: stereo fusion is SOLVED (Mode 14, headset-confirmed)

**Root cause of months of diplopia:** we rendered with the game projection (~90° h,
16:9) and submitted with `nullptr` bounds. SteamVR stretches such a texture over each
eye's FULL asymmetric G2 frustum (~94°+, ~1:1). The stretch differs per eye (mirrored
asymmetry) → same object lands at different angles in L vs R → double vision despite
correct IPD/parallax. Mono (Mode 0) always fused because both eyes shared the identical
error. Camera code (`cam_matrix.cpp`) was NOT the problem.

**Fix (Mode 14 "GeometryCanvas", `stereo_render.cpp: CopyBbToEyeCanvas`):** per eye,
StretchRect the game backbuffer into the angular-correct rectangle of a 2048×2048 black
canvas (rect computed from `GetProjectionRaw` per eye + game FOV tangents), submit the
canvas with nullptr bounds. Black border around the image is EXPECTED and geometrically
correct.

**User verdict 2026-07-24:** fusion YES, proportions YES ("sieht richtig gut aus"),
performance good. Log evidence: `StereoSubmit: L=1 R=1 mode=14`,
`StereoCanvas: L canvas=2048x2048 rect=(149,474)-(2048,1566) gameTan=(1.000,0.562)`.

**Do not break this baseline.** Any new experiment = new mode number, Mode 14 stays.

## 2. Key measurement: GTA IV FOV behavior (changes an old assumption)

Measured at 16:9: `gameTan=(1.000, 0.562)` → tanH = tanV × aspect EXACTLY.
**GTA IV keeps VERTICAL FOV fixed and derives horizontal from aspect ratio.**

Consequences:
- The black bars are mainly TOP/BOTTOM (game fills ~100% horizontally, only ~53%
  vertically of the G2 frustum).
- **Square resolution (`-width 1080 -height 1080`) will NOT remove the bars** — at 1:1
  the game would shrink tanH to 0.562 and the image gets smaller. The closed-VR square
  trick does not transfer without his engine FOV fix. Don't burn a session on it.
- The correct lever is **raising the vertical FOV of the source render**.

## 3. Mode 15 "CanvasWide" — TESTED, DEAD (2026-07-24)

User test result: vertical stretch → **Rage ignores `D3DTS_PROJECTION` for its shader
passes**. Do not pursue SetTransform-based projection changes again.

**Replacement plan (built, in progress): Modes 16/17 — engine FOV, two-stage.**
- **Mode 16 (probe, read-only, cannot freeze):** logs stable FOV-plausible floats
  around the CopyMat camera matrix (`FovProbe: off=… DEG/RAD min..max`). The engine
  vertical FOV should read ≈58.7 deg (or ≈1.02 rad) at 16:9. Run it, then READ THE LOG
  and pick the offset whose value matches ≈58.7 (or changes when aiming/entering cars).
- **Mode 17 (write):** put `"<offsetBytes> <scalePercent>"` into `gtaiv_dxvk_vr.fovpatch`
  (e.g. `104 160`), set stereo to `17`. Writes on the game thread right after CopyMat,
  guarded (only overwrites FOV-plausible values, clamped). If the write works, engine
  culling AND our canvas follow automatically — black bars shrink with correct geometry.
  If the game fights the write (value resets, visual flicker) → find the projection
  build site instead (RE task).
- If several DEG candidates appear, prefer the one closest to 58.7 that is NOT exactly
  70.0 (that's our own fallback constant, not game memory).

## 4. Open user requests, ranked, with analysis

### 4.1 Black bars → Mode 15 test above (then engine FOV if B)

### 4.2 HUD/minimap visibility (minimap bottom-left, status top-right unreadable)
HUD is drawn into the same backbuffer at screen corners = extreme view angles in VR.
FusionFix.ini (checked, `plugins/GTAIV.EFLC.FusionFix.ini`) has NO HUD position/scale
option (only `RadarZoomDelay`). Options to research:
- GTA IV HUD layout offsets in memory (radar pos/scale floats) — there are known mods
  that move the radar; find a CE-compatible address/AOB and add config values.
- Alternative: shrink-toward-center trick — we could StretchRect the BB into a
  slightly smaller rect than the true angular rect (e.g. 90%) — but that breaks the
  angle-correct geometry that fixed fusion. DO NOT do this globally. A separate
  HUD-only solution is required.
- Long term (playbook M2/M3): HUD as separate OpenVR overlay quad.

### 4.3 Movement: no strafe/backwards — TOGGLE IMPLEMENTED (untested)
`gtaiv_dxvk_vr.movemode` = `1` skips the per-frame ped-heading force in
`vr_move.cpp` — the game then maps stick input relative to the camera we override
with the HMD, which should restore strafe/backpedal naturally. If directions feel
wrong relative to the view, the game is using a stale internal camera heading for
movement mapping → then hook the pad/movement-direction computation instead.

### 4.4 "Camera swings when turning head" (dizziness risk)
Probably NOT game look logic: `ApplyHmdMouseLook` is disabled once the cam override is
armed (`hmd_look.cpp` line ~108). Two real candidates:
1. **Eye-forward lever arm — NOW CONFIGURABLE:** `gtaiv_dxvk_vr.eyefwd` (cm,
   default 38). Eye sits 38 cm in front of the ped pivot, so a head rotation swings
   the camera on a 38 cm arc — vestibular mismatch. Test 12–15 cm for comfort
   (skull may block view; head-hide via script thread is the proper fix later,
   playbook M3).
2. Temporal stereo: one eye is always 1 frame old → swim during head rotation.
   Mitigated only by same-frame dual render (playbook M1) — long term.
Also `vr_move` rewrites the ped matrix each frame from HMD yaw — the visible body
rotating under the camera may contribute to the swing feeling; test 4.3 changes
together with this.

### 4.5 Vehicle heading-follow — TOGGLE IMPLEMENTED (untested)
`gtaiv_dxvk_vr.vehfollow` = `1` adds the vehicle's yaw delta (since entry/F9) to the
camera yaw in `ApplyHmdToCam`; HMD free look stays on top. If turning right rotates
the view the wrong way, set `2` (inverted sign). No smoothing yet — add if jittery.

### 4.6 Aiming switches to third person
The aim camera is a different camera path; we hook only 4 CopyMat call sites
(onfoot front/behind, vehicle front/behind — `cam_matrix.cpp: InstallCamMatrixHooks`).
Find the aim-cam CopyMat call site (same E8 pattern family, different context bytes)
and hook it too, OR find and patch the camera-mode switch on aim. Research task;
test with pistol aim on a ped.

### 4.7 Third-person mode via hotkey
Suggested design: F6 toggles a flag; when active, skip the FP position lock in
`ApplyHmdToCam` (keep HMD orientation + IPD offsets, but anchor position behind/above
the ped using the game's own third-person cam position from CopyMat input). Cheapest
correct version: in third-person mode, apply HMD orientation and stereo IPD around the
game-provided camera position instead of the ped eye. Keep F9 recenter semantics.

## 5. Current run state / files (as of this session's end)

| File (game dir) | Value |
|---|---|
| `gtaiv_dxvk_vr.stereo` | `14` (user tested this; `15` built and ready to test) |
| `gtaiv_dxvk_vr.ipd` | 7 (cm, log showed sep=7cm) |
| `gtaiv_dxvk_vr.scale` | 100 |
| `gtaiv_dxvk_vr.fov` | optional, horizontal degrees 30–150, read once at start |
| `commandline.txt.square-vr-disabled` | keep disabled (see §2) |

Build: `.\scripts\build-asi.ps1` (verified clean 2026-07-24). Deploy+run:
`.\scripts\build-deploy-run.ps1` (waits for ASI_BUILD_ID in fresh log).
Kill switch: stereo file → `0`.

## 6. Do NOT retry (adds to the existing list in CURRENT-STATE)

- Square resolution commandline to remove bars (§2 — game is fixed-vertical-FOV).
- Full-frame VS-constant scans per BeginScene (~45 FPS cost).
- Submit UV/TextureBounds tricks for FOV — Mode 14's canvas replaces them correctly.
- Shrinking the canvas rect below the true angular size to "pull HUD inward" —
  destroys the angle-correct mapping that fixed fusion.
- Everything already on the forbidden list (CCam+0x60 write, natives from EndScene,
  pose thread, Build+Exec dual, prologue AOB, camDist-as-proof).

## 7. Suggested session order for Grok

1. Mode 15 headset test (5 min, decides the black-bar strategy).
2. Comfort spike: `kEyeForward` 0.38 → 0.12 (one constant, big dizziness win expected).
3. Vehicle heading-follow (4.5) — biggest daily-driving win.
4. Movement strafe scheme (4.3) + aim-cam hook (4.6).
5. HUD research (4.2) and third-person toggle (4.7).

One behavior change per test build. Update `docs/CURRENT-STATE.md` after each headset
test. Git push only when the user asks.
