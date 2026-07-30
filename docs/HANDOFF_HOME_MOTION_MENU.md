# HOME HANDOFF — Motion controls + menu integration (prepared offline 2026-07-30)

Branch: `feature/motion-controls`. Stereo baseline: **Mode 243** (kill 241 / 203 / 0).
Prepared on a work PC: **no game, no headset, no Cheat Engine, no Visual Studio.**
Nothing below is compiled or headset-verified. The first home build proves the compile.

Read with: `AGENTS.md`, `docs/CURRENT-STATE.md`, `docs/PREP_PACK.md`,
`docs/DECOUPLED_AIM_PLAN.md`, `docs/MENU_INTEGRATION.md`, `docs/SESSION_PROMPTS.md`.

---

## Kurzanleitung (Deutsch, für den Start zu Hause)

1. Ordner in Cursor öffnen, Branch `feature/motion-controls`.
2. Kurz `docs/PREP_PACK.md` lesen (Checkliste + was schon fertig ist).
3. Terminal: `.\scripts\build-asi.ps1` — muss ohne Fehler durchlaufen. Neu:
   `vr_input.cpp`, `menu_bridge.cpp`, `ik_aim.cpp`. Bei Fehlern: nur Signaturen
   reparieren, keine Logik ändern.
4. `.\scripts\deploy-asi.ps1 -GameDir "…\Grand Theft Auto IV\GTAIV"`.
5. Configs aus `config/home-dropin/` kopieren (README dort). Session 1:
   `stereo=243`, `aimmode=2`, `vrinput=0`.
6. Spiel starten und **Session 1** testen: zentrieren, Wand, Stick 180°, nochmal
   schießen ohne Handbewegung. Treffer muss am Controller bleiben.
7. Danach Sessions 2→6 aus `SESSION_PROMPTS.md` (jeweils nur einen Prompt pasten).
8. Not-Aus: `aimmode=0`, `vrinput=0`, `aimyaw=0`, `ammogate=0`, `ikaim=0`,
   `ikprobe=0`, `stereo=243`.

Alles Weitere unten auf Englisch, damit es zum restlichen Repo passt.

---

## 1. Where motion controls actually stand

| Attempt | Build | Outcome |
|---|---|---|
| VrPad: OpenVR → synthetic XInput (sticks, grip=aim, trigger=shoot, menu=Start) | shipped | Works. G2 face buttons unreliable — the code guesses across `Reserved0/1` and DPad aliases because "Face B rarely reports 0x2000" |
| `TASK_AIM_GUN_AT_COORD` (Plan A) | Session 3 | Resolves and invokes, but the ped only aims side to side and ignores the controller → `kUseTaskAim = false` |
| `GET_PED_BONE_POSITION` native | Session 3 | ABI fails on CE (all FAIL in logs) → replaced by the `PedGetBonePos` thiscall |
| `FIRE_SINGLE_BULLET` + `FIRE_PED_WEAPON` + mouse LMB + VrPad RT | 20260730-004820 | Two or more bullets per trigger pull; two freezes after sustained fire |
| Horizon-flattened aim ray | earlier | Ignored controller pitch |
| Raw grip −Z as the barrel | 004820 | Shots go behind you |
| 55° grip→tip approximation | 20260730-010155 | Pitch works, but you still hunt for hits |
| Ped bone WRITE (`handfollow`) | — | HARD OFF, never validated |
| Weapon ghost via `CREATE_OBJECT` (`weapfollow`) | 20260730-003848 | Followed for ~1700 ticks, then freeze + crash → HARD OFF |

### Why it still feels wrong — five causes, named

0. **The aim ray was never rotated into the world.** This is the big one and it
   was found last. `cam_matrix.cpp` rotates the whole HMD basis by
   `GetControllerYawOffset() + ComputeVehicleYawOffset()` (plus the ped-coupled
   heading delta on modes ≥ 49) before it builds the camera. `aim_decouple.cpp`
   never did any of that — it fed `OvrToGta(controller forward)` straight to the
   natives. So the aim ray lives in raw tracking space while the view lives in
   world space, and the two are apart by exactly the amount you have turned with
   the right stick since recentring. Point straight ahead after a 180° stick
   turn and you shoot behind you. That is not a metaphor for the reported
   symptom, it is the literal mechanism, and it also explains why aim felt
   "sometimes fine" (right after a recentre, when the offset is zero) and why
   driving made it worse (vehicle follow adds to the same offset).
1. **The aim ray started at the head.** `ComputeAimPoint` took
   `GetLastStereoCamPos()` as the origin and added the controller forward × 25 m.
   Right direction, wrong origin: everything within a room is offset by the
   hand-to-eye distance, which is exactly the "must hunt hits / can shoot behind"
   report.
2. **The tip pose was a constant.** `kG2TipPitchDeg = 55` is a fixed guess for the
   grip→barrel rotation. It is close for one wrist posture and wrong for the rest,
   which is why a relaxed wrist aimed low.
3. **Natives ran per CopyMat, not per frame.** A dual-eye frame enters the CopyMat
   hook several times, so `GET_PLAYER_CHAR`, `IS_CHAR_IN_ANY_CAR` and the fire
   attempt all multiplied. That is the load profile behind the SEH-then-freeze
   signature in the 004820 log.
4. **We synthesise bullets instead of steering the gun.** `FIRE_PED_WEAPON` on a
   fixed 100 ms timer bypasses clip, reload, fire rate and recoil. Every symptom
   from "no reload" to "continuous fire freezes" follows from that one decision.

Cause 0 is the one to fix first because it is cheap, mechanical and certain.
Cause 4 is the strategic one: **the mod should decide where the gun points; the
game should decide when and how it fires.** Everything below is ordered toward
that.

---

## 2. What was written offline (compile-unverified, all off by default)

### Camera-yaw fix in `cam_matrix.cpp` + `aim_decouple.cpp`

Fixes cause 0. `cam_matrix.cpp` now records the total yaw it applied to the HMD
basis this frame (`GetLastCamYawOffset`), and `aim_decouple.cpp` rotates the
controller forward, the controller up, and the head→hand offset by the same
angle before handing them to the natives. The head→hand offset also picks up
`GetWorldScale()`, which it was silently ignoring.

This is the one change here that is **on by default**, because leaving it off
means the aim is knowably wrong. `gtaiv_dxvk_vr.aimyaw` = `0` restores the old
behaviour for a back-to-back comparison without rebuilding. The camera itself is
untouched — only the value it already computed is now published.

### `src/asi/vr_input.{h,cpp}` + `config/vr_actions/*.json`

SteamVR **IVRInput** action manifest layer. Fixes causes 2 and the G2 button
guesswork in one step:

* `/user/hand/right/pose/tip` is the real OpenXR aim pose, so
  `GetControllerAimPose` skips the 55° constant entirely when it is available.
* Buttons come from bound actions instead of legacy `ulButtonPressed` bits, so
  A/B/X/Y/menu report reliably on the G2 and are rebindable in SteamVR.
* Hand skeleton actions are declared but not consumed yet — they are the input
  for drawn VR hands later.

Gate: `gtaiv_dxvk_vr.vrinput` = `1`. Default `0` keeps the legacy path byte for
byte. If the manifest is missing or SteamVR refuses it, the code logs and falls
back to legacy instead of failing.

### `aimmode = 3` in `src/asi/aim_decouple.cpp`

Fixes causes 1 and 3. `aimmode = 2` is untouched so the 010155 baseline stays
reproducible side by side:

* ray origin becomes the controller's world position (`EstimateControllerWorld`)
  instead of the camera position,
* `TickAimFromGameThread` runs the natives exactly once per submitted VR frame
  (`AimDecoupleNotifyFrame` arms it from the submit path).

### `src/asi/menu_bridge.{h,cpp}`

Render-profile switching. `F4` or the right controller menu button cycles
`gtaiv_dxvk_vr.menumap` (default `0=243`, `1=53`, `2=0`), and if
`gtaiv_dxvk_vr.menupref` names a FusionFix preference, that pause-menu row drives
the same switch. Profile change also fires a short right-hand haptic pulse.
Full reasoning in `docs/MENU_INTEGRATION.md`.

### `src/asi/ik_aim.{h,cpp}` (Plan B scaffolding)

Writes `vecAimTarget` only when **both** `gtaiv_dxvk_vr.ikaim=1` and a verified
`gtaiv_dxvk_vr.ikoffs` file exist. Missing/bogus offsets → no memory writes.
`ikprobe=1` logs `CPed*` + candidate pointers for CE (see `CE_CLICK_RECIPE.md`).

### `ammogate=1` in `aim_decouple.cpp`

Stopgap for Session 4: skip `FIRE_PED_WEAPON` when clip reports empty; slower
fire interval. Does not replace Plan B — it only makes the synthetic fire less
abusive until the engine owns shooting again.

---

## 3. Home sessions — one behaviour change per build

### Session 0 — compile only (no headset)

```powershell
.\scripts\build-asi.ps1
```

Two files were never compiled. Expected trouble spots: OpenVR `IVRInput` types
need a reasonably recent `thirdparty/openvr`, and `menu_bridge.cpp` uses
`WriteStereoModeFile` / `ReloadStereoMode` from `stereo_config.h`. Fix signatures
only. Do not change behaviour to make it build.

### Session 1 — the yaw fix alone (do this before anything else)

Deploy, then next to `GTAIV.exe`: `aimmode` = `2`, `stereo` = `243`,
`vrinput` = `0`. Nothing else changes, so this is a clean read on cause 0 against
the exact 010155 baseline the headset already knows.

Headset check: recentre, fire at a wall, then **turn 90° and 180° with the right
stick** and fire again without moving your hand. Before the fix the impact walked
away from the controller as you turned; now it should stay put.

Log line to read — the `camYaw` field is new:

```text
AimOverride #7: FIRE ped=… target=(…) fwd=(…) via=1 tip=legacy+pitch camYaw=-38.4deg
```

`camYaw` should track how far you have turned with the stick. If it stays at
`0.0deg` while you turn, `GetLastCamYawOffset` is not being fed — that means the
active stereo mode does not run the camera-build path, and the fix cannot work
for that mode. Note which mode you were in and say so.

A/B without rebuilding: write `0` into `gtaiv_dxvk_vr.aimyaw`, restart, repeat
the 180° test. The log then ends with `(yawfix OFF)`.

Kill: `gtaiv_dxvk_vr.aimyaw` = `0`.

### Session 2 — IVRInput, nothing else

Deploy, then next to `GTAIV.exe`: `vrinput` = `1`, `aimmode` = `2`,
`stereo` = `243`.

Log must show, in order:

```text
VrInput: vrinput=1 (IVRInput action manifest)
VrInput: manifest loaded (…\gtaiv_dxvk_vr_actions.json)
VrInput: ACTIVE — tipR=1 tipL=1 gripR=1 gripL=1
VrInput: R trig=… grip=… stick=(…) aim=… fire=…
```

Headset check: move both sticks, press every face button, menu button, grips and
triggers. Every one should now register — that is the thing legacy input got
wrong. If `manifest loaded` appears but `ACTIVE` never does, SteamVR did not bind
the poses: open SteamVR → Settings → Controllers → Manage Controller Bindings,
pick "GTA IV VR", and confirm the custom binding is selected.

Kill: `vrinput` = `0`.

The FIRE log line now reads `tip=IVRInput` instead of `tip=legacy+pitch`. That is
the one-glance confirmation that the 55° constant is out of the path.

### Session 3 — controller-origin aim

Only change: `aimmode` = `3`. Keep `vrinput` = `1`.

Log: `AimDecouple: aimmode=3 (controller-origin ray + once-per-frame natives)`
and `AimOverride #N: FIRE … via=1`. The `#N` counter should now advance at most
once per frame, not several times.

Headset check, in this order:

1. Point at a wall 2–3 m away and fire. The impact should be where the controller
   points, not offset toward your head. This is the aimmode 2 → 3 difference.
2. Point at something ~25 m away. Should still land.
3. Point up, down, and across your body. No shots behind you.
4. Fire continuously for 20 s. Note whether the freeze returns.

Kill: `aimmode` = `2`, then `0`.

### Session 4 — stop synthesising bullets (the real fix)

Do not add more `FIRE_*` variations. Two builds:

1. **Ammo/rate coupling as a stopgap.** Resolve `GET_AMMO_IN_CLIP`; skip the fire
   call when the clip is empty, and drive the interval from the weapon instead of
   `kFireIntervalMs = 100`. This makes reload and fire rate behave and should end
   the freeze class on its own.
2. **Plan B — CPedIKManager `vecAimTarget`.** Recipe in
   `docs/DECOUPLED_AIM_PLAN.md` §5.4. Write the controller ray point there every
   frame while aiming and let the engine solve the arms. Once the arms track, drop
   `FIRE_PED_WEAPON` entirely and let VrPad's RT fire the stock weapon: clip,
   reload, recoil, sound and the weapon visual all become correct for free.
   The 1.0.7 hint is `CPed + 0xBC0` → `+0x70`; CE will differ, so it must be found
   with Cheat Engine before a single byte is written.

Do **not** revive the `CREATE_OBJECT` weapon ghost. Object lifetime against the
streamer is what crashed 003848, and Plan B makes it unnecessary.

### Session 5 — profile switching

Create next to the ASI:

```text
gtaiv_dxvk_vr.menumap     ->  two lines:  0=243   and   1=53
gtaiv_dxvk_vr.menupref    ->  PREF_LEDILLUMINATION
```

Test `F4` first — it needs no FusionFix at all. Log:
`MenuBridge: profile 1 -> stereo 53 (was 243 …)`. Then toggle the LightSyncRGB row
in the pause menu and confirm the same line appears.

If the log switches but the image does not, that mode family arms its hooks at
startup: the file is already written, so a restart lands in the new mode. Record
which pairs switch live and which need a restart — that decides whether an OSD is
worth building.

Kill: delete `gtaiv_dxvk_vr.menupref` and `gtaiv_dxvk_vr.menumap`.

### Session 6 — CE session (only when Sessions 1–4 are settled)

`docs/DECOUPLED_AIM_PLAN.md` §5.1–5.4, filling the §6 address checklist. Highest
value entry is #4 (CE `CPedIKManager` + `vecAimTarget`), because it unlocks
Session 4 step 2.

---

## 4. Paste prompt for the home agent (Grok 4.5)

```text
Read AGENTS.md, docs/CURRENT-STATE.md, docs/HANDOFF_HOME_MOTION_MENU.md,
docs/MENU_INTEGRATION.md, docs/DECOUPLED_AIM_PLAN.md.
Project: gtaiv-dxvk-vr — GTA IV CE VR, stock DXVK 3.0.2 d3d9.dll + gtaiv_dxvk_vr.asi
+ OpenVR/SteamVR, Win32/x86, Reverb G2. Branch feature/motion-controls.
I am not a programmer — give exact click/command steps and name the log lines to check.
Protect: Mode 243 stereo checkpoint (kill 241/203/0), Mode 14 geometry canvas,
no rejected FOV/canvas-zoom tricks, no forbidden replay RVAs (PLAN_NEXT.md).
New offline code, never compiled: the camera-yaw fix (cam_matrix GetLastCamYawOffset
+ ApplyCamYaw in aim_decouple, gate gtaiv_dxvk_vr.aimyaw, ON by default),
src/asi/vr_input.cpp (IVRInput action manifest, gate gtaiv_dxvk_vr.vrinput),
src/asi/menu_bridge.cpp (F4 / FusionFix pref render profile switch), aimmode=3 in
aim_decouple.cpp (controller-origin ray + one native tick per frame).
aimmode=2 must stay unchanged as the comparison baseline.
Start with Session 1 (yaw fix alone) — it is the suspected root cause of
"shots go behind me" and it is testable without touching anything else.
Today's goal: Session <N> from HANDOFF_HOME_MOTION_MENU.md §3 — <one line>.
One behavior change per build. Deploy with .\scripts\build-deploy-run.ps1.
After the headset test, update docs/CURRENT-STATE.md with kill values written down
BEFORE testing. I have Cheat Engine for the DECOUPLED_AIM_PLAN.md §5 recipes.
```

---

## 5. Config files added by this handoff

| File next to `GTAIV.exe` | Values | Default |
|---|---|---|
| `gtaiv_dxvk_vr.aimyaw` | `1` rotate aim ray by camera yaw / `0` old behaviour | **`1`** |
| `gtaiv_dxvk_vr.vrinput` | `0` legacy / `1` IVRInput action manifest | `0` |
| `gtaiv_dxvk_vr.aimmode` | `0` off / `1` probe / `2` baseline / `3` controller-origin | `0` |
| `gtaiv_dxvk_vr.ammogate` | `1` skip empty clip + slower FIRE | `0` |
| `gtaiv_dxvk_vr.ikprobe` | `1` log CPed* for CE | `0` |
| `gtaiv_dxvk_vr.ikaim` | `1` Plan B write (needs ikoffs) | `0` |
| `gtaiv_dxvk_vr.ikoffs` | two hex offsets | none |
| `gtaiv_dxvk_vr.menupref` | FusionFix pref name to follow | none |
| `gtaiv_dxvk_vr.menumap` | `value=stereoMode` per line | `0=243` `1=53` `2=0` |
| `gtaiv_dxvk_vr_actions.json` | SteamVR action manifest | deployed |
| `gtaiv_dxvk_vr_bindings_hpmotioncontroller.json` | G2 default bindings | deployed |

---

## 6. Do not touch

* Mode 243 / 241 / 203 stereo path, Mode 14 geometry canvas, the FOV/canvas-zoom
  rejects, and every forbidden replay RVA in `PLAN_NEXT.md`.
* The camera build in `cam_matrix.cpp`. The yaw fix only *reads* the angle the
  camera already applied. Do not "also fix" the unrotated 6DoF lean offset at
  `cam_matrix.cpp:799` while chasing aim bugs — it moves the viewpoint and would
  contaminate every aim measurement in Sessions 1–3.
* `aimmode = 2` behaviour — it is the comparison baseline for aimmode 3.
* `weapfollow` `CREATE_OBJECT` ghost and the ped bone WRITE path. Both stay hard
  off until Plan B replaces them.
* Native calls from raw EndScene. Game-thread sites only (CopyMat / PedHide),
  which is why `TickMenuBridge` sits next to `TickAimFromGameThread`.
* The FusionFix preference table. `menu_bridge` only *reads* through a native; it
  patches nothing. Writing to that table is the Session-6 stretch goal in
  `docs/MENU_INTEGRATION.md` §3, not a side quest.
