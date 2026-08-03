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
  // Mode 37 + pose-stamped AER lesson: stamp each eye RT with the HMD pose from
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
  // Mode 50 + capture-time Submit_TextureWithPose (pose-stamped AER). Kill: 50/49/45.
  HeadOwnedCamStereoAer = 51,
  // Mode 50 + swapped L/R submit (depth inversion test). Kill: 50/49/45.
  HeadOwnedCamStereoSwap = 52,
  // Mode 51 + soft motion guard: 8°/6cm → keep distinct L/R + AER (no flatten);
  // 15°/12cm → full mono fallback. Kill: 51/50/49/45.
  HeadOwnedCamStereoSoftGuard = 53,
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
  // Mode 140 EXTERNAL-FP HOST: C06alt FirstPerson owns FP cam + hide + FOV.
  // Our ASI: DrawScene×2 IPD-only dual Submit + HMD→mouse (no full HMD CopyMat).
  // Mode 120 code path untouched. Kill: stereo 0 or 120 (+ restore FP off).
  // Known look issues (kept for A/B): yaw sides inverted; pitch sluggish (frame-skip/cap).
  ExternalFpHost = 140,
  // Mode 141: Mode140 dual+FP + HMD→mouse look fix for FirstPerson (GET_MOUSE_INPUT path):
  // yaw sign un-inverted; every-frame (no skip); higher pitch sens; higher max mouse step;
  // larger delta accept (fast head turns). Kill: 140 / 0 / 120.
  ExternalFpHostLookFix = 141,
  // Mode 142: Mode141 pitch/rate, but opposite yaw sign vs 141 (if 141 still mirrored).
  // Kill: 141 / 140 / 0. Yaw confirmed good; pitch dead when FirstPerson HMD=1 (Oculus).
  ExternalFpHostLookAlt = 142,
  // Mode 143: Mode142 yaw (−) + pitch unlock for FirstPerson mouse path (HMD=0):
  // per-axis clamp (don't drop pitch when yaw spikes); pitch sign flip vs 142; higher pitch.
  // Requires FirstPerson.ini HMD=0. Kill: 142 / 140 / 0.
  ExternalFpHostLookPitch = 143,
  // Mode 144: Mode143 rate/clamp, but Mode142 pitch sign (if 143 looks inverted UD).
  // Kill: 143 / 142 / 0. Confirmed: signs OK; pitch too fast vs yaw; no ped look-move;
  // walk direction hard; camera bob up/down while walking.
  ExternalFpHostLookPitchAlt = 144,
  // Mode 145: Mode144 signs + look_move ON (ped faces HMD → walk in look dir like Mode120)
  // + mouse pitch only (no mouse yaw — ped turn owns LR) + slower pitch + small pitch deadzone.
  // FirstPerson HMD=0. Kill: 144 / 0 / 120.
  ExternalFpHostLookMove = 145,
  // Mode 146: Mode145 look_move + reduced mouse yaw (half) if ped-only yaw undershoots FP cam.
  // Kill: 145 / 144 / 0.
  ExternalFpHostLookMoveYaw = 146,
  // Mode 147: LAST movement A/B before head-hide port.
  // Uncouple cam from body: mouse look = Mode144 signs (slow pitch); ped heading floats
  // ONLY (no matrix rewrite — FP attach cam not yanked). Walk toward look. Stick LR invert fix.
  // Kill: 144 / 0.
  ExternalFpHostSoftMove = 147,
  // Mode 148: Mode147 without stick invert (if 147 stick still wrong).
  // Kill: 147 / 144 / 0.
  ExternalFpHostSoftMoveStick = 148,
  // Mode 150: FP-host dual (like 147 soft move) + native SET_DRAW_PLAYER_COMPONENT hide
  // (ScriptHook-timing goal via NativeInvoke). Keep FirstPerson for cam; test hide port.
  // Kill: 147 / 0. pedhide forced conceptually.
  HeadHideNativeWithFp = 150,
  // Mode 151: OUR cam (Mode120-class DrawScene dual + HMD CopyMat) + native head hide.
  // Rename FirstPerson.asi → .off for this test. Kill: 120 / 0.
  HeadHideNativeOurs = 151,
  // Mode 152: Mode151 + FirstPerson.ini FOV profile for OUR cam:
  // ForwardFOV/RearFOV/FootFOV via gtaiv_dxvk_vr.fpfov (default 90 90 90 — not 111),
  // FPX/Y/Z→camoff, joysens/mousesens files. Absolute CCam FOV write (SET_CAM_FOV style).
  // FirstPerson.asi OFF. Kill: 151 / 120 / 0.
  OursFpFovProfile = 152,
  // Mode 153: Mode152 stack + FP-wide under-publish FOV.
  // Write CCam+0x60 = fpfov (widen engine BB) but lock canvas gameTan to HMD cover
  // (do NOT PublishGameFovFromCCamDegrees). Matches FP-host dezoom feel. Kill: 152/120/0.
  OursFpWideUnderPublish = 153,
  // Mode 154: Mode153 + aspect-correct under-publish fit (tanH/tanV keep BB aspect,
  // scale to touch cover on one axis). Fixes 153 vertical stretch / narrow rooms.
  // Kill: 153 / 152 / 120. Square commandline NOT used (already failed alone).
  OursFpWideAspectFit = 154,
  // Mode 155: Mode154 FOV + ped-fixed eye-center pivot (no 6DoF lean; ped-fwd offset
  // between eyes; ignore look-axis eyefwd). Kill: 154 / 153 / 120.
  OursFpEyeCenterCam = 155,
  // Mode 156: Mode155 with lower eye height (65cm vs 78) + F5 VR eye-canvas maxDim
  // increments (1024…2048 / SteamVR recommended). Kill: 154 / 155 / 120.
  OursFpEyeCenterLowRes = 156,
  // Mode 157: Mode156 + mild under-publish overscan (~6%) to shrink black bars
  // without Mode153 aspect lie. Slight edge crop. Kill: 156 / 154 / 120.
  OursFpMildOverscan = 157,
  // Mode 158: Mode157 + square-aspect square(ish) render path.
  // Use commandline.txt.vr-square (1440×1440) → rename to commandline.txt + restart.
  // Closer BB aspect → fewer bars; keep aspect-fit+overscan FOV. Kill: 157 / 156 / 120.
  OursFpSquareRes = 158,
  // Mode 159: Mode158 stack + stronger overscan (~10%) to eat more black bars.
  // 158 frozen as LKG. Kill: 158 / 157 / 120.
  OursFpStrongOverscan = 159,
  // Mode 160: Square-aspect path + LOW overscan (~2%). Bars mostly gone from
  // square aspect; strong overscan no longer needed. Kill: 158 / 159 / 120.
  OursFpSquareLowOverscan = 160,
  // Mode 161: same as 160 but overscan = 1.0 (0%) for A/B vs 160. Kill: 160 / 158 / 120.
  OursFpSquareZeroOverscan = 161,
  // Mode 162: Mode161 + wider engine FOV (fpfov 100 vs 90). Same square + 0% overscan;
  // packs more world into the filled HMD (true zoom-out). Kill: 161 / 160 / 158.
  OursFpSquareWideFov = 162,
  // Mode 163: Mode162 + flash gate — skip capture only for SMALL RT0 (<70% BB;
  // water/envmap). Full-size offscreen still captures. Kill: 162.
  OursFpFlashGate = 163,
  // Mode 164: Mode163 + in-car eye further back (vehcamoff default 0 -12 0). Kill: 162 / 163.
  OursFpInCarHead = 164,
  // Mode 165: Mode164 + keep first-person during enter-car anim/cutscene. Kill: 164 / 162.
  OursFpEnterCarFp = 165,
  // Mode 166: Mode162 FOV stack + HUD/radar inward offsets (hudoff file). Kill: 162.
  OursFpHudLayout = 166,
  // Mode 167: Mode165 (enter-car FP + in-car head + flash gate) + Mode166 HUD inset
  // (mission directions / radar / status). Kill: 165 / 166 / 162.
  OursFpCarHud = 167,
  // Mode 168: Mode167 car+HUD + flicker-stable dual:
  //   - everyN=1 (no off-tick stale L/R HOLD)
  //   - always capture BB (no tiny-RT skip gate — that gate caused HOLDs/flashes)
  //   - atomic L+R pair update (never mix old L with new R)
  //   - hitch → live BB both eyes (not stale HOLD)
  // FAILED headset 2026-07-28: everyN=1 + FlushRenderingCommands → DEVICE_LOST.
  // Kill: 167 / 162.
  OursFpStableCarHud = 168,
  // Mode 169: SAFE flicker (learned from 168 DEVICE_LOST logs):
  //   - Mode167 car+HUD
  //   - dualn=2 HOLD (NOT everyN=1 — that overloaded the GPU)
  //   - NO FlushRenderingCommands
  //   - always capture BB (no tiny-RT skip gate)
  //   - atomic L+R pair update
  //   - hitch → HOLD last stereo (NOT LIVELOOK mono flash)
  // Headset test 2026-07-28: no freeze, but HMD stutter/flicker (desktop smooth)
  //   — dualn=2 HOLD re-submits stale L/R every other frame. Kill: 167 / 162.
  OursFpSafeFlicker = 169,
  // Mode 170: Mode169 + everyN=1 (fresh dual every DrawScene) — STILL no Flush,
  // hitch→HOLD. Isolates whether 168 DEVICE_LOST was Flush (not everyN=1 alone).
  // Headset: superb FPS, but driving still flickers. Kill: 169 / 167.
  OursFpFreshDual = 170,
  // Mode 171: Mode170 + FREEZE ped/vehicle eye ORIGIN across the L→R dual pair.
  // Driving flicker: ped matrix moves between Left DrawScene and Right DrawScene,
  // so each eye sees a different world position (binocular rivalry). Snapshot the
  // eye-center once before L; both eyes only differ by ±IPD from that freeze.
  // Headset: still flickers on foot — dual DrawScene content differs even frozen.
  // Kill: 170 / 167.
  OursFpPairFreeze = 171,
  // Mode 172: MONO-PAIR flicker A/B + UI lift for phone.
  //   - ONE DrawScene (no IPD) → same BB copied to L and R with SAME eye crop
  //   - Headset: STILL flickers — Left-crop submitted as Right eye → rivalry
  //   - Canvas dest±clamp lift ~10% was too weak / squashy for phone
  // Kill: 171 / 170 / 167. Stereo depth temporarily flat.
  OursFpMonoPair = 172,
  // Mode 173: true mono Submit (same Vulkan tex both eyes) + phone lift = minimap.
  //   - Headset: STILL flickers with sameL=1 → not L/R content; canvas/Submit path suspect
  //   - Phone still cut off at 16% crop
  // Kill: 172 / 170 / 167.
  OursFpSameLPhone = 173,
  // Mode 174: BB-MONO + PHONE — flicker A/B vs canvas path.
  //   - Headset: FLICKER GONE. Confirms eye-canvas/dual Submit was the cause.
  //   - Phone still too low/right in HMD (desktop screenshot: cut off bottom-right)
  // Kill: 173 / 170 / 167.
  OursFpBbPhone = 174,
  // Mode 175: Mode174 (no-flicker BB mono) + phone framing nudge.
  //   TextureBounds: vMin 0.30→0.42 (up), uMin 0→0.14 (right content → left/center).
  // Headset: phone GOOD. Kill: 174 / 170 / 167.
  OursFpBbPhoneNudge = 175,
  // Mode 176: STEREO back — dual DrawScene + simple BB copy. FAILED: flicker back, no 3D feel.
  // Kill: 175 / 170 / 167.
  OursFpStereoSimple = 176,
  // Mode 177: AER — one DrawScene/frame alt L/R. FAILED: flicker, no fusion, head warp.
  // Kill: 175 / 170 / 167.
  OursFpAer = 177,
  // Mode 178: SAME-FRAME FREEZE — Mode170 dual + eye-canvas + freeze FULL pre-IPD
  // cam (basis+pos) across L→R; only ±IPD differs (stronger than Mode171 origin-only).
  // Kill: 170 / 167.
  OursFpSameFrameFreeze = 178,
  // Mode 179: Mode178 + no ColorFill on eye-canvas (keep cover crop for fusion).
  // Kill: 178 / 170.
  OursFpNoColorFill = 179,
  // Mode 180: FAILED — Mode175 Submit UV on eye-canvas = warp.
  OursFpPhoneStereo = 180,
  // Mode 183: FAILED — soft BB crop in CopySurfToEyeCanvas warped look-around
  // and did NOT move the phone. Phone move that worked = Mode175 BB+UV only.
  // Kill: 170.
  OursFpPhoneCanvasInset = 183,
  // Mode 184: FAILED headset — CTimer ms_fTimeStep=0 between L/R armed, flicker
  // unchanged; video-settings crash risk if step left at 0. Poke disabled.
  // Kill: 170.
  OursFpTimeFreeze = 184,
  // Mode 185: CHECKPOINT — Mode170 dual+canvas + Right via PhaseA→PhaseC→DrawScene.
  // Headset: on par with 170; still flickers (driving/menu/LOD). Kill: 170.
  OursFpPhaseRightDual = 185,
  // Mode 186: Mode185 + zoom-out — fpfov 110 (wider engine FOV; cupboard/near
  // objects feel smaller). Not canvas zoom. Kill: 185 / 170.
  OursFpZoomOut = 186,
  // Mode 187: Mode186 + true UEVR-style WorldScale — gtaiv_dxvk_vr.scale multiplies
  // stereo IPD baseline in game units (higher % = smaller world). F11 cycles.
  // Kill: 186 / 185.
  OursFpTrueWorldScale = 187,
  // Mode 188: FAILED — HEAD bone via out-pointer → (0,0,0) under map. Kill: 187.
  OursFpHeadBoneWorldScale = 188,
  // Mode 189: SAME-TICK dual — timer+cam freeze flicker experiment. Kill: 187 / 185.
  OursFpSameTickDual = 189,
  // Mode 190: FAILED — bone natives from CopyMat crash after load. Kill: 187.
  OursFpHeadBoneReturn = 190,
  // Mode 191: MILESTONE LKG — BuildRootA×2 L/R CopyMat (no CodePause; audio OK).
  // Headset 2026-07-28: looks/perf good; whole-screen+UI flicker when looking up/out,
  // gone looking down (post/exposure rivalry hypothesis). Kill: 187 / 185.
  OursFpSameTickBuildDual = 191,
  // Mode 192: Left BuildRootA + Right PhaseTriplet — flicker GONE/smooth but NO FUSION.
  // Kill: 191.
  OursFpSameTickPhaseRight = 192,
  // Mode 193: Mode187 + HEAD via CE Vec3 GetBonePosition thiscall (not matrix;
  // not ScriptHook natives). First build smashed stack with matrix AOB. Kill: 187.
  OursFpHeadBoneThiscall = 193,
  // Mode 194: Left BuildRootA + Right DrawScene+PushLiveCam. Headset: two distinct
  // eye angles (close-one-eye proves parallax), but NO fusion. Not "same mono BB".
  // Kill: 191.
  OursFpSameTickDrawRight = 194,
  // Mode 196: DONE probe — proved OWNER target 0x220D0 via [eax+0x178] at 0x30D0D.
  // Headset: 1 FPS (VirtualQuery on every VsRet) + joystick look crashes. Observe path
  // is now disabled (WantsTrace off). Keep enum for log archaeology. Kill: 191.
  OursFpSameTickOwnerObserve = 196,
  // Mode 197: FAILED playtest — COUNT naked hook on 0x220D0 (Mode196 target).
  // Headset: worse FPS than 191 + heavy flicker. 0x220D0 is per-draw hot (10k+/frame),
  // not a ~1×/frame walker — COUNT trampoline alone is too expensive. Hook install
  // refused; falls through as Mode191. Kill: 191.
  OursFpSameTickOwnerCount = 197,
  // Mode 198: Mode191 dual + LIGHT parent probe — at most ONE VsRet stack sample per
  // EndScene (no VirtualQuery). Hunt ~1×/frame callers ABOVE hot leaf 0x220D0.
  // Kill: 191.
  OursFpSameTickParentProbe = 198,
  // Mode 199: Mode191 dual + COUNT-ONLY naked hook on fn-start 0x4D8BF0
  // (Mode198 RARE ret 0x4D8C36 → thiscall-56). Soft: avg entries/ES > 8 after 90 ES
  // → disable + write stereo=191. Kill: 191.
  // Headset 2026-07-28: PASS — avg~0.99/ES, smooth, no crashes.
  OursFpSameTickParentCount = 199,
  // Mode 200: TRUE parent dual — SINGLE BuildRootA + HookDrawWalk×2 @0x4D8BF0
  // with Mode191 RefreshLiveCam L/R CopyMat (one world build; per-eye view at
  // Mode198/199 RARE parent). Soft avg>8/ES or SEH → 191. Kill: 191.
  // Headset 2026-07-28: PASS smooth/no flicker; fusion incomplete + look ghosting
  // (F8 IPD helps but not fully). Next: 201 cam freeze.
  OursFpSameTickParentDual = 200,
  // Mode 201: Mode200 + full pre-IPD cam freeze for the L/R parent dual pair
  // (same basis; only ±IPD differs — kills look-around pose drift ghosting).
  // No CodePause/timer. Kill: 200 / 191.
  // Headset 2026-07-28: REJECT — no fusion gain; headlook drag. Keep enum only.
  OursFpSameTickParentDualFreeze = 201,
  // Mode 202: FAILED look-blur — VS-translate fuses when still but is NOT true
  // per-eye view (translation-only on baked matrices). Kill: 200 / 191.
  OursFpSameTickParentDualVs = 202,
  // Mode 203: MILESTONE — TRUE 3D VR checkpoint (2026-07-28 headset).
  // Same-pose parent dual @0x4D8BF0 + HMD pose latch + full CCam±IPD.
  // Smooth ~90 FPS; size feel OK; near may still double. Snapshot: prebuilt/mode203.
  // Kill: 200 / 191. Later parent-try modes kill back to 203.
  OursFpSameTickParentDualPose = 203,
  // Mode 204: Mode203 path on NEXT Mode198 RARE thiscall parent fn-start 0x4DE020
  // (ret 0x4DE0DC). Same pose latch + CCam±IPD. Kill: 203 MILESTONE.
  // Headset: fusion LKG; near bounce + phone washout noted — leave frozen.
  OursFpSameTickParentDualTry2 = 204,
  // Mode 205: REJECT CRASH — Mode198 ret 0x4DDE1F → fn 0x4DDD50 looks thiscall-56
  // but is a float/math helper (not a draw shell). Dual after load = hard crash.
  // Log: dual #1 then die; 0 entries during load. Leave enum; kill=204. Do not retry.
  OursFpSameTickParentDualTry3 = 205,
  // Mode 206: MIXED — no near-bounce, no phone washout; but HOT (~35–47 entries/ES
  // after warm; soft gate only checked es#90). Car enter froze; FPS worse. Kill: 204.
  // Leave enum; do not use as daily. 203/204 frozen.
  OursFpSameTickParentDualTry4 = 206,
  // Mode 207: REJECT — tiny wrapper @0x541DC0 duals a lea/call/ret stub; same BB both
  // eyes (no 3D / no fuse). Leave enum; kill=204. Do not retry.
  OursFpSameTickParentDualTry5 = 207,
  // Mode 208: REJECT — mid-jmp @0x4DA2BA neither works. Leave enum; kill=204.
  OursFpSameTickParentDualTry6 = 208,
  // Mode 209: REJECT — OUTER @0x4D8E80 same-image both eyes (no fusion). Bake-widen
  // via outer caller failed; near fusion on 203 is not fixed by dualing the caller.
  // Leave enum; kill=203. Do not retry.
  OursFpSameTickParentDual203Outer = 209,
  // Mode 210: MIXED — 203 + toe-in ~1.5m; headset: maybe slightly better near fuse,
  // still not fused; cam drag not worse. Leave enum. Kill: 203.
  OursFpSameTickParentDual203ToeIn = 210,
  // Mode 211: same as 210 but stronger toe-in (converge ~0.85m — dash/hands).
  // Headset: still not fully fused. Kill: 203.
  OursFpSameTickParentDual203ToeInStrong = 211,
  // Mode 212: 203 path + closer eyes IPD 1cm. Headset: getting there, wants bit more.
  // Kill: 203.
  OursFpSameTickParentDual203CloseIpd = 212,
  // Mode 213: same as 212 but IPD 0.5cm (closer still). Kill: 212 / 203.
  OursFpSameTickParentDual203CloseIpdHalf = 213,
  // Mode 214: same as 213 but IPD 0.25cm (closer still). Kill: 213 / 203.
  OursFpSameTickParentDual203CloseIpdQtr = 214,
  // Mode 215: 214 IPD 0.25cm + mild toe-in ~1.5m (combine levers). Kill: 214.
  OursFpSameTickParentDual203CloseIpdToeIn = 215,
  // Mode 216: Mode204 stereo (@0x4DE020) + deferred HEAD bone sample (EndScene only;
  // cam bake reads cache — never GetBonePos mid-DrawScene). Kill: 204.
  OursFpSameTickParentDual204DeferredHead = 216,

  // ---- TrueStereo (docs/STEREO_TRUETICK_REVIEW.md) — 203-seam retry ------------
  // Mode 230: PROBE. Mode203 dual + read-only projection measure. Kill: 203.
  TrueStereoCalibProbe = 230,
  // Mode 231: EXACT. Publish measured gameTan; sep=HMD IPD; stereoscale 1.00;
  // no toe-in; worldscale not × baseline. Bars OK. Kill: 203.
  TrueStereoExact = 231,
  // Mode 232: COVER (§4.3). Exact + raise fpfov until measTan >= coverTan.
  // Kill: 231.
  TrueStereoCover = 232,
  // Mode 233: DIRECT (§4.4). Cover + 1:1 BB→eye, float VRTextureBounds from
  // measured tangents; under-cover falls back to canvas. Kill: 232.
  TrueStereoDirect = 233,
  // Mode 234: SAME-STATE (§4.5). Direct + CTimer step=0 across pair (exposure dt
  // proxy) + latch controller/vehicle yaw + ped eye-center. Kill: 233.
  // REJECTED headset (203+204): jump / no walk / no audio — do not re-enable freeze.
  TrueStereoSameState = 234,
  // Mode 235: STABLE PUBLISH — Direct + reject wild measTan (keep last good gameTan).
  // Unblocks aspect/FOV flap that broke Direct fuse. Kill: 233.
  TrueStereoStablePublish = 235,
  // Mode 236: LEFT PUBLISH — 235 gate + publish measured gameTan from Left eye only
  // (Right logged but not published — stops L↔R ~3% flip-flop of Direct bounds). Kill: 235.
  TrueStereoLeftPublish = 236,
  // Mode 237: LATERAL IPD — 236 + zero per-eye EyeZ forward shift. Eyes share one pivot
  // (left/right only). Fixes joystick sweet-spot / jacket near double. Kill: 236.
  // Headset: still sweet-spot — IPD was along HMD-right while stick yaw rotates view.
  TrueStereoLateralIpd = 237,
  // Mode 238: UEVR IPD — 236 + EyeToHead translation in CAMERA local frame (after stick
  // yaw), like praydog calculate_stereo_view_offset. No L4D2 HMD-right / EyeZ hacks. Kill: 237.
  // REJECTED headset: way worse — do not retry full EyeToHead on this path.
  TrueStereoUevrIpd = 238,
  // Mode 239: CAM-RIGHT IPD — 236 + ± ±half sep along post-yaw CAMERA right only.
  // (Not HMD-right=237 stick mismatch; not full EyeToHead=238.) Parallel cams, one pivot. Kill: 237.
  TrueStereoCamRightIpd = 239,
  // Mode 240: CAM-RIGHT IPD FLIP — 239 with swapped ±half (239 pivot OK, fuse inverted?). Kill: 239.
  // Headset: fuse improved; aspect/FOV still spazzes (TrueStereo measured publish).
  TrueStereoCamRightIpdFlip = 240,

  // Mode 241: stock Mode203 + cam-right IPD flip only (no TrueStereo measure/Direct/publish).
  // Isolates the L/R fuse win without gameTan flap. Kill: 203.
  // CHECKPOINT 2026-07-29: 100% 3D stereo VR (see prebuilt/mode241).
  OursFpSameTickParentDual203CamRightFlip = 241,
  // Mode 242: Mode204 seam (@0x4DE020) + same cam-right IPD flip as 241. Kill: 204 / 241.
  OursFpSameTickParentDual204CamRightFlip = 242,
  // Mode 243: Mode241 stereo (203 + cam-right flip) + late-latch Submit_TextureWithPose.
  // One shared HMD pose stamped on L+R at Submit (Mode136 lesson) — head feel without
  // losing 241 fusion. Kill: 241 / 203.
  OursFpSameTickParentDual203CamRightFlipLateLatch = 243,
};

// Mode 40–49: rapid HMD delta → temporary mono pair. Mode 50+ never flattens.
bool UsesMotionGuardStereo(StereoMode mode);
// Mode 50–53: stereo-first temporal path with pair audit logging.
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
bool IsExternalFpHost(StereoMode mode);            // 140–148, 150 (not 151)
bool UsesDrawSceneDualPath(StereoMode mode);       // clean dual OR FP-host OR 151
bool IsExternalFpHostLookFix(StereoMode mode);     // 141
bool IsExternalFpHostLookAlt(StereoMode mode);     // 142
bool IsExternalFpHostLookPitch(StereoMode mode);   // 143
bool IsExternalFpHostLookPitchAlt(StereoMode mode); // 144
bool IsExternalFpHostLookMove(StereoMode mode);    // 145
bool IsExternalFpHostLookMoveYaw(StereoMode mode); // 146
bool IsExternalFpHostSoftMove(StereoMode mode);    // 147
bool IsExternalFpHostSoftMoveStick(StereoMode mode); // 148
bool IsHeadHideNativeWithFp(StereoMode mode);      // 150
bool IsHeadHideNativeOurs(StereoMode mode);        // 151–189
bool IsOursFpFovProfile(StereoMode mode);          // 152 only
bool IsOursFpWideUnderPublish(StereoMode mode);    // 153 only (cover-lock)
bool IsOursFpWideAspectFit(StereoMode mode);       // 154–189 (aspect-fit under-publish)
bool IsOursFpEyeCenterCam(StereoMode mode);        // 155–189
bool IsOursFpEyeCenterLow(StereoMode mode);        // 156–189 (lower pivot + F5 VR res)
bool IsOursFpMildOverscan(StereoMode mode);        // 157–189 (family; 161+ uses 1.0)
bool IsOursFpSquareRes(StereoMode mode);           // 158–189
bool IsOursFpStrongOverscan(StereoMode mode);      // 159 only
bool IsOursFpSquareLowOverscan(StereoMode mode);   // 160 only
bool IsOursFpSquareZeroOverscan(StereoMode mode);  // 161–189 (0% overscan family)
bool IsOursFpSquareWideFov(StereoMode mode);       // 162–189 (fpfov 100 family)
bool IsOursFpFlashGate(StereoMode mode);           // 163–167 (tiny-RT skip; not 168+)
bool IsOursFpFlashStable(StereoMode mode);         // 168 only (FAILED DEVICE_LOST)
bool IsOursFpSafeFlicker(StereoMode mode);         // 169 only (dualn=2; HMD stutter)
bool IsOursFpFreshDual(StereoMode mode);           // 170–173, 176, 178–180, 183–192 (everyN=1)
bool IsOursFpPairFreeze(StereoMode mode);          // 171 only (origin freeze)
bool IsOursFpMonoPair(StereoMode mode);            // 172–173 (1 DrawScene → both eyes)
bool IsOursFpSameLPhone(StereoMode mode);          // 173 only (sameL Submit + radar phone lift)
bool IsOursFpBbPhone(StereoMode mode);             // 174–175 (Mode0 BB submit; no canvas)
bool IsOursFpBbPhoneNudge(StereoMode mode);        // 175 only (phone up+left bounds)
bool IsOursFpStereoSimple(StereoMode mode);        // 176 only (FAILED dual flicker)
bool IsOursFpAer(StereoMode mode);                 // 177 only (FAILED AER)
bool IsOursFpSameFrameFreeze(StereoMode mode);     // 178–180 (full pre-IPD cam freeze)
bool IsOursFpNoColorFill(StereoMode mode);         // 179–180 (skip canvas ColorFill)
bool IsOursFpPhoneBounds(StereoMode mode);         // 175–177, 180 Submit UV (NOT 183)
bool IsOursFpPhoneCanvasInset(StereoMode mode);    // 183 FAILED crop
bool IsOursFpTimeFreeze(StereoMode mode);          // 184 FAILED CTimer poke (disabled)
bool IsOursFpPhaseRightDual(StereoMode mode);      // 185–192 PhaseTriplet Right
bool IsOursFpZoomOut(StereoMode mode);             // 186–192 fpfov 110
bool IsOursFpTrueWorldScale(StereoMode mode);      // 187–193 scale×IPD (UEVR-style size)
bool IsOursFpHeadBoneCam(StereoMode mode);         // 193 CE thiscall HEAD bone eye
bool IsOursFpSameTickDual(StereoMode mode);        // 189 timer+cam freeze (audio hitch)
bool IsOursFpSameTickBuildDual(StereoMode mode);   // 191–199 same-tick family
bool IsOursFpSameTickPhaseRight(StereoMode mode);  // 192 smooth, no fusion
bool IsOursFpSameTickDrawRight(StereoMode mode);   // 194 distinct L/R, no fusion
bool IsOursFpSameTickOwnerObserve(StereoMode mode); // 196 DONE — observe disabled
bool IsOursFpSameTickOwnerCount(StereoMode mode);  // 197 FAILED — hook refused
bool IsOursFpSameTickParentProbe(StereoMode mode); // 198 light once/ES parent hunt
bool IsOursFpSameTickParentCount(StereoMode mode); // 199 COUNT @ 0x4D8BF0
bool IsOursFpSameTickParentDual(StereoMode mode);  // 200–216 parent dual family
bool IsOursFpSameTickParentDualFreeze(StereoMode mode); // 201 REJECT freeze
bool IsOursFpSameTickParentDualVs(StereoMode mode);     // 202 REJECT VS-translate
bool IsOursFpSameTickParentDualPose(StereoMode mode);   // 203–216 HMD pose latch + full L/R
bool IsOursFpSameTickParentDualTry2(StereoMode mode);   // 204 + 216 @0x4DE020 (204 frozen)
bool IsOursFpSameTickParentDualTry3(StereoMode mode);   // 205 REJECT @0x4DDD50
bool IsOursFpSameTickParentDualTry4(StereoMode mode);   // 206 MIXED HOT @0x309D0
bool IsOursFpSameTickParentDualTry5(StereoMode mode);   // 207 REJECT tiny @0x541DC0
bool IsOursFpSameTickParentDualTry6(StereoMode mode);   // 208 REJECT mid-jmp @0x4DA2BA
bool IsOursFpSameTickParentDual203Outer(StereoMode mode); // 209 REJECT outer @0x4D8E80
bool IsOursFpSameTickParentDual203ToeIn(StereoMode mode); // 210–211, 215 toe-in near fusion
bool IsOursFpSameTickParentDual203ToeInStrong(StereoMode mode); // 211 stronger ~0.85m
bool IsOursFpSameTickParentDual203CloseIpd(StereoMode mode); // 212–215 closer IPD
bool IsOursFpSameTickParentDual203CloseIpdHalf(StereoMode mode); // 213–215 IPD ≤0.5cm
bool IsOursFpSameTickParentDual203CloseIpdQtr(StereoMode mode);  // 214–215 IPD 0.25cm
bool IsOursFpSameTickParentDual203CloseIpdToeIn(StereoMode mode); // 215 IPD0.25+toe-in
bool IsOursFpDeferredHeadBone(StereoMode mode);     // 216 deferred HEAD sample (not 193)
// TrueStereo — docs/STEREO_TRUETICK_REVIEW.md; 230–239 on 203 seam.
bool IsTrueStereoFamily(StereoMode mode);           // 230–239 (203 seam)
bool IsStereoCalibProbe(StereoMode mode);           // 230 measure-only, no behavior change
bool IsTrueStereoExact(StereoMode mode);            // 231+ measured tangents, IPD = HMD
bool IsTrueStereoCover(StereoMode mode);            // 232+ raise fpfov until meas>=cover
bool IsTrueStereoDirect(StereoMode mode);           // 233+ 1:1 BB + float Submit bounds
bool IsTrueStereoSameState(StereoMode mode);        // 234 exposure + yaw/eye pair latch
bool IsTrueStereoStablePublish(StereoMode mode);    // 235+ reject wild measTan publish
bool IsTrueStereoLeftPublish(StereoMode mode);      // 236+ publish Left eye only
bool IsTrueStereoLateralIpd(StereoMode mode);       // 237 zero per-eye EyeZ forward
bool IsTrueStereoUevrIpd(StereoMode mode);          // 238 camera-local EyeToHead (REJECTED)
bool IsTrueStereoCamRightIpd(StereoMode mode);      // 239–240 ±half on post-yaw camera right
bool IsTrueStereoCamRightIpdFlip(StereoMode mode);  // 240 swapped L/R sign
bool IsOursFp203CamRightFlip(StereoMode mode);      // 241–243 Mode203 + cam-right flip (no TrueStereo)
bool IsOursFp204CamRightFlip(StereoMode mode);      // 242 Mode204 + cam-right flip
bool IsOursFp203LateLatch(StereoMode mode);         // 243 = 241 + TextureWithPose late (shared L/R)
bool UsesCamRightIpd(StereoMode mode);              // 239–243
bool UsesCamRightIpdFlip(StereoMode mode);          // 240–243
bool UsesLateLatchSubmit(StereoMode mode);          // 136 or 243
bool IsOursFpUiLift(StereoMode mode);              // 172–173 canvas phone lift (not 174+)
bool IsOursFpAtomicEyePair(StereoMode mode);       // 168–173, 176, 178–180, 183–192
bool IsOursFpAlwaysBbCapture(StereoMode mode);     // 168–173, 176, 178–180, 183–192
bool IsOursFpInCarHead(StereoMode mode);           // 164–165, 167–180, 183–192
bool IsOursFpEnterCarFp(StereoMode mode);          // 165, 167–180, 183–192
bool IsOursFpHudLayout(StereoMode mode);           // 166–180, 183–192
bool IsOursFpCarHud(StereoMode mode);              // 167–180, 183–192
bool IsOursFpStableCarHud(StereoMode mode);        // 168 only
bool IsOursFpDualCamFreeze(StereoMode mode);       // 171, 178–180, 189, 193, or 201
// Force HMD→ped heading (walk look-dir): 145–148, 150–172, clean dual.
bool WantsExternalFpLookMove(StereoMode mode);
bool WantsSoftPedHeading(StereoMode mode);
bool WantsFpStickInvert(StereoMode mode);
bool WantsNativePedHide(StereoMode mode);
// Absolute engine FOV like FirstPerson SET_CAM_FOV (ForwardFOV from fpfov file).
// Modes 153–166: write CCam; canvas under-publish (cover or aspect-fit).
bool WantsFpAbsoluteFov(StereoMode mode);
// Under-publish overscan: 1.0 = 0% (Mode 161–166), ~1.02 low, ~1.06 mild, ~1.10 strong.
float GetUnderPublishOverscan();
// Mode 164/165: Inspiration-style vehicle camoff (cm R/F/U). Default 0 -12 0.
void GetVehicleCamOffsetMeters(float* outRight, float* outForward, float* outUp);
void EnsureVehicleCamOffDefaults();

// Eye-canvas maxDim (F5 cycles). Comfort default 1536. Gen bumps to unlock RT size.
uint32_t GetCanvasMaxDim();
uint32_t GetCanvasMaxDimGeneration();
void SetCanvasMaxDim(uint32_t dim, bool fromHotkey);
void ReloadCanvasMaxDim();
void PollVrResHotkey();

// FirstPerson.ini-style knobs (files next to ASI; defaults match user 90/90/90 test).
float GetFpForwardFovDegrees();  // gtaiv_dxvk_vr.fpfov first value / forwardfov
float GetFpRearFovDegrees();
float GetFpFootFovDegrees();
float GetFpMouseSensScale();     // MouseSens/50 → 1.0 default
float GetFpJoySensScale();       // JoySensLev1/20 → 1.0 default
void EnsureFpProfileDefaults();  // write missing fpfov/camoff/sens files
// Write gtaiv_dxvk_vr.fpfov + reload cache (Mode 162 / TrueStereo Cover raise).
void ForceFpFovDegrees(int forward, int rear, int foot);

// Mode 123: once cover FOV known, set fovadd so fillH≈100% (no SoftInset).
void TryApplyCoverMatchedFovAdd();

StereoMode GetStereoMode();
void ReloadStereoMode();
// Persist kill/fallback mode to gtaiv_dxvk_vr.stereo (next launch safe default).
void WriteStereoModeFile(int mode);

float GetStereoSepMeters();
void PollIpdScaleHotkey();
float GetStereoIpdScale();

// 6DoF lean (all modes). Mode187+: also multiplies stereo IPD baseline (UEVR size).
float GetWorldScale();
// F7: cycle visual WorldScale presets (fovadd + stereoscale). Persist index to
// gtaiv_dxvk_vr.worldscale. Default "Open12" — DEZOOM via LOWER fovadd (less
// StretchRect crop) + higher stereoscale. Higher fovadd = more crop zoom-IN.
void PollWorldScaleHotkey();
// F11: cycle gtaiv_dxvk_vr.scale 100→125→150→175→200 (true WorldScale % on Mode187).
void PollTrueWorldScaleHotkey();

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
// Mode 152 FP presence uses 0 (see jacket when looking down, like FirstPerson).
float GetEyeForwardMeters();
void SetEyeForwardCm(int cm);

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
