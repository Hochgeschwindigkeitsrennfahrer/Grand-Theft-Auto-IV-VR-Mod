# Paste prompts for home agent (Grok 4.5) — one per session

Copy the block for **today's session only**. Do not paste all sessions at once.

Shared preamble (prepend to every session):

```text
Read AGENTS.md, docs/CURRENT-STATE.md, docs/HANDOFF_HOME_MOTION_MENU.md,
docs/PREP_PACK.md, docs/MENU_INTEGRATION.md, docs/DECOUPLED_AIM_PLAN.md.
Project: gtaiv-dxvk-vr — GTA IV CE VR, stock DXVK 3.0.2 d3d9.dll + gtaiv_dxvk_vr.asi
+ OpenVR/SteamVR, Win32/x86, Reverb G2. Branch feature/motion-controls.
I am not a programmer — exact click/command steps; name the log lines to check.
Protect: Mode 243 (kill 241/203/0), Mode 14 canvas, no FOV/canvas-zoom rejects,
no forbidden replay RVAs (PLAN_NEXT.md).
One behavior change per build. Deploy: .\scripts\build-deploy-run.ps1 -NoStart
Then ALWAYS start/restart the game via Desktop shortcut "GTA IV Quick Restart"
(same as scripts\Restart-GTAIV.vbs → restart-gtaiv.ps1 -NoPause -DirectExe).
Do NOT use steam -applaunch / default build-deploy-run start without -DirectExe.
After headset test: update docs/CURRENT-STATE.md; write kill values BEFORE testing.
```

---

## Session 0 — compile only

```text
<SHARED PREAMBLE>
Today: Session 0 from HANDOFF_HOME_MOTION_MENU / PREP_PACK — compile only, no headset.
Run .\scripts\build-asi.ps1. New sources: vr_input.cpp, menu_bridge.cpp, ik_aim.cpp.
Fix signatures only if it fails. Do not change behaviour to make it build.
```

## Session 1 — yaw fix alone

```text
<SHARED PREAMBLE>
Today: Session 1 — camera-yaw aim fix alone.
Config: stereo=243, aimmode=2, vrinput=0. aimyaw defaults ON (1).
Test: recentre, shoot wall, turn 180° with right stick, shoot again without moving hand.
Log must show camYaw changing. A/B: aimyaw=0 should restore "shots behind me".
Do not enable vrinput or aimmode=3 today.
```

## Session 2 — IVRInput

```text
<SHARED PREAMBLE>
Today: Session 2 — IVRInput only.
Config: vrinput=1, aimmode=2, stereo=243. Ensure action JSON next to ASI.
Log: VrInput manifest loaded → ACTIVE tipR/tipL. FIRE line tip=IVRInput.
Test every G2 button/stick/grip/trigger. No aimmode=3 yet.
```

## Session 3 — controller-origin ray

```text
<SHARED PREAMBLE>
Today: Session 3 — aimmode=3 only.
Keep vrinput=1. Log: aimmode=3 + FIRE #N at most once per frame.
Test near wall, 25 m, up/down/across body, 20 s continuous fire for freezes.
```

## Session 4 — ammogate

```text
<SHARED PREAMBLE>
Today: Session 4 — ammogate=1 only (still FIRE_PED_WEAPON, but skip empty clip + slower cadence).
Log: AmmoGate / ammogate=1 on FIRE lines. Empty clip should stop firing.
Do not implement Plan B writes today.
```

## Session 5 — menu bridge

```text
<SHARED PREAMBLE>
Today: Session 5 — profile switch.
Copy config/home-dropin menumap (+ optional menupref). Test F4 first, then VR menu button.
Log: MenuBridge profile … stereo … Expect haptic pulse. Note which modes need restart.
```

## Session 6 — CE Plan B

```text
<SHARED PREAMBLE>
Today: Session 6 — Cheat Engine Plan B only.
Read docs/CE_CLICK_RECIPE.md and fill docs/ADDRESS_SHEET.md.
Config: ikprobe=1, aimmode=1 first. Do NOT set ikaim=1 until offsets verified on THIS CE build.
Never paste 1.0.7 BC0 70 blindly. After fill-in, wire ikoffs and test ikaim=1 as a SEPARATE build/session.
```
