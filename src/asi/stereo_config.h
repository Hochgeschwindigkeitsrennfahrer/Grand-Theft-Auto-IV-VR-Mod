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
  // RESULT 2026-07-24: vsCallsTotal ~380/frame but vsCallsR=0 — the re-executed
  // phase objects are dispatch stubs; ALL uploads flow through ONE Rage wrapper
  // call site (exeRva 0x2C73E) on ONE thread. The real draws happen during the
  // BUILD walk (mode 18's inline-exec sites), not in our re-run.
  ExecViewConstDual = 24,
  // Mode 20 v1 machinery (BuildRootA x2 per frame = real double render, 50 FPS,
  // no jumping) + the TWO camera levers it was missing: shift the manager cam
  // matrices between the walks AND arm the VS-constant view-translate during
  // walk 2 (no double shift possible: detection keys on the LEFT cam pos).
  // RESULT 2026-07-24: stable but vsCallsR stayed ~1 — BuildRootA is not the
  // draw path either. KEY: uploads run on a DEDICATED RENDER THREAD (tid A)
  // while BuildRootA+ExecRoot share the game thread (tid B). And the stray
  // uploads that hit our window DID match+translate (view @c4, viewProj @c8,
  // row-vector, 16-register blocks) — the VS detection works.
  BuildDualViewShift = 25,
  // Temporal stereo on the REPLAY thread: native BuildRootA x1, EndScene flips
  // a VS-const view-translate arm every other frame, captures L/R canvases
  // (mode 14 geometry). Double-record + speculative object patches froze
  // (2026-07-24) — stripped. Same cadence as mode 14; eye shift where draws
  // actually read matrices. Same-frame double-replay remains the next lever.
  RecordDualReplayShift = 26,
  // READ-ONLY: hook VsParent candidate function starts (from Mode 26 stack scan),
  // WAS: count VsParent candidates per EndScene. RESULT: Cand37BD0 wrong ABI;
  // Cand4D8F85 never called. Mode 29 reused same FindFnStartNear → crash after
  // load. Hooks DISABLED; Mode 27 aliases Mode 26. Use stereo file 26.
  ReplayRootProbe = 27,
  // WAS: hook VS wrapper @0x2C6AC for same-frame dual. Headset 2026-07-24: crash
  // right after load screen — wrap hook DISABLED. Mode 28 now aliases Mode 26
  // (temporal) until a safer same-frame root is found. Use stereo file 26.
  SameFrameWrapDual = 28,
  // WAS: count-only VsParent prologue hooks (FindFnStartNear on Mode 26 mid-fn
  // rets). Headset 2026-07-24: crash after load — Cand37C01 resolved to 0x37BD0
  // (same wrong-ABI site as Mode 27), SEH then process death. Hooks DISABLED;
  // Mode 29 aliases Mode 26. Use stereo file 26.
  VsParentCountProbe = 29,
  // Mode 23 same-frame dual (proven crash-free, no jump) + DEVICE-SIDE VS
  // view-translate before the Right pass (Get/SetVertexShaderConstantF on the
  // live device — NOT upload-hook, NOT prologue hooks). Mode 24's vsCallsR=0
  // proved re-exec issues zero SetVSConstF; if draws still read device regs,
  // this supplies the missing Right eye. Soft-start 45 EndScenes (pair-hold
  // temporal), then dual; StereoDiff≈0 or SEH → permanent pair-hold fallback
  // (Mode 26 path). Never hooks 0x2C6AC / 0x37BD0. Kill → stereo 26.
  PhaseDualDeviceVs = 30,
  // Same-frame stereo WITH real parallax: soft-start = Mode 30 pair-hold, then
  // discover a ~1×/frame walker on the VsRet thread via SetVSConstF stack
  // histograms (NOT FindFnStartNear on blind mid-rets, NOT 0x2C6AC/0x37BD0).
  // Count-only hook ≥45 EndScenes (avg entries ∈ [0.8,4] + thiscall byte-sig)
  // BEFORE dual. Dual = walk×2 with VS view-translate ONLY between passes
  // (no CCam+VS stack), capture→pair-hold promote. Discovery/dual fail →
  // permanent Mode 30 pair-hold. Kill → stereo 30 or 26.
  // RESULT 2026-07-24: discover failed (stack scan too deep in callee). Use 32.
  SameFrameReplayDual = 31,
  // Mode 31 fix: VsParent stack collected INSIDE HookSetVSConstF (same depth as
  // Mode 26 NoteVsRet), VirtualQuery + ABI classify (thiscall/stdcall/ecx+edx),
  // count-only ≥45 ES avg∈[0.8,4] before dual. Dual = L capture → VS translate
  // R → pair-hold promote. Fail → write stereo file 30 + pair-hold. Kill → 30/26.
  SameFrameVsParentDual = 32,
  // Mode 32 lesson: Soft Discover ran before VsRet traffic → empty hist → early
  // seed of stackarg prologues (0x32A40/0x372B0). Mode 33 WAIT until live
  // VsParent samples appear (slots ~10–32), then ONLY hook CC-padded zero-arg
  // thiscall; count gate; dual = walk×2 + VS translate pass2 + pair-hold
  // promote. No CCam+VS stack. SEH / no cand → write stereo=30. Kill → 30/26.
  SameFrameLateVsParentDual = 33,
  // Mode 33 finding: VsParent mid slots are epilogues (5F C2 10 00), not walkers.
  // Mode 34: VsRet(0x2C73E) stack → fn starts → COUNT only. DUAL DISABLED:
  // 0x4DDAD0 COUNT ok but vsPatch=0/vsCallsR=0 (no parallax). Forbidden list
  // includes 0x4DDAD0. Fail/COUNT-ok → write stereo=30. Kill → 30/26.
  SameFrameVsRetCallerDual = 34,
  // Mode 30 pair-hold rendering + FusionFix-style FOV recompute-site hook:
  // chain-hook the cam process CALL that finalizes CCam+0x60 (same AOB as
  // FusionFix PREF_CUSTOMFOV), then ADD degrees from gtaiv_dxvk_vr.fovadd.
  // Additive (not absolute pin) preserves aim/sniper zoom. Mode 17 wrote
  // mat+0x50 (=CCam+0x60) after CopyMat and lost to recompute — this writes
  // AFTER the recompute. Kill: stereo=30 + delete fovadd. Not FusionFix menu.
  FovRecomputeSite = 35,
  // Mode 35 + canvas reads TRUE CCam FOV after fovadd (not stale D3DTS_PROJECTION).
  // Shrinks black bars without CanvasZoom. Protect Mode 35 as fallback.
  // Kill: stereo=35 (keep fovadd) or stereo=30 + delete fovadd.
  FovRecomputeTrueCanvas = 36,
  // Mode 36 comfort retune: SAME true-FOV canvas + Mode30 pair-hold, but
  // softer fovadd (deploy ~18), canvas max-dim cap (~1536), pair-latched
  // gameTan for L+R rects, WorldScale lever for "huge world" (not IPD).
  // Protect: Mode 36 / 35. Kill: stereo=30 + delete fovadd.
  FovCanvasComfort = 37,
  // Mode 37 + Luke Ross AER lesson: stamp each eye RT with the HMD pose from
  // capture time and Submit via Submit_TextureWithPose so SteamVR can reproject
  // the stale eye. Not AER v2 optical-flow. Kill: stereo=37 or 30.
  AerPoseSubmit = 38,
  // Mode 37 pair-hold / true-FOV canvas, but caps fovadd at 12 degrees. Mode 38
  // proved pose-aware Submit does not cure the remaining temporal jump; this is
  // the safer comfort experiment: retain fusion while reducing the wide-FOV
  // crop/fill and render cost that made Modes 36/37 jumpier. Kill: 38/37/30.
  FovCanvasLowMotion = 39,
  // Mode 37 true-FOV canvas + pair-hold, with an honest temporal safety valve:
  // if the cached HMD pose moves materially between the L and R game frames,
  // rebuild BOTH eye canvases from the current R frame before promotion. This
  // temporarily becomes mono (not same-frame stereo), so it removes stale-eye
  // jump on rapid head turns without unsafe replay-thread hooks. Kill: 37/30.
  FovCanvasMotionGuard = 40,
  // Mode 40's true-FOV pair-hold and rapid-motion mono guard, plus a READ-ONLY
  // trace of the replay-thread chain behind SetVSConstF's live 0x2C73E return.
  // It installs no game-function hook and never duplicates draws. It maps a
  // future same-tick seam; it is NOT distinct-eye same-frame stereo. Kill: 40/37/30.
  ReplayCallChainProbe = 41,
  // Mode 41's frequent 0x30D13 stack node resolves near 0x309D0, but the original
  // decoder only recognized E8 and FF 15 calls. Mode 42 remains READ-ONLY and
  // decodes the exact preceding FF /2 indirect-call form (including vtable calls)
  // before any game-function hook can even be considered. It keeps Mode 40's
  // temporal motion guard; it never calls/replays a game function or changes view.
  ReplayOwnerCountProbe = 42,
  // Mode 40's same visual safety valve, but when it fires, copy the current R
  // frame directly into the already-submitted L/R canvases. This avoids the
  // extra hold-RT recanvas and pair-promotion copies during a rapid HMD turn.
  // It remains temporary mono, not same-frame distinct-eye stereo. Kill: 40/37/30.
  FovCanvasMotionGuardFast = 43,
  // Mode 43 visual path plus a hard 1536 eye-RT allocation lock.  It rejects
  // harmless CCam tangent/publish wobble instead of releasing/recreating all
  // four submit/hold textures.  True FOV, pair-hold, and the fast motion guard
  // remain unchanged; this is an RT-stability/FPS experiment, not true stereo.
  FovCanvasMotionGuardRtLock = 44,
  // Mode 44 visual path plus a second application of the existing safe
  // ped-eye + relative-HMD CopyMat matrix immediately after the verified CCam
  // FOV recompute site. This makes the render-time view head-owned when Rage
  // performs a late camera update, without changing gameplay collision/culling,
  // touching VS constants, or replaying a draw. Kill: 44/37/30.
  HeadOwnedCamSpike = 45,
  // Mode 45's late head-owned refresh, but preserves the HMD's complete rigid
  // right/forward/up basis instead of rebuilding it from forward + world-up.
  // This makes physical roll tilt the horizon correctly and keeps the render
  // view head-owned; gameplay camera/collision, FOV canvas, RT lock, and
  // temporal motion guard are unchanged. Kill: 45/44/37/30.
  HeadOwnedCamFullPose = 46,
  // Mode 45's exact world-up-leveled reconstruction, retained as the safe
  // recovery from the failed inverted-pitch experiment. It never writes the
  // unsafe non-level HMD right/up axes from Mode 46.
  HeadOwnedCamLeveledPitchFlip = 47,
  // Mode 47 plus pitch-stable leveled basis: when HMD forward is near vertical
  // the world-up cross product degenerates and snaps to rx=(1,0,0), causing
  // look-up warp. Mode 48 keeps the last stable right/up and re-orthogonalizes
  // with the current forward. Also refreshes CopyMat immediately before each
  // temporal eye capture so a late follow-cam overwrite cannot stale the BB.
  // Still no Mode-46 full HMD-basis write. Kill: 47/45/44/37/30.
  HeadOwnedCamPitchStable = 48,
  // Mode 48 pitch-stable basis + pre-capture refresh, but Mode-45 pitch sign
  // (leveledPitchFlip OFF — flip=1 in 47/48 was reversed). Yaw couples to live
  // ped heading: body turns carry the view; HMD yaw is delta on top. Ped eye +
  // F9-relative 6DoF unchanged. Kill: 48/45/47/44/37/30.
  HeadOwnedCamPedCoupled = 49,
  // Mode 49 visuals/stability but motion-guard OFF — always promote distinct
  // temporal L/R when ipd>0. Logs StereoAudit (L≠R diff) each pair. Kill: 49/45.
  HeadOwnedCamStereoAlways = 50,
  // Mode 50 + capture-time Submit_TextureWithPose (Luke AER). Kill: 50/49/45.
  HeadOwnedCamStereoAer = 51,
  // Mode 50 + swapped L/R submit (depth inversion test). Kill: 50/49/45.
  HeadOwnedCamStereoSwap = 52,
  // Mode 51 + soft motion guard: 8°/6cm → keep distinct L/R + AER (no flatten);
  // 15°/12cm → full mono fallback. Kill: 51/50/49/45.
  HeadOwnedCamStereoSoftGuard = 53,
  // Direct-OpenXR same-frame candidate. Reuses Mode 23's stable per-phase
  // Execute x2 boundary, but patches only CTAB-declared gWorldViewProj at each
  // RIGHT-eye Draw* call. Every constant is restored after that draw. The pair
  // is publishable only when L/R draw sequences match exactly, every declared
  // WVP contract is accounted for, and at least one gWorld factor proves the
  // camera transform. This mode is non-default and fail-closed.
  OpenXrSameFrameWvp = 54,
  // Direct-OpenXR usability baseline. Applies one center-eye HMD camera,
  // captures the completed GTA backbuffer once, and presents that same angular
  // world image through both OpenXR projection views. This is
  // immersive, head-tracked mono: no binocular depth claim and no draw replay.
  OpenXrImmersiveMono = 55,
  // Direct-OpenXR true-stereo candidate. Ports the newest upstream's guarded
  // DrawScene x2 seam: GTA renders a matched L/R pair from the same cached
  // OpenXR pose, and the sidecar publishes it only as a distinct-eye CPU
  // mailbox transaction. It never calls OpenVR or SteamVR.
  OpenXrDrawSceneStereo = 56,
  // Direct-OpenXR temporal full-frame stereo fallback. Captures completed
  // native backbuffers on alternating L/R game frames using the exact OpenXR
  // eye poses, then publishes only completed distinct-eye pairs to the x64
  // host. This is AER rather than same-tick stereo and never calls OpenVR.
  OpenXrTemporalStereo = 57,
  // Mode 120 CLEAN rewrite (LKG / last-good smooth): Mode50 head-owned + RT lock +
  // true-FOV canvas + synced DrawScene×2 every N with HOLD/hitchcut (Praydog sequential).
  // look-move ON (HMD yaw→ped; atan2(-fx,fy)); pedCoupled OFF; motion-guard OFF;
  // NO AER temporal as 3D path; NEVER BuildRootA×2; NO FPS HUD. dualn default 2.
  // Kill: 51/45/0. Do NOT mutate — experiments go to 121+.
  CleanDualLookMove = 120,
  // Mode 121 EXPERIMENT: identical to Mode 120 but forces dualn=3 (lighter HOLD).
  // 121+ = experiments. Kill/fallback: 120 (good) / 51 / 45 / 0.
  CleanDualLookMoveDualN3 = 121,
  // Mode 122 PERF: Mode120 path + look/pose-budget HOLD (skip dual while turning or
  // after WaitGetPoses ≥18ms). dualn force 4. Attacks apartment look-stutter/pop-in.
  // Kill/fallback: 120.
  CleanDualPerfLookHold = 122,
  // Mode 123 VOLLBILD: Mode120 + cover-matched fovadd (H≈100% fill, mild V bars OK).
  // Praydog-inspired: match gameTan to HMD coverTan H — no SoftInset, no Mode83 crop.
  // Kill/fallback: 120.
  CleanDualFillMatch = 123,
  // Mode 124 ZOOM: Mode120 + dezoom-biased F7 ladder default (MatchH6 / Window0 zone).
  // Kill/fallback: 120.
  CleanDualZoomScale = 124,
  // Mode 125 COMBO: Mode122 look/pose HOLD + Mode123 cover-matched fill.
  // dualn force 4. Kill/fallback: 120.
  CleanDualPerfFillCombo = 125,
  // Mode 126 FPS+LIVELOOK: Mode123 cover fill + WaitGetPoses POSEHOLD (skip dual on
  // ≥16ms stalls — fixes exploring 60–70 half-lock) + during look/pose skip: live BB
  // → both eyes (NOT Mode122 stale stereo HOLD that snapped). dualn file/default 2.
  // Stronger dezoom bias: Window0 start + stereoscale 135 after cover-match.
  // Kill/fallback: 120. OpenVR 2.12.14. No FPS HUD.
  CleanDualFpsLiveLook = 126,
  // Mode 127 FPS+LIVELOOK TIGHT: same as 126 (fill + LIVELOOK) but tighter POSEHOLD
  // (≥12ms skip 5 / ≥24ms skip 8 HARD), forced dualn=3, hitchcut 32ms.
  // Log proof Mode126: WaitGetPoses half-lock still primary (avg stall ~21ms, AppFPS
  // dips 50–70 correlated with pose_stall; soft 16ms/3 duals not enough).
  // Kill/fallback: 120. No FPS HUD. Mode 120/126 paths untouched.
  CleanDualFpsPoseTight = 127,
  // Mode 128 FPS NO-LIVELOOK: Mode127 fill + tight POSEHOLD + dualn=3 + hitch 32,
  // but NO look-rate LIVELOOK (mono BB→both eyes ghosted). Pose stall → HOLD last
  // stereo (skip dual only; no mono/stereo flicker). Kill/fallback: 120.
  // Mode 120/127 code paths untouched.
  CleanDualFpsHoldStereo = 128,
  // Mode 129 L/R SYNC: Mode128 fill/tight/dualn=3 — on look force same-tick dual
  // every frame (no temporal L≠R desync); calm = dualn=3 HOLD; high pose_ms →
  // mono BB both eyes with ≥200ms hysteresis (no mismatched HOLD pair). Kill: 120.
  // Mode 120/128 paths untouched.
  CleanDualLrSync = 129,
  // Mode 130 MONOLOOK: Mode128 fill/tight/dualn=3 — on look (yaw/pitch Δ) submit
  // identical BB→L and R (one capture); stay mono ≥350ms after look settles
  // (no frame-by-frame mono↔stereo flip). Calm = dualn=3 HOLD (~90). Pose stall
  // also extends mono hyst. Kill: 120. Mode 120/129 paths untouched.
  // Why not 129: same-tick dual is still L then R DrawScene with game time
  // advancing between eyes → diplopia on head turn even when LOOK-FORCE fires.
  CleanDualMonoLook = 130,
  // Mode 131 MONOLOOK PITCH/FPS: Mode130 ghosting fix kept (identical BB both
  // eyes on look) but: higher pitch thresh than yaw; ≥600ms hyst; sustained look
  // before enter mono; EndScene BB→L+R only every 3rd mono frame; calm pose →
  // HOLD last stereo (not mono StretchRect); dualn=3. Kill: 120.
  // Mode 120/130 paths untouched. OpenVR 2.12.14. No FPS HUD.
  // FAILED headset test: every-3rd BB + POSEHOLD-CALM starved HMD while Present~90.
  CleanDualMonoLookPitch = 131,
  // Mode 132 HMD-EVERY: Mode131 pitch-stable monolook (2.5°/sustain3/600ms) but
  // StretchRect BB→L+R EVERY EndScene during mono (no every-3rd HMD starve).
  // Calm: dualn=3 HOLD last stereo like 128. Pose stall → HOLD last stereo
  // (re-submit each Present — never skip multi-frame without submit content).
  // Kill: 120. Mode 120/130/131 paths untouched. OpenVR 2.12.14. No FPS HUD.
  CleanDualMonoLookHmdEvery = 132,
  // Mode 133 HMD-CHEAP: Mode132 pitch-stable every-frame mono, but ONE StretchRect
  // BB→L then Submit same tex for L+R (OpenVR allows; halves copy vs 132 BB×2).
  // Keeps HMD every-frame fresh (no every-3rd starve; no stale stereo HOLD on look).
  // Calm: dualn=3 HOLD. Kill: 120. Mode 120/130/131/132 paths untouched.
  // OpenVR 2.12.14. No FPS HUD.
  // FAILED headset 2026-07-27: same-tex L+R Submit → extreme jumping, fusion gone.
  CleanDualMonoLookHmdCheap = 133,
  // Mode 134 BACK-132: exact Mode 132 look path (BB→L AND BB→R every monolook frame;
  // 132 pitch/hyst 2.5°/sustain3/600ms). NOT same-tex Submit. 133 failed; 134 is the
  // safe default so user is never stuck on 133. Kill: 120. Paths 120–133 untouched.
  // OpenVR 2.12.14. No FPS HUD.
  // FAILED headset 2026-07-27: dualn=3 HOLD + incomplete monolook → HMD freeze, desktop fine.
  CleanDualMonoLookBack132 = 134,
  // Mode 135 ALWAYS-FRESH: never submit previous-frame eye textures while view moves.
  // Looking (low thresh): monolook BB→L AND BB→R every frame (132 style; NOT same-tex).
  // Calm: same-tick dual every frame (everyN=1). POSEHOLD → fresh monolook that frame
  // (never bare HOLD). Accept FPS cost to kill HMD freeze. Kill: 120.
  // Paths 120–134 untouched. OpenVR 2.12.14. No FPS HUD.
  CleanDualAlwaysFresh = 135,
  // Mode 136 LATE-LATCH: Mode 120 content (dualn=2, HOLD, NO monolook switch) + every
  // Submit uses Submit_TextureWithPose with HMD pose sampled immediately before Submit
  // (not capture-time AER). SteamVR can reproject between content frames → smoother look.
  // Kill: 120. Paths 120–135 untouched. OpenVR 2.12.14. No FPS HUD.
  CleanDualLateLatch = 136,
};

// Modes whose compositor, pose, FOV and frame transport are the separate x64
// OpenXR host. These modes must never consult OpenVR at runtime.
bool IsOpenXrDirectMode(StereoMode mode);

// Mode 40–49: rapid HMD delta → temporary mono pair. Mode 50+ never flattens.
bool UsesMotionGuardStereo(StereoMode mode);
// Mode 50–53 and direct-OpenXR Mode 57: distinct-eye temporal pairs.
bool UsesStereoAlwaysDistinct(StereoMode mode);
// Mode 38/51/53: Submit_TextureWithPose per eye.
bool UsesAerPoseSubmit(StereoMode mode);
// Mode 49 and 50+: ped yaw coupling (body turn carries view).
bool UsesPedCoupledYaw(StereoMode mode);
// Mode 48–53: refresh CopyMat immediately before each eye capture.
bool UsesPreCaptureCamRefresh(StereoMode mode);
// Mode 44/45/47–53: locked 1536 RTs + 5% FOV publish gate.
bool UsesRtLockFovGate(StereoMode mode);
// Mode 120–136 — clean DrawScene×2 + look-move family (120=LKG; 121+=experiments).
bool IsCleanDualLookMove(StereoMode mode);
bool IsCleanDualPerfLookHold(StereoMode mode);  // 122 or 125 (stale HOLD)
bool IsCleanDualFillMatch(StereoMode mode);     // 123, 125–135 (not 136 — 136 = Mode120 scale)
bool IsCleanDualZoomScale(StereoMode mode);     // 124
bool IsCleanDualPerfFillCombo(StereoMode mode); // 125
bool IsCleanDualFpsLiveLook(StereoMode mode);   // 126 or 127 (LIVELOOK path)
bool IsCleanDualFpsPoseTight(StereoMode mode);  // 127–135 (tighter POSEHOLD)
bool IsCleanDualFpsHoldStereo(StereoMode mode); // 128 only (pose HOLD last stereo)
bool IsCleanDualLrSync(StereoMode mode);        // 129 only (look force dual + pose mono hyst)
bool IsCleanDualMonoLook(StereoMode mode);      // 130 only (look→mono BB ≥350ms hyst)
bool IsCleanDualMonoLookPitch(StereoMode mode); // 131 only (pitch-stable + BB/3 — FAILED)
bool IsCleanDualMonoLookHmdEvery(StereoMode mode); // 132 or 134 (pitch-stable + every-frame BB×2)
bool IsCleanDualMonoLookHmdCheap(StereoMode mode); // 133 only (FAILED same-tex Submit)
bool IsCleanDualMonoLookBack132(StereoMode mode);  // 134 only (=132-safe; FAILED HOLD freeze)
bool IsCleanDualAlwaysFresh(StereoMode mode);      // 135 only (everyN=1; never bare HOLD)
bool IsCleanDualLateLatch(StereoMode mode);        // 136 only (Mode120 content + TextureWithPose late)

// Mode 123: once cover FOV known, set fovadd so fillH≈100% (no SoftInset).
void TryApplyCoverMatchedFovAdd();

StereoMode GetStereoMode();
void ReloadStereoMode();
// Persist kill/fallback mode to gtaiv_dxvk_vr.stereo (next launch safe default).
void WriteStereoModeFile(int mode);

float GetStereoSepMeters();
void PollIpdScaleHotkey();
float GetStereoIpdScale();

// 6DoF lean only (file gtaiv_dxvk_vr.scale). Does NOT multiply stereo IPD.
float GetWorldScale();
// F7: cycle visual WorldScale presets (fovadd + stereoscale). Persist index to
// gtaiv_dxvk_vr.worldscale. Default "Open12" — DEZOOM via LOWER fovadd (less
// StretchRect crop) + higher stereoscale. Higher fovadd = more crop zoom-IN.
void PollWorldScaleHotkey();

// Soft stereo disparity multiplier (percent). Independent of 6DoF lean.
// File: gtaiv_dxvk_vr.stereoscale — default 115. F6 cycles. Cap keeps fusion safe.
// F7 WorldScale presets also write this.
float GetStereoScale();
void PollStereoScaleHotkey();

// True for Mode 4 and Modes 13..17 (frame-alternating eye capture + Submit).
bool IsTemporalStereoMode(StereoMode mode);

// True for Modes 14..17 and 20 (angle-correct per-eye canvas Submit).
bool UsesAngleCorrectCanvas(StereoMode mode);

// --- Optional config files next to the ASI (read once at startup, log on use) ---

// gtaiv_dxvk_vr.eyefwd: eye-forward offset in cm (default 42). Places cam through
// skull/hair. With PedHide on, try lower values (12–20) for less turn-swing.
float GetEyeForwardMeters();

// gtaiv_dxvk_vr.camoff: "x y z" cm — Inspiration FirstPerson.ini FPX/FPY/FPZ style.
// X = right, Y = forward (away from face), Z = up. Default 0 0 0.
void GetCamOffsetMeters(float* outRight, float* outForward, float* outUp);

// gtaiv_dxvk_vr.pedhide: 1 = hide head/hair/teeth/face (Inspiration approach via
// CE SetDraw helper, NOT EndScene natives). 0 = off. Default 1 if file absent.
bool IsPedHideEnabled();

// gtaiv_dxvk_vr.movemode: 1 = do NOT force ped heading to HMD view — the game maps
// stick input relative to the (HMD-overridden) camera, enabling strafe/backpedal.
// 0/absent = legacy head-directed walk.
bool IsFreeMoveEnabled();

// gtaiv_dxvk_vr.vehfollow: 1 = camera yaw follows the vehicle heading (free look on
// top), 2 = same with inverted sign (if 1 turns the wrong way). 0/absent = off.
int GetVehicleFollowMode();

// gtaiv_dxvk_vr.fovpatch: "<offsetBytes> <scalePercent>" for Mode 17.
bool GetFovPatchConfig(int* offsetBytes, float* scale);

// gtaiv_dxvk_vr.fovadd: degrees to ADD at CCam+0x60 after cam FOV recompute
// (FusionFix-style; Mode 35/36/37/38/120). Live (F7 can change). Clamp 0..40.
// Mode 120 default via worldscale "Open12" = 12 (less H crop than old Out22/28).
float GetFovAddDegrees();

}  // namespace asi
