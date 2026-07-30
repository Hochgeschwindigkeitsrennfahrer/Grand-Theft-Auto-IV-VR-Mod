# Meta XR Operator calibration proof

This is the repeatable, no-GTA proof for Meta XR Operator Standalone 205.1,
Meta XR Simulator 205.0, and the x64 `gtaiv_xr_host.exe`.

It controls the OpenXR application only through Operator's MCP/API surface. It
does **not** click, focus, type into, or otherwise control the Meta XR Simulator
window. It does not start Steam, SteamVR, Rockstar Launcher, or GTA IV.

## What the proof checks

One bounded run:

1. validates the exact `XR_APILAYER_METAX_operator` manifest, x64 layer/proxy,
   and valid Meta Platforms Authenticode signatures;
2. requires the child-only runtime manifest to classify as Meta XR Simulator
   and rejects a running SteamVR process, pre-existing x64 host, or pre-existing
   Operator listener;
3. starts one hidden, owned x64 calibration host with both a frame limit and
   timeout, while scoping the runtime and API-layer environment to that child;
4. waits for `XR_SESSION_STATE_FOCUSED` and verifies the Meta Touch Controller
   Plus interaction profile for both hands;
5. injects an instant baseline head pose, reads it back through Operator, and
   records the host's `PoseBridge`;
6. captures both composed eyes, moves/yaws the head, reads the exact pose back
   through Operator and `PoseBridge`, then captures both eyes again;
7. injects and reads back left/right `grip` and `aim` poses, then confirms both
   pose-valid bits reached `PoseBridge`;
8. holds left-stick Y at `0.8` and right A at `1.0`, confirms both through
   `PoseBridge`, explicitly writes both values back to `0.0`, and confirms
   neutral;
9. restores the original head and controller poses, stops only the host process
   that it started, confirms port 8720 is released, and checks SteamVR remained
   absent.

Controller poses are set instantly. Do not add `duration_seconds`: in a fresh
simulator session Operator cannot animate from a controller pose it has not yet
established.

## Run

Close SteamVR first. From PowerShell in `D:\code\gta-iv`:

```powershell
.\tests\meta_xr_operator_calibration_proof_contract_test.ps1

.\scripts\run-meta-xr-operator-calibration-proof.ps1 `
  -RuntimeManifest "C:\Program Files\MetaXRSimulator\v205.0\meta_openxr_simulator.json" `
  -OperatorDirectory "D:\code\gta-iv\out-openxr\external\meta-xr-operator-standalone-205.1\extracted\meta-xr-operator-standalone-public\windows" `
  -TimeoutSeconds 120
```

The Meta XR Simulator UI does not need to be automated or brought to the
foreground. The script loads its runtime directly into the hidden OpenXR host
through the child-only `XR_RUNTIME_JSON` override.

To choose an artifact directory:

```powershell
.\scripts\run-meta-xr-operator-calibration-proof.ps1 `
  -OutputDirectory "D:\OpenXR-Proofs\operator-calibration"
```

The directory must not already exist. This prevents an old proof from being
overwritten or mistaken for the current run.

## PASS

The command is successful only when `result.txt` begins with:

```text
outcome=PROOF_COMPLETE
```

The required gates include:

- focused OpenXR session and both Touch Controller Plus profiles;
- exact Operator head and four controller-pose readbacks;
- at least `0.30 m` of matching head motion through `PoseBridge`;
- active grip and aim bits for both controllers;
- held left-stick and right-A state followed by explicit neutral;
- four non-empty PNGs whose per-eye hashes change after head motion;
- strictly advancing `PoseBridge` frame IDs;
- input neutral, original pose restoration, owned-host stop, port release, and
  SteamVR absence.

## Artifacts

Each run writes a new timestamped directory below
`out-openxr\operator-calibration-proofs` by default:

```text
result.txt
summary.json
operator-transcript.jsonl
instance.json
session.json
left-profile.json
right-profile.json
original-head.json
baseline-head.json
moved-head.json
original-left-grip.json
original-left-aim.json
original-right-grip.json
original-right-aim.json
controller-readbacks.json
pose-bridge-baseline.json
pose-bridge-moved.json
pose-bridge-controllers.json
pose-bridge-input-held.json
pose-bridge-input-neutral.json
before-left.png
before-right.png
after-left.png
after-right.png
host-log-delta.txt
artifacts.sha256
```

`summary.json` is the machine-readable source of the metrics and gates.
`result.txt` is the short acceptance record. `artifacts.sha256` hashes every
other file in the proof directory.

## Scope

This proves Operator control, OpenXR input delivery, head/controller pose
delivery, stereo compositor capture, and cleanup against the calibration host.
It does not prove GTA menu navigation, walking down the apartment stairs,
driving, physical Quest optics, human stereo fusion, comfort, or Link
performance. Those require the separate supervised full-game route after this
calibration proof is green.
