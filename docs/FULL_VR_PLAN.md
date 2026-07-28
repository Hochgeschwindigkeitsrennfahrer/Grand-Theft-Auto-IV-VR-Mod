# Full VR Plan - Quest 3 OpenXR Primary, OpenVR Preserved

**Plan date:** 2026-07-23 PT

**Repository baseline:** upstream `01f355b` (Modes 50–53), merged on the
`codex/openxr-sidecar-integration` branch

**Status:** The corrected Mode 57 build-to-replay receipt chain and its
two-boundary CPU-readback scheduler passed a complete headless Elliott full-game
run on 2026-07-28. GTA loaded into the world, sustained matching producer/host
transactions `1906 -> 2520`, completed a pause-menu round trip, consumed Start,
A, forward-stick and neutral input, and completed a scripted head-pose sweep
without SteamVR. The run continued through accepted receipt pair 600. Mode 55
remains the mono reliability fallback. Mode 56's nominal same-frame DrawScene x2
outputs were byte-identical and that seam is rejected as the current stereo
route. Real Quest 3 visual/comfort acceptance and the same-tick product stereo
result required by Gate 6 remain pending.

**Primary headset:** Meta Quest 3

**Compatibility headset:** HP Reverb G2, best effort without regressing the proven OpenVR path

---

## 1. Decision

The post-mono product direction is:

1. Keep `GTAIV.exe`, stock DXVK 3.0.2, FusionFix, and every in-process hook **Win32/x86**.
2. Add a separate **x64 OpenXR host** for frame timing, HMD/controller poses, composition,
   and Quest 3 presentation.
3. Move poses and complete eye-frame transactions between the x86 game bridge and x64
   host through versioned shared memory plus GPU-shared resources.
4. Make Quest 3 through the Meta OpenXR runtime the primary route. It must not require
   SteamVR.
5. Keep the current in-process OpenVR/SteamVR submit path as a selectable fallback and
   as the Reverb G2 regression baseline.
6. Finish same-frame GTA stereo once and feed the same captured eye pair to either
   compositor backend. Do not fork the GTA camera or stereo implementation by headset.

This does **not** turn GTA IV into a 64-bit game. The split is:

```text
GTAIV.exe (x86)
  + stock DXVK 3.0.2 x86
  + gtaiv_dxvk_vr.asi x86
      - GTA hooks
      - camera and same-frame stereo
      - eye capture
      - shared GPU producer
                 |
                 | shared pose/state ABI
                 | shared GPU handles + fence
                 v
gtaiv_xr_host.exe (x64)
  + OpenXR loader x64
  + D3D11 OpenXR session
      - xrWaitFrame / xrLocateViews
      - Quest/G2 actions
      - swapchains and layers
      - frame freshness and presentation
                 |
                 v
OpenXR runtime
  - Meta Quest Link/Air Link: primary, no SteamVR
  - SteamVR OpenXR: compatibility route
```

The current PC has both Meta runtime manifests registered:

```text
x64: C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json
x86: C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_32.json
```

An in-process x86 OpenXR probe is therefore technically possible. It is not the
preferred product architecture: the x64 sidecar keeps the runtime, actions, frame
loop, and compositor out of GTA's fragile 32-bit address space. The x86 runtime is a
diagnostic fallback, not a reason to duplicate the product implementation.

---

## 2. What must be preserved

These are golden baselines, not cleanup targets:

| Baseline | Evidence | Rule |
|---|---|---|
| Flat GTA | Stock DXVK 3.0.2 x86 works | Never replace it blindly with a foreign VR DLL |
| Mono headset | `ID3D9VkInterop*` to OpenVR submit works | Keep as the recovery path |
| Mode 14 | Angle-correct canvas fused with correct proportions | Preserve the geometry and black-border behavior |
| Mode 23 | Same-frame dual execute is stable and removes temporal jumping | Use as the stable same-frame base |
| Mode 24 | Right-pass VS constant translation is built but not headset-tested | Test before designing another stereo hook |
| Kill switch | Stereo mode `0` | Every experiment must return safely to it |

The latest code contains modes 18-24. The first implementation session is therefore a
Mode 24 decision test, not a rewrite.

---

## 3. Reference projects: what to use and what not to assume

### Local source snapshot

| Project | Local state during this audit | Reuse |
|---|---|---|
| `D:\code\fnvvr` | `112fe9a`, two commits ahead of `origin/main`, with active local edits | x86/x64 split, fixed ABI, host lifecycle, D3D9-to-D3D11 GPU transport, fence/resource identity, launch preflight |
| `D:\code\swgvr` | `ebfcc4f6a`, clean and equal to `origin/main` | OpenXR D3D11 frame loop, projection and quad layers, action spaces, runtime/adapter probing |
| `D:\code\vulkanOpenMW` | `c0fbbf2`, active local edits, no remote configured | flat UI to world-projection handoff, interaction-profile model, stale-frame rejection, pointer/UI architecture |

### Important limitations

- FNVVR is not a drop-in GTA renderer. Its current status explicitly keeps live product
  presentation/mutation fused, and its game-specific engine backend is unfinished.
  Reuse the proven split, protocol, tests, and transport patterns; do not claim that
  copying its host completes GTA stereo.
- SWGVR owns its engine source and renders directly with D3D11. GTA IV is a closed x86
  D3D9 game under DXVK, so only the OpenXR lifecycle, layer, input, and diagnostics
  patterns transfer.
- OpenMW/OpenMWVR is valuable for interaction and layer design. Do not copy GPL or
  otherwise incompatible code into this MIT repository without a license review.
- FNVVR currently has no top-level license file. Until provenance is clarified, port
  concepts and small owner-approved pieces deliberately rather than bulk-copying files.
- Do not import Rockstar assets, closed HL2VR/Luke-Ross binaries, or foreign game hooks.

### Exact reference anchors

FNVVR:

- `D:\code\fnvvr\CMakeLists.txt`
- `D:\code\fnvvr\protocol\fnvxr_shared_state.h`
- `D:\code\fnvvr\protocol\fnvxr_gpu_frame_transport.h`
- `D:\code\fnvvr\renderhook\fnvxr_d3d9ex_color_transport_win32.cpp`
- `D:\code\fnvvr\host\fnvxr_gpu_color_consumer.cpp`
- `D:\code\fnvvr\host\fnvxr_openxr_pose_host.cpp`
- `D:\code\fnvvr\scripts\build-fnvxr-product.ps1`
- `D:\code\fnvvr\docs\architecture-v2.md`
- `D:\code\fnvvr\docs\6dof-coordinate-proof.md`

SWGVR:

- `D:\code\swgvr\src\engine\client\application\Direct3d11\src\win32\Direct3d11_VrBridge.cpp`
- `probeOpenXrD3D11`
- `beginWorldFrame`
- `endWorldFrameInternal`
- projection-layer and quad-layer setup

OpenMW/OpenMWVR:

- `D:\code\vulkanOpenMW\proof\fnvxr-bridge-experiment\docs\openmwvr-reuse-map.md`
- `D:\code\vulkanOpenMW\proof\fnvxr-bridge-experiment\docs\current-retail-openmw-handoff-status.md`
- `D:\code\vulkanOpenMW\nikami-openmw-lab-higgs-lite\apps\openmw\mwvr\openxrinput.cpp`
- `D:\code\vulkanOpenMW\nikami-openmw-lab-higgs-lite\apps\openmw\mwvr\vrgui.cpp`
- `D:\code\vulkanOpenMW\nikami-openmw-lab-higgs-lite\apps\openmw\mwvr\vrpointer.cpp`

---

## 4. Target repository shape

Keep the current OpenVR build working while adding a dual-architecture product build.

```text
src/
  asi/                         existing x86 GTA hooks
    vr_backend.*
    xr_bridge_client.*
    xr_gpu_producer.*
  shared/
    xr_protocol.h              pointer-free x86/x64 ABI
    xr_math.h                  headset/game coordinate conversion
    xr_frame_contract.h
  host/
    main.cpp
    openxr_runtime.*
    openxr_actions.*
    openxr_d3d11_compositor.*
    xr_gpu_consumer.*
tests/
  protocol_layout_test.cpp
  stable_snapshot_test.cpp
  xr_math_test.cpp
  frame_contract_test.cpp
  gpu_bridge_producer_win32.cpp
  gpu_bridge_consumer_x64.cpp
scripts/
  fetch-openxr.ps1
  build-product.ps1
  preflight-openxr.ps1
  run-openxr.ps1
```

Expected product artifacts:

```text
out-product/
  x86/
    gtaiv_dxvk_vr.asi          PE machine 0x014c
  x64/
    gtaiv_xr_host.exe          PE machine 0x8664
    openxr_loader.dll          PE machine 0x8664
```

No x64 DLL is copied into the GTA directory. No OpenXR runtime is redistributed.

Keep `scripts/build-asi.ps1` and the existing OpenVR launch path operational until the
new product path passes all gates.

---

## 5. One stereo core, two presentation backends

The current stereo code directly uses `vr::EVREye`, `GetProjectionRaw`, and OpenVR
submission types. Refactor only after Mode 24 is decided:

```cpp
enum class VrEye { Left, Right };

struct EyeFrustum {
  float leftTan;
  float rightTan;
  float upTan;
  float downTan;
};

struct EyePose {
  Quaternion orientation;
  Vec3 positionMeters;
  EyeFrustum frustum;
};
```

OpenVR fills this internal representation from `GetEyeToHeadTransform` and
`GetProjectionRaw`. The OpenXR host fills it from `xrLocateViews` and `XrFovf`, then
publishes it to the x86 bridge.

The existing Mode 14 angle-correct canvas math consumes `EyeFrustum`; it must not know
which runtime produced the frustum. Eye capture produces an internal `StereoPair`.
OpenVR can submit the pair in-process, while OpenXR publishes it to the x64 host.

Runtime selection should be explicit:

```text
gtaiv_dxvk_vr.backend
  openxr   = x64 sidecar, primary after acceptance
  openvr   = current SteamVR route
  off      = flat game, no headset submission
```

Never initialize OpenVR and OpenXR in the same run.

---

## 6. Cross-architecture contract

Use fixed-size, pointer-free structures. Never put `HANDLE`, pointer, `bool`, `size_t`,
STL types, or architecture-dependent padding directly in shared memory.

Every mapping begins with:

```text
magic
abiVersion
structSize
producerPid
producerEpoch
oddEvenSequence
qpcTimestamp
```

Cross-process GPU handles are stored as fixed-width `uint64_t` values only after
`DuplicateHandle` has created a handle in the consumer process. A producer-process
handle value is never treated as globally valid. The protocol declares which process
owns and closes every duplicated handle, and a new producer epoch invalidates all old
handle values.

Suggested mappings:

| Mapping | Producer | Consumer | Contents |
|---|---|---|---|
| `Local\GTAIVXR_HostState_v1` | x64 host | x86 ASI | runtime/session state, heartbeat, shutdown reason |
| `Local\GTAIVXR_Pose_v1` | x64 host | x86 ASI | predicted display time, HMD/eye/controller poses, FOV, actions, validity flags |
| `Local\GTAIVXR_GameState_v1` | x86 ASI | x64 host | source frame/tick, camera/body state, loading/UI state, bridge heartbeat |
| `Local\GTAIVXR_StereoFrame_v1` | x86 ASI | x64 host | resource-set identity, duplicated NT handles, adapter LUID, format/size, fence/value, pose/source IDs |

Required stereo-frame fields:

- left and right color resources are nonzero and non-aliased;
- adapter LUID;
- width, height, DXGI format, color space;
- resource-set ID and producer epoch;
- shared fence handle and required fence value;
- game source-frame ID and simulation-tick ID;
- pose sequence and rendered `XrTime`;
- exact left/right render poses and FOV used for those pixels;
- flags for same-tick, separated, complete, UI-free, and valid;
- host-consumed acknowledgement for safe resource reuse.

The writer marks the sequence odd, writes the complete payload, issues a full fence,
then publishes an even sequence. The reader copies twice and accepts only identical,
even snapshots. A crashed producer leaves an odd or stale frame that is rejected.

The x86 and x64 builds run the same `static_assert(sizeof(...))` and `offsetof` tests.

---

## 7. OpenXR frame ownership

Only the x64 host calls the OpenXR frame/session API:

```text
xrPollEvent
xrWaitFrame
xrBeginFrame
xrLocateViews(predictedDisplayTime)
xrSyncActions
publish PoseFrame
consume newest complete GPU StereoFrame
acquire/wait XR swapchain images
wait for producer fence
GPU-copy left/right into XR swapchains
release swapchain images
xrEndFrame
```

The game bridge:

```text
read stable PoseFrame
apply one pose sample at the GTA render boundary
render/capture both eyes from one simulation tick
signal the GPU fence
publish one complete StereoFrame transaction
```

The host submits the view poses/FOV that were actually used to render the accepted eye
pair. It does not stamp old pixels with unrelated current views. OpenXR can then
reproject from the declared render pose. A pair that is stale, incomplete, from
different ticks, from different pose sequences, or still GPU-busy is dropped.

OpenXR calls never occur in `CopyMat`, a GTA render-phase hook, or a second game thread.

---

## 8. GPU transport decision gate

This is the highest-risk part of the OpenXR migration. Prove it outside GTA before
adding it to the ASI.

### Route A — rejected on this machine: DXVK D3D9 KMT handle to D3D11

The first live direct-GTA attempt proved that DXVK's legacy D3D9 shared handle cannot
be opened as a native D3D11 resource here: `OpenSharedResource` returned
`E_INVALIDARG`. That route is retired and must not be retried every `EndScene`.

### Route B — selected and built offline: native D3D11 NT resources imported by Vulkan

The replacement keeps stock DXVK 3.0.2:

1. The x86 ASI obtains DXVK's Vulkan instance, physical device, device, queue, source
   eye `VkImage`, image layout, and adapter LUID through `ID3D9VkInterop*`.
2. A native x86 D3D11 device is created on that exact LUID.
3. It creates three slots of distinct L/R D3D11 NT-handle textures plus shared D3D11
   ready/release fences.
4. Those D3D11 textures and fences are imported into DXVK's Vulkan device using
   `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` and
   `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT`.
5. On DXVK's locked submission queue, Vulkan transitions external ownership, copies
   the captured L/R images, and signals the ready timeline value.
6. The x64 host duplicates handles from the verified producer process, opens them as
   D3D11 resources, GPU-copies the selected slot to private textures, signals release,
   and never aliases L/R views.

The pointer-free ABI v5 publishes a complete transaction only after both eye copies
are queued. `WorldStereo` additionally requires the producer's
`VerifiedWvpStereo` proof; `WorldMono` requires the explicit immersive-mono flag and
one exact source tick/pose; `UiQuad` carries the original content aspect. A slot
cannot be reused until the release fence reaches its transaction. No CPU image path
exists.

Live status on 2026-07-26: the guarded 23:11 run proved adapter import, timeline
synchronization, transaction publication/acquisition, and clean shutdown while
SteamVR remained absent. ABI-v5 presentation semantics, sRGB private views, and
stationary UI placement passed offline for this route. This superseded GPU-fence
route remains historical. The active v6 CPU-mailbox route subsequently passed the
Mode 57 headless full-game proof; physical Quest visual acceptance is still
required for it.

### Route C - last resort: minimal DXVK 3.0.2 patch

If stock DXVK cannot create a safe export path, patch only the exact `v3.0.2` source to
expose dedicated exportable eye resources. Keep:

- the stock 3.0.2 binary as the flat/OpenVR fallback;
- a small auditable patch series;
- an explicit build ID in the DXVK and ASI logs;
- no unrelated async, game, or compositor changes.

Do not switch the project back to IronWolf.

### Explicitly rejected production route

Do not ship a CPU readback/pixel ring. A low-resolution CPU copy may be used for one
diagnostic frame only, never as a milestone or fallback called "VR".

---

## 9. Milestone plan

Each numbered gate is a separate behavior change and headset session. Do not combine
two red gates into one build.

### Gate 0 - repository and rollback baseline

Deliverables:

- record current GTA commit and PE identity;
- preserve the pre-sync stash instead of applying it blindly;
- keep the two existing archive files untracked;
- capture a Mode 0 log and flat launch;
- document exact game version/module hash and current FusionFix version;
- add a build ID to both future x86 and x64 logs.

Pass:

- `master` stays based on `8fe072a` or a traceable successor;
- stereo `0` returns to the known mono/flat path;
- no reference repository is modified.

### Gate 1 - decide Mode 24 now

Test the already-built path before new stereo code:

1. Start with OpenVR because it is the proven compositor.
2. Stand still and inspect one near and one far object.
3. Turn the head, walk, and drive to expose temporal jumping.
4. Record FPS and these log lines:

```text
VsPatch: MATCH rowvec/colvec ...
StereoExecPhase: ... haveL=1 haveR=1 ... vsPatch=N
StereoSubmit: L=1 R=1 mode=24
```

Outcomes:

- **Fusion and parallax work:** freeze Mode 24 as the stereo core; no more render-hook
  research before the OpenXR bridge.
- **`vsPatch=0`:** the game uploads combined draw-local WVP constants. Move to the
  verified shader-contract path below.
- **`vsPatch>0`, no fusion:** log vertex shader identity and prove whether the patched
  register reaches each right-eye draw; do not add more matrix heuristics.
- **Crash:** set stereo to `0`, retain Mode 23 as the stable same-frame base, and inspect
  the recorded exception stage/address.

Pass:

- same-frame L/R from one simulation tick;
- near objects have stronger parallax than far objects;
- no frame-alternating left/right jump while moving;
- one fused image when still.

### Gate 2 - x64 OpenXR host, generated scene only

Build a minimal x64 host based on the Khronos loader and the FNVVR/SWG frame-loop
patterns. It renders an asymmetric left/right calibration grid, not GTA pixels.

Result on 2026-07-24 PT: **passed.** The verified x64 host identified
`Meta Quest 3`, selected the RTX 4070 Super by runtime LUID, created two 2368x2576
swapchains, reached `READY/RUNNING/FOCUSED`, submitted 900 projection frames, and
shut down cleanly. The user saw distinct left/right calibration output. No GTA
image was expected because this gate intentionally uses generated pixels. See
[`OPENXR_QUEST3_TEST.md`](OPENXR_QUEST3_TEST.md).

Log:

```text
XRHost: runtime=...
XRHost: system=...
XRHost: adapterLuid=...
XRHost: view[0/1] recommended=... fov=...
XRHost: session READY/RUNNING
XRHost: projection submit ok
```

Pass on Quest 3:

- Meta Quest Link is the active OpenXR runtime;
- SteamVR is closed;
- the x64 host creates a D3D11 OpenXR session;
- both eyes show the correct labeled calibration image;
- recenter/session stop exits cleanly.

The baseline uses only standard OpenXR plus `XR_KHR_D3D11_enable`. Meta extensions are
optional and gated. Use the Khronos headers/loader for the core path; consult the
[Meta OpenXR SDK](https://github.com/meta-quest/Meta-OpenXR-SDK) only for optional,
capability-queried Meta extensions.

### Gate 3 - fixed shared ABI and pose flow

Implementation update (2026-07-26): the fixed `PoseBridge` ABI, exact eye pose/FOV
history, Quest Touch action publisher, x86 camera consumer, XInput mapping, and reverse
haptic bridge all build offline. Input is valid only while the OpenXR session is
focused and the packet is fresh; otherwise GTA falls through to the real physical
XInput device. This is not a live pass until a headset test validates pose, controls,
haptics, and host-loss behavior.

Add the protocol and its x86/x64 tests. Then:

1. x64 host publishes HMD/eye/controller poses and actions;
2. x86 diagnostic reader logs them without changing the GTA camera;
3. a later build applies HMD orientation only;
4. a later build adds local HMD translation.

Pass:

- no torn snapshots in a stress test;
- host heartbeat loss neutralizes pose/input within 250 ms;
- pose age, sequence, predicted time, and validity are logged;
- GTA never consumes an invalid or stale pose;
- stopping the host leaves GTA flat/neutral rather than frozen.

### Gate 4 - standalone GPU bridge

Build the x86 checkerboard producer and x64 consumer described in section 8.

Pass:

- both PE architectures are verified;
- both processes report the same adapter LUID;
- left/right resources are distinct;
- a changing frame counter is observed after the shared fence;
- producer cannot overwrite a resource set until the consumer acknowledges it;
- no CPU image transfer or per-frame handle recreation;
- 10,000 transactions complete without a stale/torn frame or leaked handle.

Route B is selected in source, but this gate remains open until its first controlled
live GPU-transport test produces the evidence above.

### Gate 5 - GTA mono through OpenXR, no SteamVR

Connect the proven GPU producer to the existing Mode 0/canvas output. Present the mono
game image as a comfortable OpenXR quad or identical-eye diagnostic layer.

This is a transport milestone only, not stereo acceptance.

Pass:

- Quest 3 displays GTA through the x64 OpenXR host with SteamVR closed;
- monitor rendering remains correct;
- host/game shutdown order is clean;
- adapter, format, color space, fence, frame age, and copy timing are logged;
- backend `openvr` still starts the old path unchanged.

### Current Mode 57 headless gate - temporal build-to-replay proof

Mode 57 is the bounded temporal-stereo diagnostic on the way to Gate 6. Its
corrected provenance chain is:

```text
BuildRootA pins the OpenXR pose and records the applied eye-camera result
  -> ExecRoot consumes and forwards that exact build ticket
  -> a successful D3D BeginScene consumes that exact replay ticket
  -> EndScene captures on the same D3D device and thread
```

An Elliott proof is accepted only after four complete L/R pairs pass that chain.
Each accepted pair must use consecutive build epochs, increasing pose and camera
generations, and distinct raw held-render-target pixels before canvas conversion
(`rawRgbAbsDiff >= 1024` on a 64x64 RGB grid, at least 16 changed pixels
distributed across at least 4 of its 8x8 spatial tiles). This raw check is a
non-identity corroboration; stereo causality comes from the ordered FIFO,
matching D3D boundary, and exact applied head-local L/R camera receipts. The
exact positive marker is:

```text
proof=buildroot-execroot-fifo-beginscene-endscene-entry
```

The pinned simulator manifest is:

```text
D:\code\gta-iv\out-openxr\external\OpenXR-Simulator\bin\openxr_simulator.json
```

Use that directory exactly. This gate runs the x64 OpenXR host with backend
`openxr`, disables the game-side OpenVR DLL, and aborts if SteamVR appears. It
does not initialize or submit through OpenVR. The corrected chain passed the
full-game run archived at:

```text
out-openxr/runs/20260728-000001-steam-safe/
```

That run reached `outcome=PROOF_COMPLETE`, accepted receipt pair 600 with a
6.40 cm applied-camera baseline and spatially distributed raw pixel differences,
kept UI reasons at zero for 6549 ms while observing three fresh matching
producer/host world transactions, and completed menu, controller, pose-sweep,
cleanup, and exact-restore gates. The pre-pause continuous-frame marker covers
transactions `1893 -> 2040`; the result's matching first/latest world endpoints
are `1893 -> 2580`. The superseded 2026-07-27 run cannot close this gate. The
accepted result is still temporal stereo rather than the same-tick product result
required by Gate 6.

The latest transport/pacing regression is:

```text
out-openxr/runs/20260728-044211-steam-safe/
```

It adds an atomic two-boundary CPU-readback gate: queue L without touching the
mailbox, then queue R and commit both eyes only on a fresh valid host pose with
the exact same pair/context. The completed proof reached matching world
transactions `1906 -> 2520`, pair 600, full menu/controller/pose automation, and
exact restoration with no SteamVR process. Its sparse readback mean/p95 improved
from `6.55/9.55 ms` to `5.73/8.81 ms`. This closes the scheduling experiment,
not Gate 6: it adds bounded delivery latency, remains a CPU pixel path, and does
not make the two eye images same-tick.

### Gate 6 - complete same-frame stereo through OpenXR

Publish the locked same-frame `StereoPair`:

- distinct left/right source images and host textures;
- one game simulation tick;
- one accepted pose sequence;
- exact render poses/FOV;
- Mode 14 angle-correct canvas recomputed from OpenXR `XrFovf`;
- black borders retained until source FOV is safely widened.

Pass:

- correct left/right eye ordering;
- fusion while still;
- near/far parallax;
- no temporal eye jump during head turn, walking, or driving;
- no mono gameplay fallback reported as stereo;
- stale/incomplete frames are visibly rejected or safely held, not mislabeled.

### Gate 7 - exact WVP surgery if Mode 24 fails

Historical implementation result (2026-07-26): Mode 54 parses the active shader's
embedded CTAB, clusters exact `gWorld`/`gWorldViewProj` camera factors, patches only
the dominant gameplay factor at each right-eye `Draw*`, restores constants
immediately, and compares complete L/R draw sequences. Missing CTAB, ambiguous
factors, partial patch coverage, changed constants, or failed restore invalidates the
pair. ABI v5 preserves that proof for a future valid stereo producer. The guarded
23:22 run reported `draws=0/0` and `verified=0`, while actual VS/draw work appeared
later on the replay thread. Mode 54 is therefore a failed seam, not a candidate for
more headset tuning.

Use the FNVVR coordinate/WVP proof concept, not its game offsets:

1. Hash each GTA vertex shader bytecode used by world draws.
2. Trace constant writes and the draw that consumes them.
3. Prove the exact view or combined WVP register contract per shader family.
4. Start with a center/center dual pass: the eye outputs and draw structure must match.
5. Apply a mathematically tested eye delta only to verified world shaders.
6. Record shader coverage; unknown world shaders remain unmodified and visible in logs.
7. Add one shader family per build until world coverage is complete.

Do not ship the broad "looks like a matrix" scan as the final path. Do not patch UI,
shadow, reflection, or auxiliary passes without classifying them.

Pass:

- all visible world shader families have explicit contracts;
- left/right passes have matching draw structure;
- the right-eye transform is proven at the draw that consumes it;
- no shader-contract miss can silently publish a "complete" stereo pair.

### Gate 8 - camera, body, and 6DoF

Centralize one coordinate conversion:

```text
OpenXR: +X right, +Y up, -Z forward
GTA:    +X right, +Y forward, +Z up

gta(x, y, z) = (xr.x, -xr.z, xr.y)
```

Use a yaw-only recentered body frame. Apply HMD orientation/translation to the camera,
not continuously to the player actor. Eye poses, camera transform, and pixels must use
the same pose sequence.

Sub-gates:

1. orientation only;
2. translation only;
3. head/body yaw separation;
4. recenter;
5. on-foot movement frame;
6. vehicle entry/exit and heading follow;
7. aim-camera hook;
8. configurable first/third-person anchor.

Pass:

- lean and look without rotating the body or movement heading;
- no 38 cm pivot swing unless deliberately configured;
- no stale transform after menu, loading, death, vehicle, or camera transition;
- F9/recenter changes the baseline once and does not drift.

### Gate 9 - UI, HUD, and controls

Presentation modes:

| Game state | OpenXR presentation |
|---|---|
| world gameplay, Mode 55 fallback | immersive mono projection layer |
| world gameplay, Mode 56 candidate | DrawScene-proof verified stereo projection layer |
| world gameplay, Mode 57 diagnostic | receipt-proven temporal L/R projection layer |
| pause/map/phone/menu/loading | stationary local-space mono quad layer |
| invalid/stale transition | last safe UI quad for a bounded time, then blank |

Add a read-only GTA UI-state probe before suppressing or moving any draw. Do not infer
all menu state from pixels.

Offline implementation status (2026-07-26):

- frame ABI v5 stamps `WorldMono`, `WorldStereo`, or `UiQuad`, reason flags,
  immersive-mono/WVP-proof flags, original content dimensions, and the freshest
  source eye as one GPU transaction;
- GTA IV CE 1.2.0.59 pause/map, loading, and phone bytes are resolved read-only from
  unique AOB signatures; all probes are required or presentation fails closed;
- at `EndScene`, UI mode copies the unwarped GTA backbuffer into two distinct stable
  textures from the same frame; v5 carries the original source dimensions so the
  host restores its aspect on the quad;
- `UiQuad` intentionally displays only one selected source image identically to both
  eyes; the protocol does not impose the world-stereo same-tick rule on UI;
- the x64 host renders that image to its UI swapchain and submits a core
  `XrCompositionLayerQuad` in `XR_REFERENCE_SPACE_TYPE_LOCAL`; its pose is latched
  1.8 m in front of the current view when the UI opens and remains fixed while the
  head turns or translates;
- same-tick world routes remain strict: same source frame, pose sequence, and
  rendered `XrTime`, with exact retained pose/FOV or black; Mode 57 is the
  explicit temporal exception and requires the complete BuildRootA -> ExecRoot
  -> BeginScene -> same-thread/device EndScene receipt described above.

The earlier head-locked quad reached the headset. Stationary placement and aspect
restoration now pass both offline tests and the Mode 57 headless full-game
pause-menu round trip. Physical Quest visual/comfort acceptance remains required.

OpenXR actions:

- standard left/right aim and grip poses;
- triggers, squeeze, thumbsticks, A/B/X/Y, menu;
- Oculus Touch interaction profile for Quest 3;
- Microsoft motion-controller and simple-controller profiles for G2 compatibility;
- runtime-selected bindings rather than a Meta-only hard-coded input table.

Input sub-gates:

1. log actions only;
2. locomotion and snap/smooth turn;
3. menu pointer ray and normal mouse click;
4. phone/map/pause navigation;
5. weapon/aim integration later.

Current Touch-to-XInput mapping:

| Touch | GTA/XInput |
|---|---|
| left/right sticks | left/right sticks |
| triggers | LT/RT |
| squeezes | LB/RB |
| X/Y and A/B | X/Y and A/B |
| stick clicks | L3/R3 |
| left Menu | Start |
| Menu + L3 | Back |
| L3 + left-stick direction | D-pad |
| XInput vibration | left/right OpenXR haptics |

HUD is a separate problem from world projection. Never shrink the whole angle-correct
world canvas to pull the radar inward. First classify HUD draws; then either reposition
them in GTA or capture them to a dedicated quad/layer.

### Gate 10 - performance and pacing

Current evidence suggests a true second render costs roughly half of the 90 FPS mono
rate. Do not hide that with a misleading FPS counter.

The 2026-07-28 Mode 57 headless profile separates the present bottlenecks:

- the world produced about 31 accepted temporal pairs per second because one pair
  consumes consecutive L/R GTA frames; sampled eye-capture skew was about 16.7 ms;
- synchronous two-eye CPU readback plus mailbox copy averaged about 6.2 ms in the
  sampled world path and reached about 10 ms;
- queuing both DXVK readbacks before either `LockRect` passed a full game
  regression, but did not materially change the sparse mean/p95
  (`6.55/9.55 ms` versus `6.49/9.36 ms`). Enqueue rounded to zero and the cost
  remained in GPU completion, so a duplicate submission flush was not the main
  bottleneck;
- splitting Mode 57 over two fresh host-pose boundaries then reduced the same
  sparse readback mean/p95 to `5.73/8.81 ms` (12 samples). Phase L queues its
  copy without locking or publishing; the later phase queues R, waits, and
  atomically commits both eyes. The successful run advanced world transactions
  `1906 -> 2520`. A 250 ms wall-time bound is required because raw GTA producer
  calls run much faster than host poses; a rejected two-call bound caused the
  intentionally retained `043350` negative test to stall;
- the x64 host previously reacquired and redrew unchanged GTA transactions. It now
  reuses the most recently released OpenXR swapchain image while keeping
  `xrWaitFrame`, pose/input publication, and `xrEndFrame` live. The full-game
  regression observed at least 2700 such reuse frames.

That host change removes redundant work and prevents exact temporal capture poses
from aging out of the live lookup ring. It does not improve GTA motion rate. The
remaining Gate 10 path is same-frame dual-eye rendering plus GPU-only transport; a
CPU-mailbox scheduling change is diagnostic mitigation, not the shipping answer.
Queue-both/defer-lock is the next larger CPU-path experiment only if another
measurement is needed before replacing this transport.

Measure separately:

- GTA simulation rate;
- left/right render time;
- GPU bridge copy time;
- fence wait time;
- host swapchain copy time;
- frame age at submit;
- compositor/runtime dropped-frame state.

Initial playable target:

- stable 45 application FPS at a 90 Hz Quest mode, or 36 at 72 Hz, with runtime
  reprojection behaving cleanly;
- no CPU readback;
- GPU bridge copy p95 under 2 ms;
- stale/dropped bridge transactions below 1% in a five-minute drive;
- no unbounded wait on either process.

Final quality target:

- native 72 Hz where practical, or an explicitly documented supported half-rate mode;
- configurable render scale and canvas size;
- one center/union visibility build where GTA permits it;
- no duplicate simulation tick;
- optional `XR_FB_display_refresh_rate` only after the standard path is stable.

Never hard-code G2 or Quest recommended dimensions. Query every runtime session.

### Gate 11 - compatibility, packaging, and release

Required matrix:

| Headset/runtime | Level |
|---|---|
| Quest 3 + wired Meta Quest Link OpenXR | Required primary |
| Quest 3 + Air Link OpenXR | Required before user release if available |
| Quest 3 + Virtual Desktop/VDXR | Best effort, standard OpenXR path |
| Reverb G2 + existing OpenVR/SteamVR | Preserve proven fallback |
| Reverb G2 + SteamVR OpenXR | Best effort |
| Reverb G2 + legacy WMR OpenXR on Windows 11 23H2 | Best effort only |

Microsoft removed WMR from Windows 11 24H2 and newer. On modern Windows, the practical
G2 compatibility route is SteamVR through the
[Oasis driver](https://store.steampowered.com/app/3824490/Oasis_Driver_for_Windows_Mixed_Reality/),
which can serve OpenVR and OpenXR applications. The old WMR path remains relevant only
to machines deliberately staying on a supported older Windows release. See
[Microsoft's WMR lifecycle notice](https://learn.microsoft.com/en-us/windows/whats-new/deprecated-features).
This development PC reports Windows 25H2/build 26200, so its G2 test route cannot rely
on the removed legacy WMR stack.

Release contents:

- x86 ASI;
- x64 host;
- OpenXR loader under its own license if redistribution is chosen;
- scripts/config/docs;
- no Rockstar assets;
- no OpenXR runtime;
- no closed reference-mod binaries;
- exact third-party notices and source provenance.

---

## 10. Build and launch workflow

### Product build

`scripts/build-product.ps1` should:

1. configure and clean-build Win32;
2. run the complete Win32 test catalog;
3. configure and clean-build x64;
4. run the complete x64 test catalog;
5. verify PE machine types;
6. hash the produced artifacts;
7. stage to `out-product`, never directly to the game folder.

Pin a reviewed Khronos OpenXR SDK/loader release. This project now pins the current
`release-1.1.61` checkout at commit
`5267613edf3d937e3d77556a106a65c2f82b25c6`; the build and live Meta initialization
both passed. Upstream:
[Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK).

### OpenXR launch

`scripts/run-openxr.ps1` should:

1. verify backend is `openxr`;
2. verify the active x64 runtime manifest and runtime DLL exist;
3. verify Quest Link/headset readiness without changing the user's active runtime;
4. start the x64 host;
5. wait for `XRHost READY` with a bounded timeout;
6. deploy the exact x86 ASI build;
7. start GTA offline;
8. supervise both processes;
9. stop the host when GTA exits;
10. leave a run folder containing both logs and build identities.

The script must not silently launch SteamVR for the Quest/OpenXR route.

### OpenVR launch

Keep the existing build/deploy/run script as the fallback. Later rename or wrap it
clearly as `run-openvr.ps1`; do not remove it during OpenXR development.

---

## 11. Required logs

Game log examples:

```text
Build: asi=... git=... machine=x86 backend=openxr
Bridge: hostEpoch=... abi=... heartbeatAgeMs=...
Pose: seq=... ageMs=... predictedTime=... valid=...
GpuBridge: route=d3d11-nt-imported-vulkan adapterLuid=... sets=3
StereoPair: sourceFrame=... tick=... pose=... L=1 R=1 sameTick=1
StereoReject: reason=...
```

Host log examples:

```text
Build: host=... git=... machine=x64
XRHost: runtime=... system=... loader=...
XRHost: adapterLuid=... viewSizeL=... viewSizeR=...
Actions: profileLeft=... profileRight=...
BridgeFrame: set=... sourceFrame=... pose=... ageMs=... fenceWaitUs=...
XRSubmit: frame=... layers=... result=...
XRReject: reason=...
```

Rate-limit normal logs. Always log state transitions and the first occurrence of each
rejection reason.

---

## 12. Safety and rollback

- Offline/singleplayer only.
- One behavior change per test build.
- Default stereo remains `0` until a mode passes its headset gate.
- Keep Mode 14 as the known fusion fallback.
- Keep OpenVR selectable throughout OpenXR development.
- Host loss must never block the GTA render thread indefinitely.
- All shared-resource waits are bounded.
- A runtime, adapter, ABI, shader-contract, or game-version mismatch fails closed with
  one explicit log reason.
- No OpenXR/OpenVR call from `CopyMat`.
- No compositor frame loop on a second in-game thread.
- No script native from `EndScene`.
- No cross-thread reuse of GTA render functions.
- Do not apply the pre-sync Git stash wholesale over `8fe072a`; compare individual
  hunks only if a missing fix is identified.

Emergency recovery:

```text
1. Stop GTA.
2. Set gtaiv_dxvk_vr.stereo to 0.
3. Set gtaiv_dxvk_vr.backend to openvr or off.
4. Restart with the known OpenVR or flat script.
```

---

## 13. Concrete session queue

Do these in order. Stop after each headset result and update
`docs/CURRENT-STATE.md`.

1. Mode 24 OpenVR test and decision.
2. Quest 3 x64 OpenXR calibration-grid host, SteamVR closed.
3. Cross-architecture protocol tests.
4. Replacement x86-to-x64 GPU bridge live test.
5. Same-frame stereo pair through OpenXR while standing.
6. Moving/turning/driving stereo test.
7. Exact camera pose/FOV and translation test.
8. Quest Touch/XInput and haptic test.
9. UI-state probe plus pause/map/loading/phone quad transition.
10. Head/body decoupling and aim-camera test.
13. Wired Quest performance pass, then Air Link pass.
14. Reverb G2 OpenVR regression; SteamVR OpenXR/Oasis best-effort test when hardware is
    available.
15. Packaging, clean-machine preflight, and rollback drill.

---

## 14. Definition of Done

The Quest 3 OpenXR milestone is done only when:

- GTA still uses stock DXVK 3.0.2 x86 and the ASI is PE32;
- `gtaiv_xr_host.exe` and its loader are verified x64;
- Quest 3 runs through the Meta OpenXR runtime with SteamVR closed;
- both eyes come from one GTA simulation tick and one accepted pose transaction;
- stereo fuses, has correct near/far parallax, and does not temporally jump while
  turning, walking, or driving;
- HMD orientation and translation are independent of body heading;
- pause/map/phone/menu/loading remain usable;
- stale or incomplete eye frames are rejected;
- the shipping pixel path is GPU-only;
- build, deploy, launch, logs, kill switch, and rollback are reproducible;
- the current OpenVR path still builds and runs as fallback;
- G2 compatibility is attempted and documented without blocking Quest 3 release on an
  officially removed WMR runtime.

Motion-controlled weapons, full hand IK, HUD extraction, native 72/90 Hz in every scene,
and every third-party OpenXR runtime can follow as later milestones. They must not be
used to redefine mono, alternating-eye, or a cinema quad as completed world stereo.
