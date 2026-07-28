# Elliott OpenXR Simulator headless full-game proof

This is the repeatable, no-headset validation route for GTA IV OpenXR Mode 57.
It does not open or control a simulator window and it never registers a system
OpenXR runtime. The launcher supplies the Elliott manifest only to the x64 host
through its child `XR_RUNTIME_JSON` environment.

## Verification status

The held-frame OpenXR swapchain-reuse change passed a second complete full-game
regression:

```text
out-openxr/runs/20260728-035722-steam-safe/
```

That run reports `outcome=PROOF_COMPLETE`,
`hostSwapchainReuseReady=True`, lower-bound counters of 2700 unchanged host frames
reused and 2400 new-transaction updates, accepted Mode 57 pair 600, matching
producer/host world endpoints `1940 -> 2580`, complete menu/input/pose automation,
no SteamVR, and exact installed-file restoration. The reuse gate is asserted only
for `presentation=world-temporal-stereo` after exact temporal capture poses are
active; menu reuse cannot satisfy it. The host continues tracking, input,
`xrWaitFrame`, and `xrEndFrame` at runtime cadence; only redundant D3D11 rendering
of an unchanged GTA transaction is skipped. Cached temporal submissions retain the
original per-eye capture poses/FOVs even after the live pose-history ring advances.

The corrected Mode 57 contract passed its first complete live simulator run:

```text
out-openxr/runs/20260728-000001-steam-safe/
```

Its `result.txt` reports `outcome=PROOF_COMPLETE`. The run accepted the first four
receipt-proven L/R pairs, continued through accepted pair 600, and completed with:

- GTA IV loaded past its startup menu into the world;
- matching first/latest producer and host world-transaction endpoints
  `1893 -> 2580`, with a pre-pause continuous-frame marker over `1893 -> 2040`;
- a 6.40 cm applied-camera baseline, advancing per-eye pose/camera receipts, and
  raw L/R differences distributed across the frame;
- first accepted pair `rawRgbAbsDiff=12073`, 2395 changed pixels, 64 changed
  tiles; accepted pair 4 `rawRgbAbsDiff=1985`, 34 pixels, 22 tiles;
- accepted pair 600 `rawRgbAbsDiff=1100`, 70 pixels, 27 tiles;
- UI reasons remained zero for `6549 ms` while the world gate observed three
  fresh matching producer/host transactions;
- stationary 1280x720 local-space UI and a world -> pause UI -> world round trip;
- one sRGB decode followed by an sRGB OpenXR swapchain write;
- GTA consumption of Start, A, forward stick, and neutral;
- an eight-second simulated head-pose sweep;
- no SteamVR process, no proof/cleanup failure, and exact game-file restoration.

The earlier `out-openxr/runs/20260727-214303-steam-safe/` run is retained only as
historical transport, UI, color, and input evidence. It predates the corrected
build-to-replay association and must not be cited as stereo proof.

This does not replace a real Quest 3 visual acceptance test. It cannot judge human
stereo fusion, comfort, perceived scale, Link latency, or headset smoothness.

## Corrected Mode 57 proof contract

Mode 57 is accepted only when every captured eye carries one end-to-end receipt:

```text
BuildRootA pinned pose + camera receipt
  -> ExecRoot consumes that exact build ticket
  -> successful D3D BeginScene consumes that exact replay ticket
  -> EndScene on the same D3D device and thread captures that eye
```

The pair gate additionally requires an ordered left/right pair from consecutive
build epochs, progressive pose and camera generations, and raw pixel distinction
between the two held render targets before any canvas conversion. The launcher
must observe at least four accepted receipt-proven pairs. The proof run uses only
the x64 OpenXR host and Elliott runtime: the ASI backend is `openxr`,
`openvr_api.dll` is disabled for the run, and no OpenVR or SteamVR process may
participate.

This corrected contract now has both static coverage and the successful live
full-game result above.

## Build the pinned simulator

From PowerShell in `D:\code\gta-iv`:

```powershell
.\scripts\setup-elliott-openxr-simulator.ps1
```

The setup script checks out
`48a70f440ac7d9bda385994937e3da8e15a4d9bb`, verifies the packaged patch and
file-IPC asset hashes, builds x64 Release, verifies the PE machine, and prints the
manifest path. It does not activate or launch the runtime.

## Build GTA artifacts

```powershell
.\scripts\build-asi.ps1
.\scripts\build-openxr-host.ps1
.\tests\openxr_runtime_info_test.ps1
.\tests\elliott_cli_proof_contract_test.ps1
```

## Run the locked proof

Use the manifest printed by the setup script:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 57 `
  -RuntimeManifest "D:\code\gta-iv\out-openxr\external\OpenXR-Simulator\bin\openxr_simulator.json" `
  -ElliottCliProof `
  -StopWhenProofComplete `
  -MaxSeconds 600 `
  -Authorized
```

The launcher:

1. refuses dirty Steam launch options that request OpenVR or SteamVR;
2. stops only path-verified stale Rockstar launcher processes when no GTA title
   process exists;
3. deploys the candidate ASI with `backend=openxr` and `stereo=57`;
4. backs up/removes `openvr_api.dll` and creates the OpenVR disable sentinel;
5. starts the x64 host with a child-only Elliott runtime override;
6. polls all SteamVR process names every 100 ms and aborts if any appears;
7. uses ordinary Steam app 12210 authentication with no title arguments and one
   bounded Rockstar handoff retry;
8. drives Start/A/stick/neutral/pose through acknowledged JSON command files;
9. waits for UI reasons zero plus three fresh shared world transactions sustained
   for at least two seconds, uses a bounded pre-pause pose sweep to make static-scene
   continuity deterministic, and requires observed unchanged-swapchain reuse before
   testing pause and locomotion;
10. records `PROOF_COMPLETE`, stops the title and host, removes command files, and
    restores every saved game file.

## Acceptance markers

Do not accept a run from a menu image, one stereo pair, or the superseded
EndScene-arm marker. Require all of these in `status.log` and `result.txt`:

```text
WORLD LOAD READY
continuous world frames proven
Mode57 build-to-replay receipt PASS
Mode57 mailbox contains temporal, pixel-distinct L/R eyes
Mode57 temporal L/R transaction accepted by host
Mode57 per-eye capture poses active in OpenXR host
unchanged OpenXR world swapchain reuse READY
in-game world->pause UI->world round trip PASS
stick/neutral proof PASS
command-file A/Start/stick/neutral/pose automation COMPLETE
PROOF COMPLETE
RESTORED: original ASI/backend/stereo/OpenVR state
```

Also require:

```text
outcome=PROOF_COMPLETE
controllerProofComplete=True
stereoProofComplete=True
worldLoadFullyReady=True
pauseRoundTrip=True
temporalBuildReplayReceiptReady=True
hostSwapchainReuseReady=True
hostSwapchainReuseFramesLowerBound=...
hostSwapchainUpdatesLowerBound=...
restoreFailures=
```

`temporalBuildReplayProofPairOrdinal` must be present in `result.txt` and must be
at least `4`. The corresponding ASI log must contain accepted lines in this form:

```text
StereoOpenXRTemporal: pair #... build=.../... pose=.../... display=.../... rawRgbAbsDiff=... changedPixels=... changedTiles=... camDist=...cm camGen=.../... buildTid=... execTid=... d3dTid=... proof=buildroot-execroot-fifo-beginscene-endscene-entry
```

For each accepted line, right build epoch must equal left build epoch plus one.
On the pre-canvas held textures, the 64x64 RGB audit must report
`rawRgbAbsDiff >= 1024`, at least 16 changed pixels, and at least 4 changed
8x8 spatial tiles. This is a distributed non-identity check; it corroborates
the separate ordered FIFO, D3D-boundary, and exact applied eye-camera receipts
rather than claiming that raw pixel difference alone proves stereo causality.
Build, execution, and D3D are independent asynchronous lanes: each thread ID must
be nonzero, stable across the accepted L/R pair, and equal to the owner recorded
for its own lane. No cross-lane thread equality or inequality is assumed.
`endscene-entry` is precise: capture and validation happen at the matching
`EndScene` hook entry, before GTA calls the real D3D9 `EndScene`.

Mode 57 is temporal separate-eye stereo. Never relabel it as same-tick stereo.
