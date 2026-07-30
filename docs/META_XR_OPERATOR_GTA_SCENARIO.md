# Meta XR Operator full-game scenario

This is the bounded, CLI-only route for moving a loaded Mode 204 game from
Roman's apartment, down to the sidewalk and street, and then attempting vehicle
entry through normal OpenXR Touch input.

It does **not** launch GTA, Steam, SteamVR, Meta XR Simulator, or a simulator
window. It attaches to an already-running, supervised Meta XR Simulator session.
It does not modify Mode 204, teleport the player, patch a save, or control the
simulator GUI.

## Why this is closed-loop

The old sustained proof held forward for 60 seconds. It proved input and smooth
frames, but stopped against apartment geometry after moving about 7.1 metres.
This scenario instead:

1. requires the launcher, Mode 204, first-person, UI-reasons=0, exact-pose
   stereo, and Touch bridge gates;
2. makes both grip and aim controller poses active and verifies their Operator
   readback before sending input;
3. verifies the pinned Meta 205.1 manifest, layer, and proxy hashes plus the
   valid Meta Authenticode signatures;
4. sets one upright OpenXR head pose once, then leaves the viewer pose fixed;
   route turns use bounded right-Touch-stick pulses closed against GTA's own
   `LookMove: heading` log, so navigation cannot snap the simulated head right
   and back on every waypoint correction;
5. drives short forward pulses toward bounded world-coordinate waypoints;
6. verifies every pulse and release in GTA's consumed XInput telemetry;
7. stops after three blocked pulses instead of grinding against a wall;
8. proves the height drop and world coordinates for downstairs/outside/street;
9. sends Touch left **Y**, which the existing bridge maps to XInput **Y** for
   vehicle entry, and distinguishes “attempt consumed” from confirmed vehicle
   occupancy; after confirmed entry it must consume right-trigger acceleration
   and prove world-space travel;
10. checks producer/host transaction advancement and zero new heartbeat pauses;
11. releases every mapped controller component in `finally`.

Operator saves paired left/right composed images at each completed stage. The
street gate also emits `operator-street-record-ready.marker`.

## Coordinate provenance

No save is bundled or changed. The local read-only saves are:

```text
SGTA400  The Cousins Bellic  8BB2FBB79DA716B57B46F828A368F3116F64B54746A621AF37B812BA93DD8144
SGTA401  Three's a Crowd     80C5A63962963D0C17B628E7918403C0E8E6B7C8A6094CC2E38A8263E45CF0F4
SGTA412  First Date          CF6288B1EBE82EB1337F96DCFE14102BDDD546F36901BAD58DF9796107FA41DC
```

The common safehouse metadata in the local `SGTA400` gives:

```text
interior player:  892.437927, -499.804688, 18.402300
exterior point:   898.766785, -504.968414, 13.980100
```

The existing Mode 204 proof independently starts its left-eye camera at about
`892.425, -499.725, 20.052`, consistent with a roughly 1.65 m eye height.
Those facts anchor the scenario. The intermediate door/stair points and three
curb search points are bounded candidate waypoints; they are data in one JSON
file so a first simulator run can tune geometry without changing renderer or
input code.

The repository has no existing save warp or teleport route. The only save
handling found is an old installer that copies/backs up saves. This scenario
does not use it and never writes an `SGTA*` file.

## Offline check

This starts nothing:

```powershell
.\scripts\run-meta-xr-operator-gta-scenario.ps1 -ValidateOnly
.\tests\meta_operator_gta_scenario_contract_test.ps1
```

Expected:

```text
MetaXrOperatorGtaScenarioPlan: PASS ... launchesGame=0 simulatorGuiControl=0 rendererChanges=0
MetaOperatorGtaScenarioContractTest: PASS ...
```

## Supervised use

First start the existing Steam-safe launcher with Meta Simulator, Mode 204, and
the Operator layer. Load the save until the live first-person world is visible.
Do not use `-ElliottCliProof`; that command protocol belongs to the other
simulator.

In a second PowerShell:

```powershell
.\scripts\run-meta-xr-operator-gta-scenario.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -RunDirectory "<the current out-openxr\runs\...\-steam-safe folder>" `
  -Authorized
```

The default acceptance level without an available ambient vehicle is:

```text
SCENARIO_COMPLETE_VEHICLE_ATTEMPTED
```

That requires the apartment start, downstairs/outside height drop, street
travel, consumed XInput Y press, continuous stereo frames, paired captures, and
final neutral. It does not pretend that an ambient vehicle existed.

If vehicle occupancy is confirmed, success is upgraded to:

```text
SCENARIO_COMPLETE_VEHICLE_DRIVEN
```

That outcome is only emitted after GTA consumes right-trigger acceleration and
telemetry proves at least 1.0 metre of world-space camera travel. A confirmed
entry followed by no acceleration/movement is a hard failure, not a weaker pass.

For the stronger gate:

```powershell
.\scripts\run-meta-xr-operator-gta-scenario.ps1 `
  -RunDirectory "<current run folder>" `
  -RequireVehicleEntry `
  -Authorized
```

That additionally requires `VehCam: INSIDE vehicle` or the existing first-person
vehicle-pointer telemetry, followed by the acceleration-and-distance drive gate.

## Evidence and the honest video limit

Each run writes:

```text
out-openxr\operator-scenarios\<timestamp>-roman-street-car\
  status.log
  result.txt
  stage-*-left.png
  stage-*-right.png
  operator-street-record-ready.marker
```

The existing video recorder is intentionally Elliott-specific: it captures that
runtime's hidden composed SBS preview HWND. Meta XR Operator 205.1 exposes
composed still-image capture, not a video stream. Reusing the Elliott recorder
for Meta would be false evidence and would require simulator-window control.
Therefore this scenario produces first-party Operator stills and a precise
street marker; a Meta video remains a separate capture seam. No video should be
claimed until that seam exists or a physical Quest mirror is recorded.
