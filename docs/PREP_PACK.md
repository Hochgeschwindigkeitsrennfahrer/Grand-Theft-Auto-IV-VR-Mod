# Offline prep pack (work PC → home) — 2026-07-30

Everything below was written **without** VS / game / headset / CE. First home
action is still Session 0 compile.

## Master reading order at home

1. This file (checklist)
2. [`HANDOFF_HOME_MOTION_MENU.md`](HANDOFF_HOME_MOTION_MENU.md) — diagnosis + sessions
3. [`SESSION_PROMPTS.md`](SESSION_PROMPTS.md) — paste one block into Grok
4. [`CE_CLICK_RECIPE.md`](CE_CLICK_RECIPE.md) + [`ADDRESS_SHEET.md`](ADDRESS_SHEET.md) when doing CE
5. [`LOG_LINES.md`](LOG_LINES.md) — search strings only, no re-explaining
6. [`MENU_INTEGRATION.md`](MENU_INTEGRATION.md) if touching FusionFix prefs
7. [`DECOUPLED_AIM_PLAN.md`](DECOUPLED_AIM_PLAN.md) for Plan A–D theory

## What was prepared offline (token savers)

### Code (compile-unverified)

| File | Gate | Purpose |
|---|---|---|
| `cam_matrix` yaw publish + `aim_decouple` ApplyCamYaw | `aimyaw` default **1** | Fix "shots behind me" |
| `vr_input.cpp` + `config/vr_actions/*.json` | `vrinput=1` | Real tip pose + buttons |
| `aimmode=3` + `AimDecoupleNotifyFrame` | `aimmode=3` | Controller-origin ray, 1×/frame |
| `menu_bridge.cpp` + haptic pulse | F4 / menumap / menupref | 243 ↔ 53 ↔ 0 |
| **`ik_aim.cpp` (NEW)** | `ikaim=1` **and** `ikoffs` | Plan B write scaffolding |
| **`ikprobe` log (NEW)** | `ikprobe=1` | Prints `CPed*` + candidate ptrs for CE |
| **`ammogate` (NEW)** | `ammogate=1` | Skip FIRE on empty clip; slower cadence |

### Drop-in configs

Folder: `config/home-dropin/` — copy files into the GTAIV folder as needed.
See `config/home-dropin/README.txt`.

### Docs for zero rediscovery

- `CE_CLICK_RECIPE.md` — mouse clicks for Plan B / C / float3
- `ADDRESS_SHEET.md` — empty table to fill (Grok reads instead of re-scanning)
- `SESSION_PROMPTS.md` — one paste prompt per session
- `MENU_INTEGRATION.md` — why we did **not** hook the pause menu

## Home day-0 checklist (print / keep open)

```
[ ] git pull / open feature/motion-controls in Cursor
[ ] .\scripts\build-asi.ps1          → Session 0
[ ] .\scripts\deploy-asi.ps1 -GameDir "…"
[ ] Copy Session-1 files from config/home-dropin (stereo, aimmode=2, vrinput=0)
[ ] SteamVR on → game → recentre → 180° stick yaw test
[ ] Write kill values on sticky note: aimmode0 vrinput0 aimyaw0 ikaim0 stereo243
[ ] After each session: update CURRENT-STATE.md + ADDRESS_SHEET if CE
[ ] Never enable ikaim without CE-verified ikoffs on THIS build
[ ] Never revive weapfollow CREATE_OBJECT / ped bone WRITE
```

## Expected first log lines (healthy boot, Session 1)

```text
CamMatrix: FindPlayerPed @ …
AimDecouple: aimmode=2 …
AimOverride #1: FIRE … tip=legacy+pitch camYaw=….deg
```

Session 2 adds:

```text
VrInput: vrinput=1 …
VrInput: manifest loaded …
VrInput: ACTIVE — tipR=1 …
AimOverride … tip=IVRInput …
```

CE prep (`ikprobe=1`):

```text
IkProbe: CPed*=0x…
IkProbe: CPed+0xBxx -> 0x… (candidate ikMgr)
```

## Still needs home (cannot prep further offline)

- Compile / link errors against local OpenVR headers
- SteamVR binding UI confirmation on G2
- Real FusionFix pref name if `PREF_LEDILLUMINATION` is wrong on your install
- CE Structure Dissect for real `ikoffs`
- Whether stereo family switches live or needs restart
- GET_AMMO_IN_CLIP ABI on CE (ammogate falls open on EXCEPTION)

## Do not spend home tokens on

- Re-deriving why yaw was wrong (already in HANDOFF §1 cause 0)
- Re-researching FusionFix menu XML (MENU_INTEGRATION.md)
- Inventing bone WRITE / CREATE_OBJECT again
- Blind 1.0.7 `BC0 70` for ikaim
- Changing Mode 243 / Mode 14 / forbidden RVAs
