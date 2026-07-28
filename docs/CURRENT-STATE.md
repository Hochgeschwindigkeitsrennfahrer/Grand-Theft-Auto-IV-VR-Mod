# CURRENT-STATE — gtaiv-dxvk-vr

**As of:** 2026-07-27 18:56 PT

## Current candidate: Mode 56 direct OpenXR L/R pair (offline PASS, no headset result)

Upstream was fetched on 2026-07-27: `origin/master` is still `01f355b` and is
already contained by this branch. The full clean OpenVR release at
`origin/backup/clean-session-20260727` (`416427e`), followed by the development-setup
commit at `origin/cursor/setup-dev-requirements-d131` (`5308417`), is now integrated.
Its Modes 120–136 and supporting camera, late-latch, performance, and setup code remain
available as the explicit OpenVR/Reverb G2 fallback.

The prior GPU bridge is not a usable gameplay route: its host release fence blocked
the GTA/DXVK queue after a few world transactions, leaving the headset on stale
images. The direct OpenXR route now uses the FNVVR-style CPU mailbox instead: no host
release fence, shared Vulkan image, or GPU queue wait is placed in GTA.

Mode **56** reuses the clean release's guarded **DrawScene ×2** rendering seam for
the direct OpenXR route. It renders GTA once with the left OpenXR eye pose and once
with the right OpenXR eye pose,
then publishes the two completed images as one atomic transaction. It intentionally
does **not** call upstream OpenVR submission, SteamVR, stale-frame HOLD, or mono-look
experiments while `backend=openxr`; those paths remain isolated to the explicit
OpenVR fallback.

| GTA state | Mode 56 OpenXR presentation | Required behavior |
|---|---|---|
| gameplay | distinct left/right projection textures | real binocular candidate from two GTA renders pinned to one cached OpenXR pose |
| pause/map, loading, phone | local-space quad latched where it opens | fixed virtual screen, correct GTA aspect, never face-locked |
| fallback | Mode 55 center-eye projection | head-tracked mono if the guarded DrawScene seam disables itself |
| both | Quest Touch through GTA's XInput path | sticks, triggers, face/shoulder/thumb buttons, D-pad/Start/Back chords, recenter, and rumble-to-haptics |

The mailbox ABI is now **v6 / CPU v2**. Each triple-buffered slot reserves two
1280×720 BGRA images. World-stereo is accepted only when the producer marks a
same-tick `VerifiedDrawSceneStereo` pair; the x64 host copies left and right into
different private D3D11 textures. Mono world and UI send one source image and the host
duplicates it deliberately. This makes the transport honest: it cannot call a
single-image frame stereo.

The washed-out color fix remains active: host sampling uses an explicit
`*_UNORM_SRGB` SRV, so GTA's gamma-encoded pixels are decoded exactly once before the
OpenXR sRGB swapchain write.

Offline gates now pass:

- x86 ASI build and direct-OpenXR isolation checks;
- stereo/WVP regression suite;
- controller suite:
  `sticks=1 triggers=1 face=1 shoulders=1 thumbs=1 dpad=1 menu=1 recenter=1 haptics=1`;
- x64 WARP mailbox ingest/readback for mono duplication, distinct L/R world stereo,
  and world-stereo -> stationary-UI switching:
  `protocol=v6 worldStrict=1 wvpProof=1 drawSceneProof=1 immersiveMono=1 cpuMailbox=1 cpuMailboxStereo=1 cpuMailboxWorldUi=1 stationaryUiQuad=1 uiAspect=1 routeSwitch=1 srgbDecode=1 runtimeUntouched=1`.

Post-merge artifacts built from integration commit `4eb195f` (not deployed):
x86 ASI SHA-256
`2BB6B7FC6B3369366354EBE37CDFBED83E75BAF1EC5C7C5D63AE6A04995FF912`;
x64 host SHA-256
`94ACC4CDEA7B83236F5B1850F5B1EAA2AF3C006E12E112BF0249EA0968EB4777`.
No GTA, OpenXR session, Steam, or SteamVR process was started for these builds.

The merged setup scripts now verify vendored Git dependencies without changing
global `safe.directory` settings. OpenVR is pinned and verified at `v2.12.14`;
the host build also normalizes duplicate `PATH`/`Path` entries before invoking
MSBuild. Both fixes were required to reproduce the clean upstream build on this
workspace.

The guarded launcher remains SteamVR-blocked and restores the installed game state.
It defaults to Mode 55; the explicit Mode 56 test command is:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" -StereoMode 56 -MaxSeconds 900 -Authorized
```

It removes/backups `openvr_api.dll`, writes the disable sentinel, checks the Meta
OpenXR runtime, polls for every SteamVR process every 100 ms, and aborts if SteamVR
appears. It has not been run against Mode 56. A single supervised headset result must
prove smooth world motion, real L/R depth, stationary menus, color, and controls
before this can be called working.

## Earlier direct Quest/OpenXR integration and live-run history

**Controlled launch result, 2026-07-26 22:46 PT:** the x64 host used the Oculus
OpenXR runtime, identified Meta Quest 3 and the RTX 4070 SUPER, created the D3D11
session, initialized Touch actions, and reached `FOCUSED`. A direct, argument-free
`GTAIV.exe` start then exited into the Steam/`PlayGTAIV.exe` authentication route.
The attempt was stopped instead of following that route. SteamVR did not start.
The candidate ASI never reached a new in-game session, so this was **not** a Mode 54,
GPU-transport, stereo, menu, or controller result. All test processes were stopped
and the original installed ASI plus `backend=off` / `stereo=0` were restored.

**Second controlled result, 2026-07-26 22:57 PT:** normal Steam/Rockstar
authentication successfully started the candidate ASI in GTA with `backend=OpenXR`,
Mode 54, Quest pose samples, Touch-to-XInput ready, and all six WVP draw hooks ready.
Before a world transaction was produced, SteamVR started. The run was terminated,
including SteamVR's first respawn, and all game files/settings were restored.

**Third controlled result, 2026-07-26 23:11 PT:** the guarded launcher temporarily
removed `openvr_api.dll`, created the OpenVR disable sentinel, and started GTA through
normal Steam/Rockstar authentication. SteamVR remained absent for the entire run.
The Meta Quest session reached `FOCUSED`; Touch-to-XInput, the six D3D9 draw hooks,
the x86/x64 GPU bridge, GTA transaction publication, host connection, and host GPU
acquisition all reached `READY`. Loading/menu output appeared on the implemented
head-locked mono quad.

When GTA changed from UI state to world gameplay, Mode 54 installed all three
`Execute` hooks and immediately raised `0xC06D007E` at left-eye capture stage 3.
That is the Visual C++ delay-load exception for `ERROR_MOD_NOT_FOUND`: the shared
canvas capture still called `GetEyeRawProjection()` → `vr::VRSystem()` while the
OpenVR DLL was deliberately absent. The exception guard permanently disabled the
dual pass and GTA continued native mono. The user confirmed **"never went stereo"**.
The preserved result is `verifiedWvp=False`; no world frame was falsely accepted.
Evidence: `out-openxr/runs/20260726-231140-steam-safe/`.

**Post-test fix, not live-tested:** Mode 54 canvas capture now converts the Quest
`PoseBridge.eyeFovs` directly into each eye's raw asymmetric tangents. It never calls
the legacy OpenVR projection cache. The build adds a source isolation gate plus
numeric left/right/top/bottom regression coverage. Clean x86 ASI SHA-256:
`09DC5691BC0324D71C88FB38E725DFD10A69FB3AA0AB5BCCB50C12F4D23FE3E8`.

**Fourth controlled result, 2026-07-26 23:22 PT:** the same guarded launcher again
kept SteamVR absent, and the OpenXR per-eye tangent fix reached gameplay
successfully. Mode 54 still produced no accepted world frame. Its own audit reported
left/right draw sequences `0/0` and `verified=0`, while later logs showed the real
vertex-shader/draw traffic executing on GTA's dedicated replay thread after the
supposed dual-pass window had closed. This is decisive: the ASI `Execute`-twice
window does not own the D3D9 geometry execution and cannot create true stereo there.
The user also reported a washed-out image, which led to the sRGB fix in the current
Mode 55 candidate. Evidence:
`out-openxr/runs/20260726-232255-steam-safe/`. The game, host, and all settings were
stopped/restored after the test.

The logs proved two independent SteamVR triggers:

- the historical restart script called
  `vrmonitor://debugcommands/system_dashboard_toggle` by default;
- the new OpenXR path still called the shared OpenVR `LogVrDisplayInfo()` helper,
  and Mode 54 canvas sizing could call `GetCoverFovTangents()`. Steam's
  `vrclient_GTAIV.txt` recorded GTA as `VRApplication_Scene` and started
  `vrserver` for app 12210.

Both are fixed offline. The restart dashboard URI is explicit opt-in only and is
refused unless SteamVR already exists. Commit `400294e` removes OpenVR display calls
from the OpenXR path, computes canvas cover tangents from the Quest OpenXR
`PoseBridge` FOV, and fails closed if `openvr_api.dll` is ever loaded while
`backend=openxr`. The supervised launcher adds two operational guards: it creates
the existing OpenVR disable sentinel and temporarily removes/backups
`openvr_api.dll`, restoring both after the run. The 23:11 test proved these guards:
SteamVR never started and the original game state was restored.

Upstream `origin/master` at `01f355b` (Modes 50–53), the full clean OpenVR release
at `416427e` (through Mode 136), and its setup child `5308417` are integrated into
`codex/openxr-sidecar-integration`. The direct path keeps all GTA-loaded code x86 and
uses a separate x64 `gtaiv_xr_host.exe`. OpenVR/Reverb G2 remains an explicit
fallback; `backend=off` is truly flat and initializes no compositor.

The failed DXVK D3D9 KMT-handle route has been replaced. The x86 ASI now creates
native D3D11 NT-handle textures and shared fences on DXVK's exact adapter, imports
them into DXVK's Vulkan device, and GPU-copies a distinct L/R transaction. The x64
host duplicates and opens those D3D11 handles. At the 23:11 test the contract was
pointer-free ABI v4, triple-buffered, ready/release timeline synchronized, and
contained no CPU pixels. The current Mode 55 candidate advances that contract to v5
for explicit immersive-mono and UI-content-aspect metadata. The 23:11 test
live-verified the underlying transport through host GPU acquisition. It did not
verify world stereo because the producer failed before a proved WVP pair.

Mode 54 was the non-default direct-OpenXR same-frame stereo candidate. It keeps
Mode 23's stable per-phase `Execute`-twice boundary, but intercepts the actual D3D9
`Draw*` calls during the second pass. The active vertex shader's embedded CTAB must
declare an exact row-major `gWorldViewProj`; shaders that also declare `gWorld` prove
the camera factor. The dominant gameplay-camera factor is patched for the right eye;
other classified WVP factors (for example, auxiliary light/reflection passes) remain
unchanged. Every patched constant is restored immediately after its draw.

Mode 54 publishes nothing unless the left/right draw counts and rolling sequence
hashes match, all gameplay WVP draws were patched, a dominant `gWorld` camera factor
was proved, original constants match, and every Set/restore succeeds. ABI v4 carries
`VerifiedWvpStereo`; the x86 producer withholds unproved world frames and the x64 host
rejects them. Missing/malformed CTAB, ambiguous camera factors, temporal pairs, mono
pairs, and partial coverage therefore produce no world transaction instead of being
called stereo. UI quads intentionally do not require the world-WVP proof. The 23:22
run proved that GTA's real draw replay occurs outside this capture window, so Mode 54
is retained as diagnostic history and is not the next headset target.

Pose and input are now wired, not merely logged: exact OpenXR eye pose/FOV history is
matched to accepted world frames; fresh focused Quest Touch actions feed GTA's native
XInput path; stale/unfocused input falls through to a physical controller; XInput
rumble is bridged back to OpenXR haptics.

The Mode-54-era menu implementation used a separate presentation mode:

- world gameplay: strict same-simulation-tick stereo projection;
- pause/map, loading, and phone: one fresh GTA image on a view-space, head-locked
  16:9 OpenXR quad shown identically to both eyes;
- read-only GTA IV CE 1.2.0.59 UI-state probes are required or the OpenXR path refuses
  to arm;
- normal GTA XInput navigation remains active on the Touch sticks/buttons.

The 23:11 headset test confirmed that the mono menu image reached the quad, but the
user rejected the head-locked placement: it must be a virtual screen fixed in the
local world when opened, **not glued to the face**. Mode 55 replaces that behavior
with the stationary local-space latch described at the top of this document.

Offline signature check against the installed `GTAIV.exe` (file version 1.2.0.59,
SHA-256 `08759A5516F9837920EA504436236BBAB89D0826A8E4D04FF106345177B5345D`)
found exactly one supported pause/map fallback, one loading signature, and one phone
signature. The newest FusionFix pause signature itself is absent from this disk image,
so the verified 1.2.0.59 form is retained as an explicit fallback.

At that point both builds passed offline checks: ASI PE32/x86; host PE32+/x64. The x86 test reported
`StereoWvpTest: PASS math=5 ctab=1 pairAudit=1 openxrFov=1 openxrEyeRaw=1 runtimeUntouched=1`;
the host reports
`protocol=v4 worldStrict=1 wvpProof=1 uiQuad=1 runtimeUntouched=1`.
Clean build from code commit `85a9967`: ASI SHA-256
`09DC5691BC0324D71C88FB38E725DFD10A69FB3AA0AB5BCCB50C12F4D23FE3E8`;
x64 host SHA-256
`C7F02CA8F40351B7F7DEAFD6006E7D440EFF9D2222F9B1C2D26E86681C83B472`.

**Important historical conclusion:** Mode 54 is compiled and fail-closed, but the
23:22 run proved that DXVK/GTA replay does **not** reach its six draw hooks inside the
dual-pass window. It produced `draws=0/0`, no WVP proof, and no stereo. Do not repeat
Mode 54 tuning or call it full stereo.

**Safety:** the installed game is restored to the original ASI, `backend=off`, and
`stereo=0`. GTA, Meta host, and SteamVR processes are absent.
`scripts/run-openxr-gta.ps1` is preflight-only and cannot start GTA, the OpenXR host,
Steam, or SteamVR. The next GTA test must use
`scripts/run-openxr-gta-steam-safe.ps1`, including its OpenVR DLL removal and disable
sentinel, and requires a new explicit authorization.
**Do not put on the headset yet.** The complete Mode 55 gameplay/menu/color candidate
has no live result.
**Last OpenVR baseline (not running; backend is now `off`):** stereo **`51`**
(Mode-50 always-distinct L/R + **`Submit_TextureWithPose`** AER). The full clean
OpenVR release history follows.

---

## Integrated clean OpenVR fallback history

## 2026-07-27 ~16:48 — Mode **136** LATE-LATCH (deployed / live)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode136-latelatch` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`136`** · dualn **`2`**. ASI **218112** B · SHA256 `C0A7B977…`.<br>
**Mode 120–135 code paths untouched.** Kill: **`120`** + dualn **`2`**.

### What changed
- Content = Mode **120** (DrawScene×2 everyN=2 + HOLD; no monolook / LIVELOOK / AlwaysFresh).
- Submit = **`Submit_TextureWithPose`** every eye; pose from `GetDeviceToAbsoluteTrackingPose` **right before Submit** (not capture-time AER).
- Goal: SteamVR reprojection between content frames → smoother HMD look vs 135.

### Log proof (this deploy)
- `ASI_BUILD_ID 20260727-mode136-latelatch`
- `StereoMode: 136` · `CLEAN LATE-LATCH` · `everyN=2`
- `LateLatch: eye=L/R TextureWithPose=1 err=0`
- `StereoSubmit: L=1 R=1 mode=136`

### Test (DE)
1. Quick Restart (already 136/2) → headset.
2. Still ~90? Slow/fast yaw+pitch natural vs **135**? Compare kill **120**.
3. Bad → stereo **`120`**, dualn **`2`**.

### Next (prepared only — not deployed)
- **137** Cam/Pose EMA · **138** same-moment L/R · **139** cover FOV/zoom — blueprints in SESSION_PREP.

---

## 2026-07-27 ~16:00 — Mode **133** HMD-cheap monolook (deployed)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode133-hmcheap` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`133`** · dualn **`3`**. ASI **214528** B · SHA256 `E54D1340…`.<br>
**Mode 120 / 130 / 131 / 132 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode133_20260727-160043`.<br>
**Expect log:** `ASI_BUILD_ID 20260727-mode133-hmcheap`, `StereoMode: 133`, `CLEAN MONOLOOK-HMD-CHEAP`, `MONOLOOK-CHEAP`, `EndScene LIVELOOK BB→L same-Submit`, `StereoSubmit … sameL=1`, `POSEHOLD-CALM`, `everyN=3`.

### Why desktop > HMD on Mode 132 (log-proven)

- **AppFPS** often ~88–90 calm (dips ~60–76 under load); **pose_ms** often ~13–21ms WaitGetPoses → POSEHOLD.
- **Monolook frequent** on look: `MONOLOOK-HMD` + `EndScene LIVELOOK BB→both` every frame (#1…#600+).
- Desktop Present after WaitGetPoses shows **live BB** (no copy). HMD pays **StretchRect×2** (BB→L and BB→R) + dual Submit → feels behind even when AppFPS≈90.

### Mode 133 (chosen: cheapest every-frame fresh)

Keep 132 pitch/hyst (**2.5°** / sustain **3** / **600ms**). During monolook: **one** StretchRect BB→L, Submit **same** texture for L+R (OpenVR allows; halves copy). Calm: dualn=**3** HOLD. No every-3rd starve (131 fail). No stale stereo HOLD on look (128 ghosting). dualn kept at 3 (AppFPS already ~90 when calm — gap was latency/copy, not dual cadence).

**Test (DE):** Quick Restart → HMD näher an Desktop? · look ghost weg? · nicht einfrieren? · calm ~90? Kill: **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| **130** | monolook interim (ok) | 3 |
| **131** | **FAILED** HMD starve | 3 |
| **132** | HMD-every BB×2 (desktop still ahead) | 3 |
| **133** | **HMD-cheap** BB→L + same Submit (live) | **3** |

---

## 2026-07-27 ~15:51 — Mode **132** HMD-every monolook (prior)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode132-hmdevery` · OpenVR **v2.12.14**.<br>
**Was live:** stereo **`132`** · dualn **`3`**. ASI **212992** B · SHA256 `B3061A5C…`.<br>
**Mode 120 / 130 / 131 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode132_20260727-155128`.<br>
**User:** better than 131, but **still not desktop performance** → superseded by **133**.

### Mode 131 = FAILED (HMD starve)

User: desktop **super smooth**, headset **hardcore freezing**. Cause: Mode131 EndScene BB→L+R only every **3rd** mono frame + POSEHOLD-CALM → HMD starves while Present stays ~90 (same class as Mode122 HOLD snap). Interim before 132: stereo **130** + dualn **3**.

### Mode 132

Monolook (identical BB both eyes) like 130/131. Keep 131 pitch-stable: pitch≥**2.5°**, sustain **3**, hyst **600ms**. StretchRect BB→L+R **EVERY** EndScene during mono (no every-3rd). Calm: dualn=**3** HOLD last stereo. Pose stall → HOLD last stereo (re-submit each Present).

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| **130** | monolook interim (ok) | 3 |
| **131** | **FAILED** HMD starve | 3 |
| **132** | HMD-every monolook (prior) | **3** |

---

## 2026-07-27 ~15:42 — Mode **131** pitch-stable monolook + FPS (FAILED)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode131-pitch-fps` · OpenVR **v2.12.14**.<br>
**Live boot was:** stereo **`131`** · dualn **`3`**. ASI **211968** B · SHA256 `ED23745D…`.<br>
**Mode 120 / 130 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode131_20260727-154238`.<br>
**Result:** **FAILED** — HMD freeze / starve (BB every 3rd). Superseded by **132**.

### Mode 130 feedback → 131

Ghosting **better** ✓. FPS **strong drops** again (live perf: avg **79** / dips&lt;75 **25/81**; BAD windows dual≈0, LIVELOOK StretchRect counter **~5100**). Looking **jumpy**, especially **pitch** (Mode130 pitch thresh **1.0°** &lt; yaw **1.25°**; 350ms hyst; pitch-only mono triggers).

### Mode 131 (what it tried)

Keep monolook (identical BB both eyes on look). Pitch≥**2.5°** + **3-frame sustain** before enter; ≥**600ms** hyst. EndScene BB→L+R only every **3rd** mono frame. Calm pose → **POSEHOLD-CALM** last stereo (not mono StretchRect). dualn=**3**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–130 | prior (unchanged) | … |
| **131** | **FAILED** pitch/FPS monolook | **3** |

---

## 2026-07-27 ~15:35 — Mode **130** monolook sync (prior)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode130-monolook-sync` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`130`** · dualn **`3`**. ASI **210432** B.<br>
**Mode 120 / 129 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode130_20260727-153000`.<br>
**Log proof:** `ASI_BUILD_ID 20260727-mode130-monolook-sync`, `StereoMode: 130`, `CLEAN MONOLOOK`, `MONOLOOK-HYST`, `EndScene LIVELOOK BB→both`, `DRAWSCENE dual` calm, `everyN=3` — **no** `LOOK-FORCE-DUAL`. `VR_Init OK`.

### Why Mode 129 failed (log-proven)

LOOK-FORCE-DUAL **did fire** during look (n=1…360+). AppFPS **avg≈89.6 / med 90**, dual:hold ~1:1.8 (dualn=3).<br>
**Root cause:** “same-tick dual” is still **sequential** `DrawScene L → DrawScene R` with camera/IPD change while **game time / streaming advances between eyes** → L/R temporally split → diplopia (“swim”) on head turn. Force-dual every look frame does not create true simultaneous L/R.<br>
Other causes ruled out for this symptom: submit `swap=0`, sep≈7cm, canvas UV is per-eye frustum placement (expected), stereoscale 135 unchanged. No softskip/time-freeze hook in tree — skipped.

### Mode 130

On look (yaw/pitch Δ): **identical** BB→L and R (one capture) via EndScene LIVELOOK path; **≥350ms hysteresis** after settle before stereo returns (no frame-by-frame mono↔stereo flip). Calm: dualn=3 HOLD like 128/129 (~90). Pose stall extends mono hyst.

**Expect in log:** `StereoMode: 130`, `CLEAN MONOLOOK`, `MONOLOOK-HYST` — **no** `LOOK-FORCE-DUAL`.

**Test (DE):** Desktop **GTA IV Quick Restart** → look around (**no double?**) · stand still (**stereo + ~90?**). Kill: stereo **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–129 | prior experiments (unchanged) | … |
| **130** | **monolook sync** (live) | **3** |

---

## 2026-07-27 ~15:23 — Mode **129** L/R sync (prior)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode129-lrsync` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`129`** · dualn **`3`**. ASI **209408** B · SHA256 `8BF1E66B…`.<br>
**Mode 120 / 128 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode129_20260727-152322`.<br>
**Log proof:** `ASI_BUILD_ID 20260727-mode129-lrsync`, `StereoMode: 129`, `CLEAN L/R SYNC`, `VR_Init OK`.

### Mode 128 eval → 129

**Stutter mostly fixed** (perf.csv, no FPS HUD): AppFPS **avg≈90 / med 90**, pose_ms **≈9**, pose_stall20 ≈0, dual:hold **1:2** (dualn=3).

**Strong ghosting / double-image on look:** Mode128 `dualn=3` HOLD + `POSEHOLD-STEREO` submits **different-time** L vs R while head moves → classic temporal stereo desync. Not LIVELOOK (removed in 128).

**Mode 129:** look → **force same-tick dual**; calm → dualn=3 HOLD; high pose → **mono BB ≥200ms hyst** (no mismatched HOLD pair).

**Expect in log:** `StereoMode: 129`, `CLEAN L/R SYNC`, `LOOK-FORCE-DUAL`, `POSEMONO-HYST` — **no** `POSEHOLD-STEREO`.

**Test (DE):** Desktop **GTA IV Quick Restart** → look around (less double?) · still ~90 when calm? Kill: stereo **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–128 | prior experiments (unchanged) | … |
| **129** | **L/R sync** (live) | **3** |

---

## 2026-07-27 ~14:55 — Mode **128** no LIVELOOK (prior)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode128-nolivelook` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`128`** · dualn **`3`**. ASI **207872** B · SHA256 `6FEBAD92…`.<br>
**Mode 120 / 127 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode128_20260727-145500`.

### Mode 127 eval → 128

LIVELOOK (mono BB→both eyes on yaw/pitch) caused **ghosting**. Mode **128** keeps 127 fill + tight POSEHOLD (≥12ms/5, HARD≥24ms/8) + dualn=3 + hitch 32, but:
- **Removes** look-rate LIVELOOK
- Pose stall → **HOLD last stereo** (skip dual only; no mono/stereo flicker)

**Expect in log:** `StereoMode: 128`, `CLEAN FPS NO-LIVELOOK`, `POSEHOLD` / `POSEHOLD-STEREO`, `everyN=3` — **no** `LIVELOOK` / `BB→both eyes`.

**Test (DE):** Desktop **GTA IV Quick Restart** → look around — less ghosting? FPS still ~90? Kill: stereo **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–127 | prior experiments (unchanged) | … |
| **128** | **FPS no LIVELOOK** (live) | **3** |

---

## 2026-07-27 ~14:38 — Mode **127** + auto log archive (deployed)

**Tree:** `gtaiv-dxvk-vr-clean`.<br>
**Buildid:** `20260727-mode127-logarch-fps` · OpenVR **v2.12.14**.<br>
**Live boot:** stereo **`127`** · dualn **`3`**. ASI **206848** B · SHA256 `4BF05704…`.<br>
**Mode 120 / 126 code paths untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode127_20260727-143742`.

### Log archive (infra — all modes)

On ASI init (`LogInit` / `PerfDebugInit`), if a session file is **non-empty**, it is renamed before truncate:

| Live file | Archive name |
|-----------|--------------|
| `gtaiv_dxvk_vr.log` | `….log.m{mode}.{YYYYMMDD-HHMMSS}` |
| `gtaiv_dxvk_vr.perf.csv` | `….perf.csv.m{mode}.{…}` |
| `gtaiv_dxvk_vr.mem.log` | `….mem.log.m{mode}.{…}` |
| `gtaiv_dxvk_vr.vr.log` | `….vr.log.m{mode}.{…}` |

Mode peeked from `gtaiv_dxvk_vr.stereo` at init. Keeps **last 20** archives per base name. No FPS HUD.

### Mode 126 dip re-analysis (pre-127 session)

| Evidence | Finding |
|----------|---------|
| `perf.csv` AppFPS avg **68.4**, min **48.7**; dips&lt;75 = **70/102** | Still strong frame-dip stutter |
| Dips ↔ `pose_stall20` | **68/70** dips had stalls; **0/24** healthy (≥88) had stalls |
| `.vr.log` | **652** WaitGetPoses stalls, avg **21.4ms**, max **201ms**; HARD≥40ms only **4** |
| POSEHOLD / LIVELOOK | Soft POSEHOLD **31** (HARD **0**); LIVELOOK **61** (pose-budget 32 / look 7) |
| Verdict (DE) | **WaitGetPoses half-lock noch primär.** Mode **126** POSEHOLD (16ms/3) reicht nicht — Duals sinken, aber Pose-Wait ~22ms blockiert weiterhin FPS. |

### What Mode 127 does (vs 126)

| Piece | 126 | **127** |
|-------|-----|---------|
| Fill / LIVELOOK / dezoom | cover-match + live BB both eyes + Window0 + ss135 | **same** |
| POSEHOLD soft | ≥16ms → skip 3 duals | **≥12ms → skip 5** |
| POSEHOLD HARD | ≥40ms → skip 6 | **≥24ms → skip 8** |
| dualn | file/default 2 | **forced 3** |
| hitchcut | 40ms | **32ms** |

**Expect in log:** `StereoMode: 127`, `CLEAN FPS+LIVELOOK TIGHT`, `POSEHOLD`, `LIVELOOK`, `everyN=3`, `LogArchive: session start`, prior logs as `.m126.…` archives.

**Test (DE):** Desktop **GTA IV Quick Restart** → SteamVR → headset → gleiche Explore-Strecke wie 126 → NVIDIA overlay + `gtaiv_dxvk_vr.perf.csv`. Kill: stereo **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–126 | prior experiments (unchanged) | … |
| **127** | **FPS tight + LIVELOOK** (live) | **3** |

---

## 2026-07-27 ~14:07 — Mode **126** FPS fix + live look (deployed)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`**.<br>
**Buildid:** `20260727-mode126-fpsfix-livelook` · OpenVR **v2.12.14** / `IVRSystem_023`.<br>
**Live boot:** stereo **`126`** · dualn **`2`**. ASI **197632** B · SHA256 `7BBE45A7…`.<br>
**Mode 120 code path untouched.** Kill: **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode126_20260727-140702`.

### Log cause of 60–70 exploring dip (Mode 123 afternoon)

| Evidence | Finding |
|----------|---------|
| `perf.csv` AppFPS dips **62–70** | Correlated with `pose_stall20` rising (0 → 1–3 / 1.5s) + dual rate drop |
| Healthy windows AppFPS≥88 | `pose_stall20=0`, avg `pose_ms≈7.9` (normal 90Hz slot) |
| `.vr.log` | **40** WaitGetPoses stall lines, avg **22.4ms**, max **45ms** |
| Verdict | **WaitGetPoses half-lock** (~22ms ≈ 2× 11.1ms slot) during explore/stream load. Mode **123** had **no POSEHOLD**. Mode **122** POSEHOLD helped FPS but **stale stereo HOLD** snapped on look. |

### What Mode 126 does

| Piece | Behavior |
|-------|----------|
| Fill | Mode **123** cover-match (H≈100%; V bars OK; no SoftInset) |
| FPS | **POSEHOLD**: WaitGetPoses ≥16ms → skip next duals; ≥40ms → skip 6 (HARD) |
| Look | **LIVELOOK**: live BB → **both** eyes (fresh mono) — **not** Mode122 stale L/R HOLD |
| Dezoom | Start **Window0** (fovadd=0); cover refine + **stereoscale 135** |
| dualn | File/default **2** (not forced 4) |
| Never | SoftInset / LetterboxPrefer / FPS HUD / OpenVR bump |

**Expect in log:** `StereoMode: 126`, `CLEAN FPS+LIVELOOK`, `POSEHOLD`, `LIVELOOK`, `COVER-MATCH stereoscale=135`, `AppFPS` stays nearer **90** while exploring.

**Test (DE):** Desktop **GTA IV Quick Restart** → SteamVR → headset → roam same explore path as 123 stutter → NVIDIA overlay + `gtaiv_dxvk_vr.perf.csv`. Kill: stereo **`120`**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| 121–125 | prior experiments (unchanged) | … |
| **126** | **FPS+LIVELOOK** (live) | 2 |

---

## 2026-07-27 ~05:07 — Overnight prep Modes **122–125** (prep3 final)

**Tree:** `gtaiv-dxvk-vr-clean`. Guide: **`docs/SESSION_PREP_20260727.md`** · RE: **`docs/RE_STREAM_LOOK_POPIN.md`**.<br>
**Buildid:** `20260727-mode122-125-prep3` · OpenVR **v2.12.14** / `IVRSystem_023`.<br>
**Live boot:** stereo **`120`** · dualn **`2`** (LKG). Experiments via stereo file only.<br>
**FusionFix.cfg:** SkipMenu=1 SkipIntro=1 FieldOfView=0 kept.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG frozen** / kill | 2 |
| **121** | dualn=3 experiment | **3** |
| **122** | look+pose HOLD | **4** |
| **123** | cover fill-match | 2 |
| **124** | zoom MatchH6 default | 2 |
| **125** | 122+123 combo | **4** |

**Kill:** **`120`** + dualn **`2`**.<br>
**Backup:** `Documents\cursor\_vr_glue_backup_mode122_prep_*`.

---

## 2026-07-27 ~03:51 — CLEAN Mode **121** redeploy (OpenVR 2.12.14 rebuild)

**Live on game:** clean Mode **121** (not MAIN 88).<br>
**Why rebuild:** prior clean ASI was OpenVR **2.15.6** (`IVRSystem_026`) → SteamVR 2.12 would hit error **105**. Pinned `thirdparty/openvr` to **v2.12.14** (`IVRSystem_023`), rebuilt, deployed ASI + matching `openvr_api.dll`.<br>
**Sidecars:** stereo=`121` · dualn=`3` · buildid=`20260727-clean-121-redeploy`. FusionFix SkipMenu/SkipIntro kept. d3d9/dxvk.conf/dinput8 untouched.<br>
**ASI SHA256** `9ABDE9C3…` · openvr SHA256 `FD100F4F…` (same as MAIN 2.12.14).<br>
**Backup MAIN 88:** `Documents\cursor\_vr_glue_backup_clean121_redeploy_20260727-035100` (+ mode120-lkg / mode121 ASI copies). Mode120 LKG sidecar in `out-asi` untouched.<br>
**Fallback:** stereo `120`+dualn `2` (same ASI) or restore MAIN ASI from backup + stereo `88`+dualn `2`.<br>
**Test:** Desktop **GTA IV Quick Restart** → log `ASI_BUILD_ID 20260727-clean-121-redeploy`, `StereoMode: 121`, `VR_Init OK`.

---

## 2026-07-27 ~03:10 — Mode **121** dualn=3 experiment (120 frozen LKG)

**Tree:** `gtaiv-dxvk-vr-clean` only. **Mode 120 = last good smooth — do not mutate.**<br>
**121+ = experiments.** Buildid **`20260727-mode121-dualn3`**. Stereo **`121`**. dualn **`3`**.

### What Mode 121 is
Copy of Mode 120 path (DrawScene×2 + HOLD + look_move + F7 worldscale + perf logs, no FPS HUD) with **forced dualn=3** (lighter dual / more HOLD). Mode 120 still defaults dualn=**2**.

| stereo | Role | dualn |
|--------|------|-------|
| **120** | **LKG / last good smooth** — frozen kill/fallback | **2** (default) |
| **121** | Experiment (this build) | **3** (forced) |
| 51 / 45 / 0 | Harder kills | — |

### Go back to Mode 120
1. Write `120` into `gtaiv_dxvk_vr.stereo` (next to `GTAIV.exe`).
2. Write `2` into `gtaiv_dxvk_vr.dualn` (so leftover dualn=3 does not affect 120).
3. Restart GTA (Desktop **GTA IV Quick Restart**).

### Test (DE)
Restart → same hitchy area as 120 → compare FPS (NVIDIA overlay + `gtaiv_dxvk_vr.perf.csv` if debug=1).

**Kill:** **`120`** (good) / **`51`** / **`45`** / **`0`**.

**Deploy:** ASI **192000** B · SHA256 `F86173C4…` · sidecar `gtaiv_dxvk_vr.asi.mode121` (does **not** overwrite `mode120-lkg` if present). Backup `…mode120-pre-mode121`. ntfy sent.

---

## 2026-07-27 ~03:00 — Mode **120** F7 WorldScale MORE (dezoom direction FIXED)

**Tree:** `gtaiv-dxvk-vr-clean`. Buildid **`20260727-mode120-worldscale-more`**. Stereo **`120`**.

### Which knob actually dezooms (Mode 120 true-FOV canvas)

| Knob | Effect on “zoom” |
|------|------------------|
| **fovadd ↑** | Widens `gameTan` → StretchRect **SRC crop** when fill>100% → **zoom-IN** / giant-monitor. Live proof: fovadd=18 → fill≈**150%h**/~95%v on G2. Old Ultra28 was wrong direction. |
| **fovadd ↓** | Less H crop → more of BB / less “sitting in cropped center” → **DEZOOM**. H-match ≈ fovadd **6** (coverTan 1.16). |
| **stereoscale ↑** | Stronger disparity → **smaller world** feel (cap 130). |
| SoftInset / LetterboxPrefer / CanvasZoom | **Dead** — do not use. |

### F7 presets (10 steps) — persist `gtaiv_dxvk_vr.worldscale`

| Step | Name | fovadd | stereoscale | Feel |
|------|------|--------|-------------|------|
| 1/10 | CropMax | 28 | 115% | heaviest crop / zoom-IN (old Ultra) |
| 2/10 | Crop | 24 | 118% | still crop-heavy |
| 3/10 | TallFill | 20 | 120% | V near full; H ~155% |
| 4/10 | Mild18 | 18 | 120% | prior session (fill≈150%h) |
| 5/10 | Soft16 | 16 | 122% | easing crop |
| **6/10** | **Open12** | **12** | **125%** | **DEFAULT — further out than old Out22** |
| 7/10 | Room8 | 8 | 128% | near H-match |
| 8/10 | MatchH6 | 6 | 130% | ~100%h cover |
| 9/10 | Air4 | 4 | 130% | slight under-fill |
| 10/10 | Window0 | 0 | 130% | no fovadd; max stereo (V bars possible) |

Also writes `fovadd` + `stereoscale`. Live F7. No SoftInset. RT lock / dual+HOLD / look_move / no FPS HUD / light perf logs kept.

**Deploy:** ASI **192000** B · SHA256 `CF01EEEB…` · sidecar `gtaiv_dxvk_vr.asi.mode120-worldscale-more` · backup `…mode120-pre-worldscale-more`. ntfy sent. worldscale index **5** (Open12).

**Kill:** stereo **`51`** / **`45`** / **`0`**.

**Next (DE):** F7 until comfortable; report which **# Name** felt right; watch `fill≈` in log.

---

## 2026-07-27 ~02:43 — Mode **120** sampled perf/mem logs (stutter probe)

**Tree:** `gtaiv-dxvk-vr-clean`. Buildid **`20260727-mode120-perflog`**. Stereo **`120`**. `gtaiv_dxvk_vr.debug=1` (default ON).

### Why
Prior session log (`20260727-clean-3dmove-opt`, ~40 min, MonoSubmit ≈#39800) showed Mode120 dual everyN=2 + HOLD, Submit errL=errR=0, **no** Hitch/AppFPS/WaitGetPoses-ms/mem samples — stutter cause not in main log. System RAM ~83% of 16 GB (~2.8 GB free) with Cursor/SteamVR/steamtours/Discord/Spotify already up.

### New files (next to EXE, sampled ~1.5–2s — no FPS HUD)
| File | Contents |
|------|----------|
| `gtaiv_dxvk_vr.perf.csv` | app_fps, pose_ms, dual/hold/hitch_skip, pose_stall≥20ms, draw_gap/max_hitch |
| `gtaiv_dxvk_vr.mem.log` | GTAIV WorkingSet + PrivateBytes |
| `gtaiv_dxvk_vr.vr.log` | rare WaitGetPoses/Submit errors + ≥20ms stalls |

Toggle off: write `0` into `gtaiv_dxvk_vr.debug`.

**Deploy:** ASI 191488 B · sidecar `gtaiv_dxvk_vr.asi.mode120-perflog` · backup `…mode120-pre-perflog`. ntfy sent.

**Next:** play until mid-session stutter returns → share/open `.perf.csv` + `.mem.log`.

---

## 2026-07-27 ~02:40 — Mode **120** + F7 WorldScale (clean tree)

**Tree:** `gtaiv-dxvk-vr-clean` only. Main `gtaiv-dxvk-vr` = reference.

### Mode 120 3D vs main folder (short)

| | **Main** (Mode74–111) | **Mode 120** (clean) |
|--|--|--|
| 3D path | Spaghetti: EyeProj, canvas UV, FocusPace, tight guards, remaps, FPS HUD | Synced **DrawScene×2** everyN + **HOLD** off-ticks |
| Look | mixed / pedCoupled experiments | **look_move** `atan2(-fx,fy)`; **pedCoupled OFF** |
| Guard | tight motion guards / StretchRect storms | **OFF** |
| Submit | AER / TextureWithPose variants | **WaitGetPoses + Submit** |
| HUD | ColorFill FPS digits | **NO FPS HUD** |
| Perf | often tanked | **#1** — smooth ~90 even driving |

### F7 visual WorldScale (this build) — SUPERSEDED by worldscale-more

| Step | Name | fovadd | stereoscale | Feel |
|------|------|--------|-------------|------|
| 1/6 | Tight | 0 | 115% | current-ish Mode120 (no FOV expand) |
| 2/6 | Mild | 18 | 120% | Mode37 comfort |
| **3/6** | **Out** | **22** | **125%** | **DEFAULT — deutlich weiter raus** |
| 4/6 | Far | 24 | 128% | smaller / more out |
| 5/6 | VeryFar | 26 | 130% | near max |
| 6/6 | Ultra | 28 | 130% | widest practical |

- Persist: `gtaiv_dxvk_vr.worldscale` (preset index 0–5)
- Also writes `fovadd` + `stereoscale`. Live (no restart). No SoftInset / LetterboxPrefer. RT lock kept.
- 6DoF lean still `gtaiv_dxvk_vr.scale` (not F7). F6 still fine-tunes stereoscale alone.
- Buildid: **`20260727-mode120-worldscale-f7`**
- ASI: **187904** B · SHA256 `A619D81E…` · sidecar `gtaiv_dxvk_vr.asi.mode120-worldscale` (LKG `mode120-lkg` untouched)
- Kill: stereo **`51`** / **`45`** / **`0`**
- **Correction:** higher fovadd was crop zoom-IN, not dezoom — see worldscale-more.

**Next:** headset — confirm Out default less giant; F7 cycle; keep ~90 FPS.

---

**Tree:** `gtaiv-dxvk-vr-clean`. Buildid **`20260727-mode120-perflog`**. Stereo **`120`**. `gtaiv_dxvk_vr.debug=1` (default ON).

### Why
Prior session log (`20260727-clean-3dmove-opt`, ~40 min, MonoSubmit ≈#39800) showed Mode120 dual everyN=2 + HOLD, Submit errL=errR=0, **no** Hitch/AppFPS/WaitGetPoses-ms/mem samples — stutter cause not in main log. System RAM ~83% of 16 GB (~2.8 GB free) with Cursor/SteamVR/steamtours/Discord/Spotify already up.

### New files (next to EXE, sampled ~1.5–2s — no FPS HUD)
| File | Contents |
|------|----------|
| `gtaiv_dxvk_vr.perf.csv` | app_fps, pose_ms, dual/hold/hitch_skip, pose_stall≥20ms, draw_gap/max_hitch |
| `gtaiv_dxvk_vr.mem.log` | GTAIV WorkingSet + PrivateBytes |
| `gtaiv_dxvk_vr.vr.log` | rare WaitGetPoses/Submit errors + ≥20ms stalls |

Toggle off: write `0` into `gtaiv_dxvk_vr.debug`.

**Deploy:** ASI 191488 B · sidecar `gtaiv_dxvk_vr.asi.mode120-perflog` · backup `…mode120-pre-perflog`. ntfy sent.

**Next:** play until mid-session stutter returns → share/open `.perf.csv` + `.mem.log`.

---

## 2026-07-27 ~02:40 — Mode **120** + F7 WorldScale (clean tree)

**Tree:** `gtaiv-dxvk-vr-clean` only. Main `gtaiv-dxvk-vr` = reference.

### Mode 120 3D vs main folder (short)

| | **Main** (Mode74–111) | **Mode 120** (clean) |
|--|--|--|
| 3D path | Spaghetti: EyeProj, canvas UV, FocusPace, tight guards, remaps, FPS HUD | Synced **DrawScene×2** everyN + **HOLD** off-ticks |
| Look | mixed / pedCoupled experiments | **look_move** `atan2(-fx,fy)`; **pedCoupled OFF** |
| Guard | tight motion guards / StretchRect storms | **OFF** |
| Submit | AER / TextureWithPose variants | **WaitGetPoses + Submit** |
| HUD | ColorFill FPS digits | **NO FPS HUD** |
| Perf | often tanked | **#1** — smooth ~90 even driving |

### F7 visual WorldScale (this build)

| Step | Name | fovadd | stereoscale | Feel |
|------|------|--------|-------------|------|
| 1/6 | Tight | 0 | 115% | current-ish Mode120 (no FOV expand) |
| 2/6 | Mild | 18 | 120% | Mode37 comfort |
| **3/6** | **Out** | **22** | **125%** | **DEFAULT — deutlich weiter raus** |
| 4/6 | Far | 24 | 128% | smaller / more out |
| 5/6 | VeryFar | 26 | 130% | near max |
| 6/6 | Ultra | 28 | 130% | widest practical |

- Persist: `gtaiv_dxvk_vr.worldscale` (preset index 0–5)
- Also writes `fovadd` + `stereoscale`. Live (no restart). No SoftInset / LetterboxPrefer. RT lock kept.
- 6DoF lean still `gtaiv_dxvk_vr.scale` (not F7). F6 still fine-tunes stereoscale alone.
- Buildid: **`20260727-mode120-worldscale-f7`**
- ASI: **187904** B · SHA256 `A619D81E…` · sidecar `gtaiv_dxvk_vr.asi.mode120-worldscale` (LKG `mode120-lkg` untouched)
- Kill: stereo **`51`** / **`45`** / **`0`**

**Next:** headset — confirm Out default less giant; F7 cycle; keep ~90 FPS.

---

## 2026-07-27 ~02:15 — Mode **120** clean 3D+lookmove (opt-first) on `01f355b`

**Tree:** `gtaiv-dxvk-vr-clean` @ `01f355b` + Mode120 rewrite (main tree = reference only).

| Item | Value |
|------|--------|
| Mode | **120** `CleanDualLookMove` (default deploy) |
| Buildid | **`20260727-clean-3dmove-opt`** |
| ASI | 186880 B · SHA256 `E864976B…` · **no FPS HUD / ColorFill digits / fpshud** |
| LKG | `out-asi/gtaiv_dxvk_vr.mode120-lkg.asi` + game `gtaiv_dxvk_vr.asi.mode120-lkg` |
| 3D path | Synced **DrawScene×2** every `dualn` (default **2**) + **HOLD** off-ticks + hitch skip — **not** AER Mode51, **not** BuildRootA×2 |
| Look-move | **ON** (`lookmove=1`); ped heading = HMD yaw via **`atan2(-fx, fy)`**; **pedCoupled OFF** |
| Motion guard | **OFF** |
| Kills | stereo **`51`** / **`45`** / **`0`** |

### User finding — session/GPU accumulation (document)

After a **PC restart**, FPS is fine again. Something accumulates across ASI swaps / SteamVR / GPU sessions and tanks frames until reboot — **not** only a dirty leftover ASI. If FPS dies mid-session after several deploys: **reboot PC first**, then re-test before blaming new Mode120 code. Cold start = full quit GTA + prefer fresh SteamVR after heavy thrash.

**Next:** headset check Mode120 vs kill 51; NVIDIA overlay only (no in-game FPS HUD).

---

## 2026-07-27 ~01:48 — VIRGIN GitHub rebuild (clean tree only)

**Prior `gtaiv-dxvk-vr-clean` tree abandoned** (local patches / Mode 110–111 / FPS HUD /
look_move leftovers suspected of poisoning perf tests). Folder deleted + worktree pruned.
**Fresh clone only** from GitHub:

| Item | Value |
|------|--------|
| Path | `C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr-clean` |
| SHA | **`01f355b`** (`origin/master`) |
| Branch | **`clean-rebuild`** @ exact tip — **zero local patches** |
| Build | Win32 ASI OK · buildid **`20260727-github-fresh-01f355b`** |
| Deploy | ASI **183296** B · SHA256 `6422C2C4…F2CF` · `openvr_api.dll` |
| Config | **`gtaiv_dxvk_vr.stereo` = `51`** only (+ buildid). No fpshud / lookmove / fovadd / mode111 ASI backups |
| Binary proof | **No** `FpsHud` / `fpshud` / `AppFPS` strings in deployed ASI |
| Kept | Stock `d3d9.dll` + `dxvk.conf` untouched |
| Backup | Game glue → `_vr_glue_backup_20260727-014831\` |
| Main tree | **Not touched** (`gtaiv-dxvk-vr` messy worktree left alone) |

**Honest:** this is virgin GitHub tip Mode **51** (the ~90 FPS reference on exact GitHub).
Old clean experiments are gone; do not mix Mode 110/111 / HUD code back in.

**Cold start required** (full quit → SteamVR → start GTA). See test steps below in chat.

---

**As of:** 2026-07-24 ~22:18 (historical tip content at `01f355b`)
**Deployed now:** stereo **`51`** (Mode-50 always-distinct L/R + **`Submit_TextureWithPose`**
AER) + **`fovadd=18`**, **`ipd=3`**, scale **`100`**, stereoscale **`125`**. Mode **50**
(motion-guard OFF baseline), **53** (soft guard: 8°/6cm AER-only, 15°/12cm hard mono), and
**52** (swap-eyes depth test) are built and freeze-tested. Mode **49** remains the full
motion-guard stability baseline. Mode 46 stays disabled (post-load freeze). Mode 45/48 remain
kill baselines.
**Hotkeys split:** **F9** = view recenter only; **F10** = 6DoF translation origin reset.
F6=stereo scale, F7=WorldScale, F8=IPD.
**Root cause found (Mode 49 “no 3D”):** Mode-40 motion guard (≥1.5° / ≥2 cm between L/R capture
poses) fired constantly on temporal pairs and copied the **same R frame to both eyes** → flat image
even with correct IPD. Mode 50+ disables that guard; Mode 53 adds optional soft guard at much
higher thresholds only.
**Kill:** stereo **`49`** (motion-guard stability), **`50`** (no AER), **`45`**, **`48`**, **`44`**, **`37`**, or **`30`**
+ delete **`fovadd`**. Protected: **`36`/`35`**. Keep **`ipd`≥1**, canvas **zoom DISABLED**.
**Expectation:** Real temporal parallax with AER reprojection comfort on rapid turns. Log proof:
`motionGuard=0`, `StereoAudit avgDiff≈11000+` at ipd=3, `zeroRate≈1%`, distinct `CamMatrix ipd`
L vs R (±0.016 at 3 cm), `AERPose submitWithPose=1 err=0`, **no** `MOTION-GUARD mono pair`
lines. Headset must confirm depth vs Mode 49/50. Kill to **`50`** or **`49`** if jump worse.

### Session 2026-07-24 ~22:18 (Mode 51 AER + ipd=3 + Mode 53 soft guard — shipped)

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

**User:** Mode 37 still huge/monitor, jump, bad FPS; F7 doesn’t feel like being in the world; read Luke AER v2 Patreon.<br>
**Takeaway:** AER v2 = optical-flow intermediates (skip). Transferable = stamp each eye with capture-time pose (`Submit_TextureWithPose`, OpenVR #1253).<br>
**Shipped Mode 38:** Mode 37 path + pose cache on L/R capture + pose promote with pair-hold + `Submit_TextureWithPose`. Fallback to `Submit_Default` if pose missing/err.<br>
**Deploy verified (`ASI_BUILD_ID 20260724-191333`):** `StereoMode: 38`, `mode 38 AER-POSE SUBMIT … ok=1`, pair promote `poseL=1 poseR=1`, `AERPose: eye=L/R submitWithPose=1 err=0`, `StereoSubmit … mode=38`. Kill **37** / **30**.<br>

### Session 2026-07-24 ~18:45–19:00 (Mode 37 comfort — Mode36 bars-gone + Mode30 smooth)

**User feedback (Mode 36):** bars completely gone (keep); still screen-on-face; **FPS much worse**; **jump/jitter very strong** (Mode 30 was smooth); world **too close/huge**. Continue.<br>
**Diagnose:** Mode 36 pair-hold path was intact. Jump/FPS from (1) fovadd=22 wide engine FOV + temporal edge disparity, (2) FOV wobble recreating 4 eye RTs every few frames, (3) scale=50 = huge 6DoF feel.<br>
**Shipped Mode 37:** same true-FOV canvas + Mode30 pair-hold; **fovadd=18**; canvas **maxDim=1536 LOCKED**; pair-latched rect tangents; publish gate ~2.5%; **WorldScale→100** (one size lever; IPD untouched). No CanvasZoom / no IPD×scale / no forbidden hooks.<br>
**Deferred:** independent VR cam; Mode 34 dual.

### Session 2026-07-24 ~18:25–18:45 (Mode 36 true-canvas FOV — fill HMD / kill bars)

**User feedback (Mode 35):** better presence / less warp, still screen-on-face + black bars; protect as baseline; push harder.<br>
**Root cause:** engine `fovadd` wrote CCam+0x60, but canvas still used stale `D3DTS_PROJECTION` tangents (Rage ignores that transform — Mode 15 dead) → bars stayed.<br>
**Shipped Mode 36:** Mode 35 pair-hold + FOV site + `PublishGameFovFromCCamDegrees` (Mode 16 factor 58.7/45) → canvas/rect track TRUE FOV. No CanvasZoom.<br>
**Also fixed:** `ParseModeFile` capped at 35 (file `36` became mode 3!); FOV ADD idempotent (no 67→89 compound); ADD only when base FOV ≤55° (skip cutscene/menu spikes).<br>
**Deployed:** stereo=`36`, fovadd=`22` (toward G2 ~94°; vertical fill ~90%). Mode **35** unchanged as kill/protect.<br>
**Headset check:** smaller/no bars? Still fused? No look-warp? If bad → stereo **35** or **30**. Try fovadd **18–25** if needed.

**Deferred:** independent VR cam; Mode 34 dual; soft TextureBounds fill; theater quad.

### Session 2026-07-24 ~18:10–18:25 (Mode 35 FOV recompute — research + spike)

**User ask:** feel inside the game with real 6DoF; Mode 30 fusion OK but warping + screen-on-face.<br>
**Research:** Luke Ross / Halo / L4D2 / UEVR / FusionFix — kill monitor-on-face with **true engine FOV**, not canvas zoom / theater. FusionFix `fixes.ixx` = cam process CALL then `*(CCam+0x60) += n*5` = our Mode 17 recompute site.<br>
**Shipped Mode 35:** Mode 30 pair-hold + chain-hook that CALL; `gtaiv_dxvk_vr.fovadd` ADD degrees (deployed **15**). Additive preserves aim zoom.<br>
**Log proof:** 45→60 stable; thousands of FovSite calls; submits mode=35; game stayed up.<br>
**Headset check:** less black-bar / monitor feel? No look-warp? If warp/bad → kill to 30 + delete fovadd. Keep FusionFix menu FOV at **0**.

**Deferred:** independent VR cam vs wall collision; Mode 34 dual; theater quad.

### Session 2026-07-24 ~17:50–18:05 (kill canvas zoom + research)

**Headset feedback:** `zoom=85` claimed-FOV warp — every head move warps environment (same class as FusionFix FOV look-up warp).<br>
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

**Mode 34 RESULT (buildId `20260724-164125`):**<br>
Goal: when HookSetVSConstF ret==`0x2C73E`, resolve stack → **function starts**, CC-pad thiscall0, count ~1×/frame, dual+pair-hold. Fail → stereo=30.<br>
- EndScene brace bug (Mode34 nested under Mode33) fixed mid-session.
- `Mode34: DISCOVER armed … rareAgg=24 sampleHits=1392 vsRetHits=276`
- Rare starts with avg≈1.0 e.g. `0x52E7C0`, `0x5303D0`, `0x4DDAD0`
- `Mode34: COUNT exeRva=0x1BF010` (thiscall-frame) → **process hard-kill** (no SEH FALLBACK write)
- Hardened after: forbid `0x1BF010`, reject `thiscall-frame`, prefer avg∈[0.8,2.5] closest to 1.0
- Dual **not armed**. Next Mode34 try only with hardened filter; kill → **30/26**.

### “No 3D” on Mode 30 — ROOT CAUSE (2026-07-24 ~16:00)

Pair-hold was **not** broken: L/R eye flip + `StereoPairHold: promoted pair` + `StereoSubmit L=1 R=1 mode=30` all healthy.<br>
Live log had **`sep=0cm`** because `gtaiv_dxvk_vr.ipd` = **`0`** (F8 preset included 0).<br>
CamMatrix with ipd=0 → `ipd=(±0.000…)` → flat image, comfort intact.<br>
**Fix:** restored IPD; F8 presets no longer include 0 (now start at **1**). Log warn if file=0.

### CE vs version downgrade (verdict)

**Stay on CE.** Classic/1.0.7–1.0.8 + VorpX/C06alt FP offsets would not give us a replay-thread draw walker: those mods are cam/FP/ScriptHook (and often SBS cinema), not Rage dual-draw. Downgrade would invalidate FusionFix + all CE AOBs/RVAs we already mapped (Mode 18–26), force a parallel glue rewrite, and still leave the same “find the VsRet walker” problem on a different binary. Singleplayer/offline OK, but not worth a dedicated spike unless CE same-frame is abandoned. **Full steam ahead on CE.**

> **Planning update (2026-07-23 PT, no new headset result):** Repository baseline is
> `8fe072a`, which contains modes 18-24. Mode 24 is built but still needs its headset
> decision test. Quest 3/OpenXR is now the post-v0 primary target through a separate
> x64 host; the existing OpenVR/Reverb G2 path remains the fallback. See
> [`FULL_VR_PLAN.md`](FULL_VR_PLAN.md).

> **Gate 2 headset result (2026-07-24 PT): PASSED for OpenXR presentation.**
> `gtaiv_xr_host.exe` is verified x64 (`PE 0x8664`) against pinned Khronos OpenXR
> SDK `release-1.1.61`. Meta reported runtime `Oculus 1.205.0`, system
> `Meta Quest 3`, RTX 4070 Super LUID `0000000000014e12`, and two 2368x2576
> three-image swapchains. The headset run reached
> `READY -> RUNNING -> SYNCHRONIZED -> VISIBLE -> FOCUSED`, displayed distinct
> left/right calibration output, submitted 900 projection frames, and shut down
> cleanly. The user correctly reported no GTA image: this gate renders generated
> calibration pixels only. The unchanged OpenVR ASI also regression-builds as x86
> (`PE 0x14C`). At that time the next gate was the fixed x86/x64 shared ABI and
> log-only pose flow; the 2026-07-26 update at the top supersedes that planning note.

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

**Currently deployed:** stereo=**`30`**, ipd=**`1`**, eyefwd=**`20`**, pedhide=**`1`**, zoom=**OFF**.<br>
**Mode 30 (2026-07-24):** PAIR-HOLD = Mode 14/26 CCam temporal, but submit textures<br>
update **only when a complete L→R pair is ready** (both eyes promote together).<br>
User: jumps **definitely less**, barely noticeable — keep as playable. With ipd≥1: try fusion.<br>
Log: `StereoPairHold: promoted pair #N`, `StereoSubmit … mode=30`, `CanvasZoom: DISABLED`, `sep=1cm`, `eyeFwd=20cm`.

**Mode 34 RESULT (2026-07-24) — dual armed then DEAD for parallax:**<br>
VsRet(`0x2C73E`) stack → fn starts; DISCOVER armed; COUNT `0x4DDAD0` avg=2.0 → DUAL armed;<br>
live `vsPatch=0` / `vsCallsR=0` for 40k+ frames (no parallax). Earlier COUNT `0x1BF010` hard-kill.<br>
**Hardened:** forbid `0x4DDAD0` + `0x1BF010`; **never arm Dual** (COUNT-ok → stereo=30). Kill → **30** or **26**.

**Mode 33 RESULT (2026-07-24) — wait worked, dual not armed:**<br>
Goal: WAIT until live VsParent samples (Mode32 Soft was empty), then only CC-pad<br>
zero-arg thiscall → count → dual+VS. Never `0x2C6AC`/`0x37BD0`/`0x32A40`/`0x372B0`.<br>
Fail → write stereo=30.

Log quotes (buildId `20260724-161039`):
- `StereoSep: 6 cm (file …)` / `mode 33 … ok=1`
- `Mode33: still waiting … es=60 rareAgg=0` (correct — no early seed)
- `Mode33: VsParent sample#1 slot=10 exeRva=0x22187` (+ `0x37520`/`0x32BD7`/`0x4D901B`)
- `Mode33: DISCOVER armed (live VsParent appeared; rareAgg=4 sampleHits=8; no early seed)`
- Mid bytes prove **epilogues**, not walkers: `0x22187=5F C2 10 00` (pop/ret 0x10),<br>
  `0x32BD7→0x32A40` REJECT forbidden stackarg, `0x37520→0x372B0` REJECT forbidden
- `Mode33: COUNT exeRva=0x4D8F10` → `avgEntries=0.31` REJECT
- `Mode33: FALLBACK pair-hold — wrote stereo=30`
- After: `StereoSubmit: L=1 R=1 mode=30` (alive). F8 no longer cycles to 0.

**Hard finding:** VsParent slots 10–32 are **return addresses inside callees** (epilogues /<br>
mid-instr), not ~1×/frame thiscall0 draw walkers. Next same-frame spike must find the<br>
**caller** that drives VsRet ~1×/frame (static E8 / deeper stack), not re-hook these mids.

**Mode 32 RESULT (earlier) — safe fallback, dual not armed:**<br>
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

**Next session:** headset-check Mode **30** + **ipd=1** + zoom OFF (no warp?). Stay on **30**.<br>
Do not re-arm Mode34 dual. Use `VsRetStatic` safe RVAs for next same-frame COUNT-only probe.<br>
Playable stays **`30`** + **`ipd=1`** + **`eyefwd=20`** + **`pedhide=1`**. Kill stereo → **`26`**.

**Mode 31:** discover failed (`frameSlots=0`, callee too deep). Superseded by Mode 32/33.

**Mode 30 dual probe (earlier) — DEAD for parallax:**<br>
device-VS dual `deviceVsPatches=0`. Do not re-arm without replay-thread evidence.

**Mode 29 FAILED (2026-07-24):** `FindFnStartNear(Cand37C01)` → `0x37BD0` crash.<br>
Modes **27/29** alias Mode 26. **Mode 28:** wrap@0x2C6AC crashed — aliases 26.<br>
**SteamVR dashboard:** `restart-gtaiv.ps1` closes it after GTAIV is up (default).<br>
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

- Mid-body (survives FusionFix SafetyHook):<br>
  `83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C`
- Function start = mid − **13**
- File-offset reference (exe): mid ~`0x527EDE` → start ~`0x527ED1`<br>
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

Mode 9 FPS hole: AOB on every phase call — fixed.<br>
**Mode 10:** Execute-only dual → L≠R, F8 sep had no effect. Build+Exec from ExecA = freeze — forbidden.<br>
**Mode 11:** Build+Exec from PhaseA build → **black screen** — forbidden.<br>
**Mode 13 spike:** temporal stereo + **native-FOV inset** Submit bounds (no D3D cover widen).<br>
Cover-crop without cover render caused zoom; Mode **0** = daily ~90 FPS. Mode 11 dead. Kill **0**.<br>
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

**Mode 31 — SameFrameReplayDual (2026-07-24):** discover failed (stack too deep).<br>
Superseded by Mode 32/33. Kill → **`30`** or **`26`**.

**Mode 32 — SameFrameVsParentDual (2026-07-24):** HookSetVSConstF-depth VsParent +<br>
ABI classify + count gate. Soft empty → early seed. Dual not armed; wrote stereo=30.<br>
Kill → **`30`** or **`26`**.

**Mode 33 — SameFrameLateVsParentDual (2026-07-24):** WAIT for live VsParent samples,<br>
then CC-pad thiscall0 only. Wait **worked**; mids are epilogues not walkers;<br>
`0x4D8F10` count avg=0.31. Dual not armed; wrote stereo=30. See header.<br>
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
4. **OpenVR IPD raw values are correct:**<br>
   `L=(-0.030,…)` `R=(+0.030,…)` sepX≈60 mm.
5. **IPD via cam basis (right/up/−forward) was wrong** — right eye got negative X + large Y.<br>
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

- **F9** — view recenter (SteamVR zero + ped/veh heading baseline; 6DoF unchanged)<br>
- **F10** — 6DoF translation origin reset (lean/strafe anchor only)<br>
- **F6** — stereo scale · **F7** — WorldScale · **F8** — IPD cycle<br>
- SteamVR dashboard auto-closes on restart / after OpenVR init (manual close still OK)<br>
- Quick restart: `scripts/restart-gtaiv.*` (optional `-DirectExe` after RGLess)<br>
- Faster start/launcher: `docs/STARTUP_SPEED.md`<br>
- Inspiration (VC VR / C06alt): `docs/INSPIRATION_NOTES.md`<br>


- ntfy phone push on agent stop: topic `cursor-henning-atldv3m1iqosbzh2` @ `https://ntfy.sh`<br>
  Hook: `~/.cursor/hooks.json` → `hooks/ntfy-on-stop/run.cmd`

---

## Three BuildRenderList-like phases (CE exe, unique AOBs)

All share `cmp [edi+0x938], -1`, but different continuations:

| Label | File start | Mid AOB (unique) | Mid−start | Note |
|-------|------------|------------------|-----------|------|
| **DrawScene** | `0x6DC600` | `…FF 0F 84 DE 09 00 00 6A 00 6A 0C` | 13 | hooked so far |
| **PhaseA** | `0x527EC0` | `…FF 0F 84 76 03 00 00 80 3D` | 30 | large stack `0x6D8` → **SceneToGBuffer candidate** |
| **PhaseC** | `0x975D50` | `…FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | 39 | third phase |

RTTI strings in exe: `.?AVCRenderPhaseDrawScene@@`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@`.<br>
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

1. Natural: PhaseA + PhaseC with **Left** IPD<br>
2. DrawScene hook: DrawScene Left → StretchRect RT_L → PhaseA+C+DrawScene **Right** → RT_R<br>
3. EndScene: submit L/R when both RTs filled; otherwise mono fallback<br>

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

1. **Find GBuffer / execute** — not BB after BuildRenderList; RTs of deferred phase or real draw call.<br>
2. Per-eye **projection** (without old FOV-freeze path).<br>
3. Optional: Mode 6 for debug only; daily use = Mode **0**.

Stereo success criteria: parallax **and** fusion, acceptable FPS.

---

## Quick start checklist

1. SteamVR on, dashboard closed<br>
2. `gtaiv_dxvk_vr.stereo` = `0`<br>
3. Start GTA → log: `CamMatrix: 4/4 hooked`, `armed after 360`, `MonoSubmit … errL=0`, **no** `StereoTemporal`<br>
4. Expect FPS ~90<br>

Kill switch on freeze: kill game → set stereo file to `0` → restart.
