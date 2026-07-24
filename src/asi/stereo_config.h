#pragma once

#include <cstdint>

namespace asi {

// File: gtaiv_dxvk_vr.stereo — digit / "10"-"13". Kill-switch: 0
enum class StereoMode : int {
  Off = 0,
  Probe = 1,
  DualSameCam = 2,
  DualIpd = 3,
  DualRtSubmit = 4,       // temporal L/R (legacy)
  PhaseProbe = 5,
  SameFrameDual = 6,
  GBufferRtDual = 7,      // same-frame RT dual (original Mode 7 — not exec-dual)
  FusionSwap = 8,
  VtableProbe = 9,
  ExecuteDual = 10,
  BuildExecDual = 11,     // BLACKSCREEN — do not use
  D3dCamDual = 12,
  // L4D2VR/BotW-oriented fusion: temporal dual eye + cover FOV + TextureBounds,
  // IPD/VRScale like L4D2 (defaults: scale=100%, sep≈HMD IPD).
  StereoFusion = 13,
  // Temporal dual eye + angle-correct per-eye canvas: game image is placed at its
  // TRUE angular position inside each eye's HMD frustum (black border around it).
  // No engine FOV change, no Submit UV tricks — fixes the per-eye stretch that
  // broke fusion with nullptr bounds. Optional gtaiv_dxvk_vr.fov (horiz degrees).
  // CONFIRMED by headset test 2026-07-24: fusion + correct proportions.
  GeometryCanvas = 14,
  // Mode 14 + projection widen toward HMD cover FOV via the SetTransform hook ONLY
  // (no VS-constant scans — those cost ~45 FPS). The canvas reads the widened
  // tangents live and adapts. Diagnostic outcomes: smaller border + correct
  // proportions = Rage consumed it; vertical stretch = Rage ignored it (use 14).
  // TESTED 2026-07-24: vertical stretch → Rage IGNORES D3DTS_PROJECTION. DEAD.
  CanvasWide = 15,
  // Mode 14 rendering + READ-ONLY probe: log stable float candidates around the
  // CopyMat camera matrix to locate the engine FOV field (target ~58.7 deg vertical
  // or ~1.02 rad). Zero writes — cannot freeze.
  CamFovProbe = 16,
  // Mode 14 rendering + engine FOV write at a VERIFIED offset from Mode 16.
  // Config file gtaiv_dxvk_vr.fovpatch: "<offsetBytes> <scalePercent>", e.g. "104 160".
  // Written on the game thread right after CopyMat (unlike the old CCam+0x60 freeze,
  // which was a blind offset). Kill: delete file or stereo=14/0.
  // RESULTS 2026-07-24: +80 works but is recomputed per frame by the game (oscillates
  // against our write); +208 is inert. Engine FOV needs the recompute-site hook.
  CamFovWrite = 17,
  // READ-ONLY probe: log return addresses of the PhaseA/C/DrawScene Build+Execute
  // hooks to locate the render-root (the Rage "RenderView" equivalent) for a true
  // same-frame dual render. Mono submit only, ~1 minute run.
  // RESULT 2026-07-24: three caller functions found (all thiscall, no stack args):
  //   0x8F73B0 exec dispatcher, 0x8F7F00 PhaseA build root, 0xA87680 PhaseC+DrawScene
  //   build+exec loop. See CURRENT-STATE.md.
  RenderRootProbe = 18,
  // Passthrough-hook the three mode-18 candidate roots, count calls per frame and
  // log the call order. Identifies THE once-per-frame render root for the
  // same-frame dual render. Mono submit, ~1 minute run.
  // RESULT 2026-07-24: per frame exactly ExecRoot 1x -> BuildRootA 1x -> 13x phase
  // walk inside BuildRootA. BuildRootA this = global 0x118D7F0 (mov ecx before call).
  RootDispatchProbe = 19,
  // TRUE same-frame stereo: hook BuildRootA (0x8F7F00, thiscall, no args) and run
  // the ENTIRE 13-phase frame build+draw twice — Left eye, capture BB into the
  // Left canvas, then Right eye, capture Right. Both eyes share one game tick →
  // kills the temporal L/R jumping by construction. Costs ~2x render time.
  // Non-reentrant (dual-call AFTER the native walk completed), unlike dead mode 11.
  // RESULT 2026-07-24: v1 (build-only) = mono (Rage double-buffers lists; exec draws
  // them elsewhere). v2/v3 (exec re-call from the build context) = EXCEPTION even
  // with FrameA setup → build and exec live on DIFFERENT THREADS. Dead end as-is.
  SameFrameRootDual = 20,
  // READ-ONLY: on the exec side (render thread), scan the render-manager object
  // (global 0x118D7F0) and the .data section for the CURRENT camera position /
  // view-matrix translation. Finds where the per-frame view matrix is stored so
  // mode 22 can swap it between two exec passes (same lists, two eyes, one thread).
  // Also logs build vs exec thread ids to confirm the threading model.
  // RESULT 2026-07-24: two live cam WORLD matrices in the manager (pos rows at
  // mgr+0xD0 and mgr+0x1D0 = 0x118D8C0/0x118D9C0); no inverse/view copies found.
  ViewMatrixScan = 21,
  // TRUE same-frame stereo on the RENDER THREAD: run ExecRoot twice per frame
  // with the SAME draw lists, shifting the manager cam matrices' position row by
  // the eye delta between passes (view matrix is derived at exec time, not baked
  // into lists). L capture after pass 1, R after pass 2. SEH-guarded; falls back
  // to native mono on exception. Expect ~half FPS, no temporal L/R jumping.
  // RESULT 2026-07-24: CRASH stage 5 (2nd ExecRoot call, AV @0x6E75E3) — the root
  // exec CONSUMES the frame's draw lists; a second root call is use-after-free.
  ExecViewDual = 22,
  // Mode 10 proved the per-PHASE Execute calls (vt[9]) can run twice per frame
  // without crashing — it only lacked a camera change between passes (CCam eye
  // switch does not reach the exec side). Mode 23 = mode 10's per-phase double
  // exec + mode 22's manager-cam-matrix shift between the L and R pass.
  // RESULT 2026-07-24: STABLE (3300+ dual frames, 45 FPS = true double render,
  // jumping gone) but NO fusion — the view transform is baked into the draw
  // lists' shader constants at BUILD time; the manager matrices are inert copies.
  ExecViewPhaseDual = 23,
  // Mode 23 + the missing piece: during the RIGHT pass, hook
  // SetVertexShaderConstantF (draw lists replay constant uploads through D3D9)
  // and translate every 4x4 block that maps the build cam pos onto the view
  // axis (= view/viewProj matrix, either matrix convention) by the eye delta.
  ExecViewConstDual = 24,
};

StereoMode GetStereoMode();
void ReloadStereoMode();

float GetStereoSepMeters();
void PollIpdScaleHotkey();
float GetStereoIpdScale();

// VRScale (L4D2VR): HIGHER → more IPD + 6DoF in game units → world feels SMALLER.
// File: gtaiv_dxvk_vr.scale (percent, 100 = 1.0)
float GetWorldScale();
void PollWorldScaleHotkey();

// True for Mode 4 and Modes 13..17 (frame-alternating eye capture + Submit).
bool IsTemporalStereoMode(StereoMode mode);

// True for Modes 14..17 and 20 (angle-correct per-eye canvas Submit).
bool UsesAngleCorrectCanvas(StereoMode mode);

// --- Optional config files next to the ASI (read once at startup, log on use) ---

// gtaiv_dxvk_vr.eyefwd: eye-forward offset in cm (default 38). Smaller = less
// camera swing when turning the head (comfort), but the ped skull may block view.
float GetEyeForwardMeters();

// gtaiv_dxvk_vr.movemode: 1 = do NOT force ped heading to HMD view — the game maps
// stick input relative to the (HMD-overridden) camera, enabling strafe/backpedal.
// 0/absent = legacy head-directed walk.
bool IsFreeMoveEnabled();

// gtaiv_dxvk_vr.vehfollow: 1 = camera yaw follows the vehicle heading (free look on
// top), 2 = same with inverted sign (if 1 turns the wrong way). 0/absent = off.
int GetVehicleFollowMode();

// gtaiv_dxvk_vr.fovpatch: "<offsetBytes> <scalePercent>" for Mode 17.
bool GetFovPatchConfig(int* offsetBytes, float* scale);

}  // namespace asi
