# Plan — where we are and what to do next (2026-07-26)

Read with `docs/CURRENT-STATE.md`, **`docs/RE_OFFSETS.md`**, **`docs/MAPPED_RVA_CHEATSHEET.md`**, and `docs/HANDOFF_GROK.md`.

**Mapped RVA rule:** CE `.text` **file_offset + 0xC00**. Older session notes below still say `0x30300` / `0x3187C` etc. as historical — live targets are **`0x30F00` / `0x32470`**.

## Session note 2026-07-26 — Mode **88** DEFAULT (nap marathon)

**Shipped:** EyeProj ON + DrawScene dual every-N HOLD + eyert 1440 + Reset-safe RTs + buffered/quieter logs + DEVICELOST Submit skip + QPC dualMs + WaitGetPoses stall probe.  
**Build:** `20260726-191037-mode88-final90` · stereo **`88`** · eyert **`1440`** · dualn **`2`** · kill **`45`**.

**Next (one thing per session):**
1. Headset street walk on **88** — hitch vs Mode87? EyeProj feel? Bars OK?
2. Graphics options Reset — confirm no freeze (`DeviceReset:` in log) — agent armed; needs headset confirm.
3. Optional A/B: stereo **`89`** (soft68 bars) if bars annoy more than hitch.
4. Optional: dualn=`3` if streets still hitchy.
5. Cull sync / sidewalk holes if they return with EyeProj ON.
6. Street: watch `WaitGetPoses: ms≥20` vs dualMs under load (apartment dual ~0.1ms; pose avg ~11ms).

**Never:** Mode86 default; Mode83 fill-zoom; live `view+0x80`; SteamVR RT takeover as default.

---

## Session note 2026-07-25 — Mode74 AER presence (mainline)

**User:** smoother not priority — need **proper VR / presence inside the world**. Port RealVR RE ideas.

**Shipped AER strategy:**
- One engine eye/frame (L/R) + Submit both `TextureWithPose` (held = last tex + capture pose)
- Engine FOV = HMD cover H+V; DRAW worldlook + EyeToHead/IPD + seated 6DoF
- Shared FrameDesc; dual×2 stays file `0` (not default)
- stereo **`74`** · dual **`0`** · kill **`45`** · tag **`mode74-aer`**

**Ask:** turn head → world stable? lean / near objects → inside VR or still cinema?

**Honest gaps vs Luke GTA5:** no 3DMigoto Unmap FOV rewrite; no CUDA AER v2; no ScriptHookV cam natives; no same-tick L+R; GTA IV Rage seams ≠ GTA5.

---

## Session note 2026-07-25 ~17:23 — Mode74 hitchcut

Superseded by **AER presence**. Build was `20260725-172331-mode74-hitchcut`.

---

## Session note 2026-07-25 ~17:15 — Mode74 temporalfirst

Superseded by **hitchcut**. Build was `20260725-171527-mode74-temporalfirst`.

---

## Session note 2026-07-25 ~17:09 — Mode74 sessionlatch

HARD OFF → SESSION OFF. Superseded by **temporalfirst**.  
Build was `20260725-170927-mode74-sessionlatch`.

---

## Session note 2026-07-25 ~17:01 — Mode74 fpsfix

HOLD-only-while-OPEN + temporal on HARD OFF. User thrash → superseded by **sessionlatch**.  
Build was `20260725-170139-mode74-fpsfix`.

---

## Session note 2026-07-25 ~16:55 — Mode74 sparsedual

Sparse 1/8 + HOLD + HARD OFF. User: better stereo, headset FPS bad → superseded by **fpsfix**.  
Build was `20260725-165539-mode74-sparsedual`.
---

## Session note 2026-07-25 ~16:41 — Mode74 hmdfov

**User:** still giant TV after worldlook. FOV→HMD helped a bit → still fake 3D.  
buildid was **`20260725-164113-mode74-hmdfov`**. Superseded by **sparsedual**.

---

## Session note 2026-07-25 ~16:30 — Mode74 worldlook

HMD look stamp; dual OFF. Superseded by hmdfov → sparsedual.

---

## Session note 2026-07-25 ~15:59 — Mode74 toein (RE AFK)

**Checkpoint:** `d5bb0ec` / `checkpoint-mode74-contentinj-20260725`

**RE:** UploadFn `0x2A1E10` silent in-world; hot path = ReplayDispatch **`ret=0x30D13`** → SetVSConstF.  
**Shipped:** Mode72 ±IPD **+ toe-in @2m**; UploadFn hook kept; CamMatrix IPD on; StereoDiff; dual 1/8 opt-in still OFF.  
buildid: **`20260725-155908-mode74-toein`** · stereo **`74`** · kill **`45`**.

**Superseded by worldlook** (head-lock root).

---

## Session note 2026-07-25 ~15:47 — Mode74 contentinj (presence, dual OFF)

**Goal:** true-er 3D without every-frame BuildRootA×2.

**Root (crashsafe):** 1 inject/ES + CamMatrix `ipd=0` while Mode74 “owned” IPD via VS — weak parallax.

**Shipped:** DualViewBudget=12 multi-inject; forced ±half on src-copy when dualEn=0; near-cam contentHit armed; CamMatrix IPD on when dualEn=0; never view+0x80 / dual×2 off.  
buildid: **`20260725-154721-mode74-contentinj`** · stereo **`74`** · kill **`45`**.  
Agent: `eyeSep=5.0cm` injects↑; CamMatrix `ipd≠0`; `dualEn=0`; `contentInj=0` (ptr hits).

**Headset:** lean near car/wall. Still flat → hook upload fn **`0x2A1E10`** prologue next (prefer over sparse dual).

---

## Session note 2026-07-25 ~15:18 — Launch DirectExe + Mode74 dualsep

**Part A (HIGH):** Steam `-applaunch` → LAUNCHING Cancel+Play. **Fixed:** `restart-gtaiv.ps1` defaults to **GTAIV.exe DirectExe** (cwd=game dir). Verified GTAIV in ~1s. One-click: Desktop **GTA IV Quick Restart** / `restart-gtaiv.bat`. Fallback `-SteamLaunch`.

**Part B:** Still-monitor after CamIPD. Root: dual VIEW inject **eyeSep=0** (skipped sep); inject budget never reset while gate OPEN (injects stuck ~90); build-thread SetVSConstF → 45.

**Shipped:** dual src-copy ±half when inject fires; CamMatrix→D3DTS_VIEW per eye; EndScene budget reset + VS c0 from CamMatrix; never view+0x80.  
buildid: **`20260725-1518-mode74-dualsep`** · stereo **`74`** · kill **`45`**.  
Agent: injects climb with ES; L/R VIEW push differ; 0 EXCEPTION; launch OK.

**Headset:** lean near car/wall. Still monitor → capture after replay upload / `0x2A1E10` draw-thread seam. Not more IPD alone.

---

## Session note 2026-07-25 ~14:25 — Mode74 CamIPD dual (presence)

**User:** PubProj smooth but **"still flat"** — need IN the game.

**Audit:** PubProj HOOK L/R ox OK; StereoDiff≠0; **Mode72 VIEW inject silent in dual** (`contentInj=0`); CamMatrix had **ipd=0 during dual** because Mode74 skipped it for VS-owned sep.

**Shipped:** CamMatrix applies IPD while `StereoInDualPass()`; outside dual Mode74 VS still owns e2h. PubProj HOLD +0x180 for eye walk kept.  
buildid: **`20260725-1425-mode74-camipd`** · stereo **`74`** · kill **`45`**.  
Superseded by dualsep above for inject/budget/launch.

---

## Session note 2026-07-25 ~13:57 — Mode74 PubProj (superseded — smooth/flat)

**User:** EyeProj still flat — need to be **IN** the game (not monitor-in-face).

**Shipped then:** MinHook **PublishProj `0x31BA0`** → `+0x308`. Smooth PASS; presence FAIL → CamIPD above.  
buildid was: **`20260725-1404-mode74-pubproj`**.

---

## Session note 2026-07-25 ~13:52 — Mode74 EyeProj (superseded — still flat)

`GetProjectionRaw` → `+0x180` + c4 push. Armed in log; headset still flat.  
buildid was: **`20260725-1352-mode74-eyeproj`**.

---

## Session note 2026-07-25 ~07:08 — Mode74 stable (flicker/perf/jump)

**Headset on hmd6dof:** half FPS, jumpy, ~1Hz flicker (also in hmdlook fallback). Sticky gate OK (0 CLOSE) — not that flicker.

**Root:** double HMD path (CamMatrix 6DoF+IPD + Mode74 seated-6DoF+live e2h) + mid-dual pose resample.

**Shipped:** frozen pose/dual; EyeToHead cache-only; Mode74 no seated-6DoF; Mode74 owns IPD (CamMatrix skips while 74); light pose EMA. Sticky every-frame dual kept.  
buildid: **`20260725-0708-mode74-stable`** · stereo **`74`** · kill **`45`**.

**Superseded for 3D feel by EyeProj above.** Do **not** reopen dual seam.

---

## Session note 2026-07-25 ~06:54 — Mode74 HMD×EyeToHead (warp) — REGRESSED

**User:** best-so-far saved as fallback; world still **warps on head move**.

**Fallback:** commit **`1476229`** / tag **`fallback-mode74-hmdlook-20260725`** (`20260725-0648-mode74-hmdlook`).

**Hypothesis:** HMD rotation on src-copy without **EyeToHead** (+ seated) → shear; asymmetric proj still missing (Mode15 dead).

**Shipped then:** Mode74 src-copy = HMD pose × `GetEyeToHeadTransform` + seated 6DoF — **headset: half FPS / jumpy / flicker** → superseded by **stable** above.  
buildid was: **`20260725-0654-mode74-hmd6dof`**.

---

## Session note 2026-07-25 ~06:48 — Mode74 fused↔separated flicker FIX + HMD look

**User:** every-frame Mode74 no freeze, but fused↔separated alternating.

**Root cause (log):** gate miss-close → Mode72 temporal. ~**55 CLOSE / 56 OPEN** on `20260725-0635-mode74-every` (`streaming gate CLOSED after 60 miss… back to Mode72 temporal`).

**Part A shipped:** sticky gate OPEN (no miss re-close) + no `TemporalCapturePairHold` while OPEN.  
buildid: **`20260725-0644-mode74-noflicker`** · stereo **`74`** · kill **`45`**.  
Agent: 0 further CLOSE, `stickyOpen=1`, `liveStreak` 1000+, `StereoDiff≠0`.

**Part B shipped:** Mode74 HMD orient on SetVSConstF **src-copy** (never view+0x80) + `HmdLook: ACTIVE/ENABLED` logs.  
buildid: **`20260725-0648-mode74-hmdlook`** · stereo **`74`** · kill **`45`** — **git fallback** above.

**User test:** load world → stable stereo (no jump) → turn head (view follows) → F9 recenter. Kill file **`45`**.

**Next:** warp fix → see ~06:54 note. Do **not** reopen dual seam unless freeze returns.

---

## Session note 2026-07-25 ~06:38 — Mode 74 every-frame **MAJOR PASS** (flicker found later)

**Headset:** user **"no freeze"** + log proves every-frame dual: buildid `20260725-0635-mode74-every`, `StereoMode: 74`, `every=1/1`, gate OPEN, `SAME-FRAME dual` past #5 to **dualN≈9k+**, `StereoDiff absdiff≠0`, `StereoSubmit mode=74`, **0** EXCEPTION.

**Stereo ladder done:** 1/8 → 1/4 → 1/2 → **1/1 every-frame** stable with gate + inject-only-in-dual.

**Follow-up:** fused↔separated flicker + HMD look → see ~06:48 note above.

---

## Session note 2026-07-25 ~06:35 — Mode 74 every-frame 1/1 (shipped → PASS above)

**Shipped:** `kMode74DualEveryN` **2→1** only (true same-frame every BuildRootA after gate). Fixed EveryN=1 modulo (`tick%1` never `==1`). Gate / inject-only-`g_inDual` / exception→45 / miss re-close kept. No IPD/camera/projection.
buildid: **`20260725-0635-mode74-every`** · stereo **`74`** · kill **`45`** · soft **`72`**.

**Headset:** **"no freeze"** → MAJOR PASS; next = Camera/HMD look (above).

---

## Session note 2026-07-25 ~06:29 — Mode 74 sparse 1/2 PASS → go-ahead

**User:** **"no freeze, go ahead"** → every-frame shipped above.
buildid then: **`20260725-0629-mode74-1of2`** · stereo **`74`** · kill **`45`** · soft **`72`**.

---

## Session note 2026-07-25 ~06:27 — Mode 74 sparse 1/4 PASS

**Headset:** user **playing fine, no freezes**. Log proves duals this session: `gate=OPEN`, `sparse=1/4`, `SAME-FRAME dual` past #5 hang to **#4800+**, `StereoDiff absdiff≠0` (e.g. 15578 / 8391), `StereoSubmit mode=74`, **0** EXCEPTION.
buildid: **`20260725-0621-mode74-1of4`** · stereo **`74`** · kill **`45`** · soft **`72`**.

**False alarm:** “Steam LAUNCHING / game not seen” was wrong — user was already in-world.

**Honesty:** still ~**1/4** same-frame + **3/4** Mode72 temporal — superseded by **1/2** ship above.

---

## Session note 2026-07-25 ~06:21 — Mode 74 denser sparse 1/4 (shipped → PASS above)

**Shipped:** `kMode74DualEveryN` **8→4** only. Gate / inject-only-`g_inDual` / exception→45 / miss re-close kept.
buildid: **`20260725-0621-mode74-1of4`** · stereo **`74`** · kill **`45`** · soft **`72`**.

---

## Session note 2026-07-25 ~06:18 — Mode 74 sparse 1/8 PASS

**Headset:** user **"no freeze"** + log proves dual ran: gate CLOSED→OPEN, `SAME-FRAME dual` #1→#480+,
`dualN≈479`, `haveL/R=1`, `StereoDiff absdiff≈10k`, `StereoSubmit mode=74`, no EXCEPTION.
Sparse 1/8 = occasional true same-frame mixed with Mode72 — superseded by **1/4** above.

buildid: **`20260725-0615-mode74-sparse`** · kill **`45`** · soft **`72`**.

---

## Session note 2026-07-25 ~06:15 — Mode 74 sparse after freeze (shipped; PASS above)

**Evidence then:** continuous Mode74 gate OPEN → dual #1–#5 → hard hang.  
**Shipped:** gated + dual **1/8** + inject only while `g_inDual`. Remap **73→72**, **71→45**.

---

## Session note 2026-07-25 ~06:10 — Mode 74 gated same-frame (froze)

Continuous dual after gate hung at #5 — superseded by sparse.
buildid was **`20260725-0609-mode74-gated`**.

---

**Headset ladder (done):** **66 PASS** → **67 REJECT** → **65 OK** (`slot178=0x220D0`) → **42 dry** → **64 REJECT**.

**Prior next:** stereo **`69`** ReplayDispatch MAT-PEEK — superseded by 70–74 path.

---

## Session note 2026-07-25 ~05:25 — Mode 69 matrix peek (after 68 PASS)

**Shipped:**
- Mode **69** `ReplayDispatchMatPeek` — same hook site as 68; SEH-safe read of `float[16]` at view+0x80
- Logs translation `t=(m12,m13,m14)` + max frame-to-frame delta²; dual=OFF; auto→45
- Parse caps raised to **69**

**Test:** stereo **`69`** → `Mode69: PEEK armed` → `cadence=PASS` if readable.

---

## Session note 2026-07-25 ~03:57 — Mode 68 ReplayDispatch COUNT (offline only)

**Shipped to out-asi only:**
- Mode **68** `ReplayDispatchCountProbe` — COUNT-only @ mapped **`0x30CD0`** (AOB; no CC-pad; thiscall-esi)
- Parse/write caps raised to **68** (was silently ignoring stereo=68)
- Chain: PublishSync **66 PASS** → ReplayDispatch → `call [eax+0x178]` → live **`0x220D0`**
- Dual plan NOTES only (docs): inject L/R at **view+0x80** before call @ **`0x30D0D`** — **not implemented**

**When ready — ONE test:**
1. Deploy ASI
2. stereo **`68`** → `Mode68: COUNT armed exeRva=0x30CD0` then `cadence=PASS|SHARED|REJECT`
3. Kill **`45`**

---

## Session note 2026-07-25 ~02:00–03:36 — mapped seam RE (user playing; no deploy)

**Shipped to out-asi only:**
- ViewConst Mode **64** TRUE start **`0x32470`**; PublishSync **`0x30F00`**; ViewMat **`0x314C0`**
- Mode **41/42** OWNER-EDGE @ **`0x30D13`** + upload rets **`0x2A2183`/`0x2A25FF`** (ReplayGate triad; READ-ONLY)
- Mode **65** slot178 first-bytes; Mode **66** log notes 11/12 PublishProj pairing
- PublishProj **`0x31BA0`** (11 callers); viewport fn **`0x31110`** optional ViewMat
- Helper **`0x31940`** ×58 E8 → Mode **66** may report **SHARED** (avg>4), not REJECT
- ViewConst gate BSS **no writer** → prefer ladder **`45→66→67→65→64→41→42`**
- Scripts: `offline-seam-mapped.py`, `verify-mapped-sites.py`, `offline-publishproj-map.py`, `offline-31940-clusters.py`, …
- Docs: `MAPPED_RVA_CHEATSHEET.md`, RE_OFFSETS CRITICAL + ladder reorder
- ResolveReSite prefers expected mapped RVA when prologue matches
- Upload fn 0x2A1E10 (vtable): two MatMulx3->+178 paths; sibling 0x2A1D50 helper-only
- ReplayGate-guarded +178 triad: 0x30D0D + 0x2A217D + 0x2A25F9 — Mode42 OWNER-EDGE logs all three rets
- Mode66/67 COUNT: PASS / SHARED / REJECT labels
- **No AGENT_LOOP ticks** — continuous offline only

**When ready to test — ONE mode:**
1. Deploy ASI
2. stereo **`66`** → `Mode66: COUNT armed exeRva=0x30F00` then `cadence=PASS|SHARED|REJECT`
3. Kill **`45`**
4. Later: **67** → **65** → **42** (triad OWNER-EDGE); **64** last

---

## Session note 2026-07-25 ~01:45 — PE +0xC00 RVA skew fix (user playing; no deploy)

**Root cause:** CE `.text` mapped RVA = file offset **+0xC00**. Mode 66 REJECT was wrong fixed RVA.

**Shipped to out-asi only:**
- `ResolveReSite()` AOB + corrected expected RVAs
- Mode **64/66/67** AOB-armed COUNT (PublishSync **0x30F00**, ViewConst **0x32470**, ViewMat **0x314C0**)
- Mode **66** CC-pad relaxed
- VsRet constant **0x2D33E** (was 0x2C73E file-off) for Mode 34/41 filters
- `re_validate` + `offline-re-scan.py` + RE_OFFSETS CRITICAL banner

**When user returns — ONE test:**
1. Deploy ASI (game must be quit)
2. stereo **`66`** → expect `Mode66: COUNT armed exeRva=0x30F00` then `cadence=PASS|REJECT` with real entries
3. Kill **`45`**

---

## Session note 2026-07-25 ~01:15 — Mode 67 ViewMatWriter COUNT (no game launch)

**Shipped:**
- **Correction:** ViewMatWriter TRUE start = **`0x308C0`** (not mid **`0x308C9`**) — *superseded: that was still file-offset; mapped is **`0x314C0`***
- **Mode 67** — Mode 45 renderer + COUNT-only MinHook; dual=OFF
- Ladder **`45→66→67→65→64→41→42`** (*superseded order — prefer 66 first*)

**Kill:** **`45`**. **No dual.**

---

## Session note 2026-07-25 ~07:00 — Mode 66 PublishSync COUNT (no game launch)

**Shipped:**
- **Mode 66** (`PublishSyncCountProbe`) — Mode 45 renderer + COUNT-only MinHook @ **`0x30300`** prologue; SEH passthrough; 45 EndScene session; auto-reverts to **`45`**; dual=OFF; no **`0x300D0`** hook
- **`stereo_config.h/cpp`** — `stereo=66` opt-in; default stays **`45`**
- **`re_validate.cpp`** — PublishSync **`0x30300`** anchor bytes
- **`docs/RE_OFFSETS.md`** — ladder **`45→64→65→66→41→42`**; Mode 66 safety table

**When user returns — ONE headset test per session (ladder order):**

| # | stereo | Capture in log |
|---|--------|----------------|
| 0 | **45** | `StereoSubmit: L=1 R=1 mode=45` |
| 1 | **64** | `Mode64: COUNT done … cadence=PASS` |
| 2 | **65** | `Mode65: VT178-LOG … slot178=0x… activeView=0x…` |
| 3 | **66** | `Mode66: COUNT done … cadence=PASS` |
| 4 | **41** | `Mode41: CHAIN … ret=0x3199A fn=0x3187C` |
| 5 | **42** | `Mode42: OWNER-EDGE … call=FF/2@0x3010D` |

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`. **No dual.**

---

## Session note 2026-07-25 ~06:30 — Full `0x30300` PublishSync RE (no game launch)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — complete **`0x30300`** prologue→ret disasm; **`0x2FBF0`** matrix-sync chain; **`[0x17F583C]`** active-view map (1 write @ **`0x3006F`**, ~116 reads); **12** static **`E8`** callers (not only **`0x3187C`** tail); safe live ladder **45→64→65→66→41→42** with exact log lines
- **`scripts/offline-re-scan.py`** — `scan_active_view_global`; full **`0x30300`** caller list; capstone mark @ **`0x30349`**
- **`stereo_config.h` / `stereo_render.cpp`** — Mode **66** stub comments; Mode **64/65** RE comment polish (no new hooks)

**RE findings:**
- **`0x30300`** syncs **`[this+0xC0/0x100/0x200]`** via three **`0x2FBF0`** muls, then **`call 0x300D0`** only when **`[0x17F583C]==this`**
- Inside **`0x300D0`**: skip if **`[0x17ED918]!=0`**; else **`call [device+0x178]`** + **`+0x1B4`**
- Callers: **`0x3187C`** + **`0x308C9`** tails share PublishSync; plus VP/publish family + 3 distant sites
- **Mode 66** (PublishSync COUNT) — implemented in next session (~07:00)

**When user returns — ONE headset test per session (ladder order):**

| # | stereo | Capture in log |
|---|--------|----------------|
| 0 | **45** | `StereoSubmit: L=1 R=1 mode=45` |
| 1 | **64** | `Mode64: COUNT done … cadence=PASS` |
| 2 | **65** | `Mode65: VT178-LOG … slot178=0x… activeView=0x…` |
| 3 | **41** | `Mode41: CHAIN … ret=0x3199A fn=0x3187C` |
| 4 | **42** | `Mode42: OWNER-EDGE … call=FF/2@0x3010D` |

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`. **No dual.**

---

## Session note 2026-07-25 ~06:00 — Publish tail + device singleton (no game launch)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — corrected publish tail RVAs (**`0x30300`/`0x30FA0`/`0x30C10`**, not `0x430xxx`); device singleton **`[0x17ed8d8]`** map (247 reads, 2 init writes); Mode **65** doc
- **`scripts/offline-re-scan.py`** — disasm publish callees + `scan_global_device_singleton`
- **Mode 65** (`ReplayVtable178Log`) — Mode 45 renderer + read-only EndScene peek at vtable **`+0x178`**/**`+0x1B4`**; **no game hook**; opt-in **`stereo=65`**

**RE findings:**
- **`0x30300`** syncs view matrices then, when active view matches **`[0x17f583c]`**, calls **`0x300D0`** → **`call [eax+0x178]`** — **most stereo-relevant** publish callee
- **`0x30FA0`** builds projection/tangent block at **`[this+0x308..]`** — medium relevance (constants, not draw owner)
- **`0x30C10`/`0x30A60`** writes VP rows **`[this+0x1C0..0x1F8]`** — high relevance for view data, shared with **`0x308C9`**
- Device singleton: only static writes are init **`0x23A48`** and clear **`0x24DA8`**; max vtable slot **`0x248`**

**When user returns — pick ONE headset test (in order after baseline):**
1. **Baseline:** Mode **45** — smooth? F7 steps 8.0→12.0→30.0?
2. **COUNT:** stereo **`64`** — `Mode64: COUNT done … cadence=PASS`?
3. **Chain:** stereo **`41`** — slots **`0x3199A/0x319A4`** → **`fn=0x3187C`**?
4. **Vtable log:** stereo **`42`** then optional **`65`** — `slot178=` RVAs for replay dispatch

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`.

---

## Session note 2026-07-25 ~05:00 — Deep offline RE (no game launch)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — disasm notes for CopyMat/FovSite/VS wrapper/BuildRootA; corrected
  indirect anchor **`0x3010D`** (`call [eax+0x178]`); view-const candidate **`0x3187C`**
- **`scripts/offline-re-scan.py`** — linear disasm, E8 caller graphs, VS-region scan, anchor verify
- **`re_validate.cpp`** — 4 CopyMat AOBs + anchor byte checks + polished summary
- **F7** 11-step leanGain to **30.0** on Mode 45 path (`eyeDelta = hmdDelta / leanGain`)

**RE conclusion (updated):**
- Mode 42 **`0x30D13`** return is **mid-SSE** inside fn `~0x308C9` — do not hook there
- Static indirect dispatch **`0x3010D`** is the next live observation target (`eax`, `[eax+0x178]`)
- Best **aligned** replay/view candidate: **`0x3187C`** — needs COUNT-only ≥45 EndScenes before any trial
- **SameFrameSeamGate=CLOSED**

**When user returns — pick ONE headset test:**
1. **Baseline:** Mode **45** — smooth? F7 new steps 8.0→12.0→30.0 for huge-world feel?
2. **RE validate:** stereo **`62`** — `ReValidate: summary required=7/7 … anchors=7`?
3. **Chain probe:** stereo **`41`** — do `0x3199A/0x319A4` rows show enclosing **`0x3187C`**?
4. **World size (visual):** `fovadd` **20** alone (not F7+fovadd same session)

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`.

---

## Session note 2026-07-25 ~04:30 — RE offset map (offline; no game launch)

**Shipped:**
- **`docs/RE_OFFSETS.md`** — durable CE 1.2.0.59 RVA/AOB map, forbidden sites, VsRet chain,
  re-find procedure
- **`scripts/offline-re-scan.py`** — PE scan without Steam/GTA
- **`re_validate.cpp`** — runtime AOB drift check (`ReValidate:` in log)
- **Mode 62** (`RePatternValidate`) / **Mode 63** (`SameFrameSeamGate`) — Mode 45 renderer +
  RE logging; **default stays 45**
- **F7** 8-step leanGain (1.0…5.0); stereo file **46–61 → 45** remap

**RE conclusion:** Same-frame distinct eyes still need a **replay-thread draw owner** (~1×/frame,
hookable thiscall). VsRet **`0x2C73E`** is observation-only. **`0x309D0`** indirect edge needs
live **`eax`/vtable** capture (Mode 42 follow-up) — do not hook yet.

**When user returns — pick ONE headset test:**
1. **Baseline:** Mode **45** — smooth? F7 steps 1.0→5.0 make world smaller when leaning?
2. **RE validate:** stereo **`62`** — log shows `ReValidate: OK` for all required AOBs?
3. **Chain probe:** stereo **`41`** — 1 calm minute; save `Mode41: CHAIN` lines for next RE pass
4. **World size (visual, not F7):** F6 stereoscale 125→130; or try `fovadd` **20** (not both at once)

**Kill:** **`45`** · fallback **`30`** + delete `fovadd`.

---

## Session note 2026-07-25 ~01:30 — Mode 61 renderer-soft + worldsize preset

**Shipped:**
- **Mode 61** (`HeadOwnedCamRendererSoft`) — Mode **45** camera unchanged; renderer = RT lock +
  pair-hold + AER pose submit + soft guard (no Mode-40 1.5° flatten). **SameFrame=0** (replay seam
  still blocked).
- **`gtaiv_dxvk_vr.worldsize`** — single visual-scale knob (75–125). **100** = fovadd 18 +
  stereoscale 125. Deployed **110** with default stereo **45**.

**Default deploy:** `stereo=45`, `worldsize=110`, `ipd=3`, `scale=100`.

**Test Mode 61:** write `61` to stereo file, restart, headset-check jump vs 45. Kill **`45`**.

**Test worldsize:** restart after editing worldsize file; verify log line. Kill: delete worldsize +
  restore `fovadd=18` / `stereoscale=125` individually if needed.

**Still deferred:** safe same-tick distinct-eye replay seam — no new hooks this session.

---

## Session note 2026-07-25 ~00:45 — Camera frozen at Mode 45; next = renderer + world size

**Decision:** Stop camera ownership experiments for this phase. Mode **45** is the only default
deploy. Modes 46/59 remap to 45; 47–60 stay in code for manual tests only.

**What “VR renderer + world size” means next (honest scope):**

| Track | Goal | Not a substitute for |
|-------|------|----------------------|
| **Same-frame stereo** | Find a safe replay-thread seam (`VsRet=0x2C73E` chain) so L/R eyes render from **one game tick** with distinct view constants | Another CopyMat / VS-translate / forced-walk cam mode |
| **Renderer comfort (Mode 61)** | AER + soft guard on Mode 45 cam — less flatten, not true stereo | Same-frame or cam rewrite |
| **World size (visual)** | **`gtaiv_dxvk_vr.worldsize`** (75–125) sets **fovadd + stereoscale** together; deployed **110** | F7 WorldScale alone — that only scales **6DoF lean**, not building scale |
| **World size (6DoF)** | F7 linear scale 100–500 at user discretion | Quadratic scale, scale=1000, or cam-anchor clamps |

**One change per session.** Headset proof: `StereoMode: 45`, paired `StereoSubmit: L=1 R=1 mode=45`,
`Mode44: RT lock … recreates=0`, **no** `Mode5x` / `forcedWalk=1` / `VrMove: Mode57` lines unless
you explicitly set an experimental stereo file.

**Kill:** `45` · fallback `30` + delete `fovadd`.

---

## Session note 2026-07-24 ~20:25 — Mode 46 full HMD-basis / tilt test

### Correction ~20:30 — Mode 46 froze after loading and is disabled

The only headset test of Mode 46 froze after the loading screen. Mode 45 immediately recovered
and then passed a two-minute post-load run with advancing `StereoSubmit: L=1 R=1 mode=45`.
The direct OpenVR right/up/forward matrix passed to the late RAGE camera path is therefore unsafe
despite its nominally correct rigid basis. `46` now maps to protected `45` in the config parser;
the deployed stereo file remains `45`. Do not use or re-arm 46, and do not start a Mode 47
translation/WorldScale experiment from that broken pose path.

For every later mode, deploy with its intended stereo file, restart, and require continuing
post-load paired submits over at least two minutes. If the log stalls or the game hangs, write
`45`, restart, verify continuing Mode-45 submits, and document the failure before any new work.

Mode 45 is the protected user-tested stability baseline: no bars, no jitter/jumping, stable
performance. The remaining immediate camera symptom is head tilt appearing to move the world in
the inverse direction. Inspection found that `ApplyHmdToCam` preserved only the HMD forward vector,
then rebuilt right/up against GTA's world-up; it necessarily deletes physical roll.

**Shipped Mode 46:** retain Mode 45's exact Mode-44 RT lock, FOV path, motion guard, and safe
post-CCam late refresh. Only replace that orientation reconstruction with the full OpenVR
right/forward/up basis, converted through the already-proven Ovr→GTA mapping. Controller/vehicle
yaw rotates all three axes together. No CCam+VS stack, no canvas zoom, no change to IPD, no replay
hook/draw, and no forbidden address. F7 remains translation-only: higher WorldScale produces a
larger game-unit translation for the same real lean, i.e. a smaller-feeling world.

**Headset check:** F9; tilt left/right slowly. Horizon must tilt with the head and the world must
remain spatially stable. Then look up/down and lean; confirm no inverse counter-motion. Expected
log: `StereoMode: 46`, `fullHmdBasis=1`, `Mode46 ... fullBasis=1`, `recreates=0`, and paired
submits. If any regression, write **`45`** and restart; do not modify the Mode-45 baseline.

**Still not true VR:** Mode 46 improves the head-owned mono/temporal render view but does not create
same-tick distinct eyes or decouple collision/culling. The next root milestone remains a proven safe
Rage replay-thread seam for two distinct camera views in one game tick.

---

## Session note 2026-07-24 ~20:05 — Mode 44 RT lock

**Shipped Mode 44:** this is the Mode-43 fast current-frame mono guard with exactly one
additional behavior: after the first 1536×1536 submit+hold RT allocation, later canvas/tangent
requests on the same D3D9 device cannot release and recreate those four default-pool textures.
The true-FOV publish gate is also 5% for this mode (rather than the normal 2.5%), filtering
ordinary CCam FOV jitter before it can alter canvas geometry. Bars-gone `fovadd=18`, pair-hold,
and the Mode-43 three-copy guarded path remain unchanged. No game function is hooked, replayed,
or probed.

**Launch proof:** build `20260724-200503` logged `StereoMode: 44`, the RT-LOCK install,
one `eye RTs 1536x1536 OK`, `Mode44: RT lock 1536x1536 initialized recreates=0`, and paired
`StereoSubmit: L=1 R=1 mode=44`. The unattended launch cannot prove a performance gain or a
long-session `recreates=0` periodic counter. **Kill:** `43`, `40`, `37`, or `30` + delete
`fovadd`.

**Next true-VR work:** do not mistake resource stability for same-frame stereo. The open
root problem remains a safe replay-thread seam that renders distinct L/R views in one game tick;
the forbidden/disproved addresses and the 0x309D0 indirect edge stay off limits. Get a headset
FPS/rapid-turn check of Mode 44 before a new visual experiment.

---

## Session note 2026-07-24 ~20:00 — Mode 43 fast motion guard

**Shipped Mode 43:** keep Mode 40's thresholds, temporary-mono result, pair-held true-FOV geometry,
and all replay seams untouched. On a guard firing only, the current R frame still first lands in
`holdR` to preserve the pose/cadence decision, but its L/R current-frame canvases go directly to
the submit textures. This skips Mode 40's extra recanvas to `holdL`/`holdR` plus the two-texture
promotion: **3** full canvas copies in a guarded R epoch instead of **5**. Calm frames use the
unchanged Mode-37 temporal pair-hold path.

**Launch proof:** build `20260724-200006` deployed `StereoMode: 43`, installed the FOV site,
allocated locked `1536x1536` submit+hold RTs, and repeatedly logged `StereoSubmit: L=1 R=1
mode=43`. The unattended launch did not produce HMD motion, so `directSubmit=1` is intentionally
not claimed as live-motion proof yet. It is a rapid-turn overhead reduction, not a same-frame
distinct-eye solution and not a normal-frame FPS cure. **Kill:** `40` (conservative guard), `37`,
or `30` + delete `fovadd`.

---

## Session note 2026-07-24 ~20:15 — Mode 41 replay-chain probe

**Shipped Mode 41:** retain Mode 40's Mode-37 true-FOV pair-hold and its rapid-motion temporary-mono
guard, but add a **read-only** trace only when the real replay-thread VS upload returns to
`VsRet=0x2C73E`. It records each valid stack return with its slot, detects the direct/indirect CALL
immediately before that return where possible, resolves a candidate function start/ABI for logging, and
ranks the result by EndScene epochs. It uses the already-installed D3D9 `SetVSConstF` observation hook:
**no game-function hook, no draw replay, no camera write, and no claimed same-frame stereo.**

**Live result:** build `20260724-194401` reached `VsRetHits=142077` with `SameFrame=0
distinctEyes=0`, paired eye submits (`mode=41`), and no new hook. The trace reconfirmed the forbidden
`0x37BD0` chain and the stackarg chains. It also exposed a frequent `0x30D13 → 0x309D0`
`thiscall-56` node, but its caller is not statically recoverable; direct-call nodes
`0x3199A/0x319A4` resolve only to an unaligned enclosing start. Therefore neither is a safe next
count/replay target yet.

The next probe must establish `0x309D0`'s actual caller ABI/object lifetime without calling it, or map
the indirect edge around it. Do not call it yet. Keep Mode 40/41 as the safe interim and do not revive
Mode 34 dual.

**Kill:** write **`40`** (same headset behavior, no trace), **`37`** (always temporal stereo), or
**`30`** then delete `fovadd`. Mode 41 is a seam-finding step, not the true-VR finish.

---

## Session note 2026-07-24 ~19:50 — Mode 42 resolved the indirect edge

Mode 42 deployed as another **read-only** Mode-40-compatible build. The live replay chain now proves
that `0x30D13` returns from `FF 90 disp32`: `FF /2@0x30D0D`, ModRM `0x90`, length 6. In other words,
the frequent `0x309D0` chain node is reached through `call [eax+disp32]`, not a directly callable
owner function. `mode=42` paired submit remained healthy, and the log honestly reports
`SameFrame=0 distinctEyes=0`, `hook=NO`, and `replay=NO`.
The deployed `20260724-195512` artifact also restores modes 41/42 to the explicit stereo-submit
gate; its log confirms `StereoSubmit: L=1 R=1 mode=42`.

**Next safe probe:** observe `eax` plus the resolved `[eax+disp32]` vtable target at that exact
indirect call site, without invoking it. Do not count-hook/replay `0x309D0` based on its outer
thiscall-looking prologue alone. The target's object lifetime and ABI must be proven before a
same-tick dual attempt.

---

## Session note 2026-07-24 ~19:45 — Mode 39 rejected; Mode 37 restored

**User feedback:** Mode 39 brought black bars back and did not improve jumping or FPS. This is
consistent with the log: Mode 39 actively capped the requested `fovadd=18` to **12°**
(`FovSite ... add=12`), so it necessarily reduced the true-FOV fill while keeping the same
alternating-eye pair-hold renderer.

**Deployed next headset baseline:** `stereo=37`, `fovadd=18`. It restores the no-bars geometry
that Mode 39 removed, while retaining the locked 1536 canvas, pair-latched tangents, and no
CanvasZoom. This is a geometry restoration, **not** a jump or FPS cure.

**Research decision:** L4D2VR/HL2VR render both engine views in one simulation tick; UEVR also
labels alternating/AFR as its last-resort, nausea-prone path. GTA's remaining true-VR gap is
therefore a safe same-tick Rage replay/render seam with distinct L/R view/projection data. Do
not spend another build on a compositor-pose or FOV-cap variation. Independent VR camera stays
after that seam is understood and has a hard fallback to 30/37/38.

---

## Session note 2026-07-24 ~19:38 — Mode 40 motion guard shipped

The safe same-frame **distinct-eye** seam is still blocked: replay-thread VS/draw work is separate
from the game-thread BuildRootA/ExecRoot paths, and the known replay candidates are either forbidden
or proven ineffective/unsafe (`0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`). Do not describe the
previous Phase dual as a solution: it is same-frame but has identical camera images because its view
constants were baked before replay.

**Shipped Mode 40:** keep Mode 37's no-bars FOV/canvas and pair-hold. If the cached L/R HMD poses differ
by ≥1.5° rotation or ≥2 cm translation, rebuild both eye canvases from the current R game frame and submit
them together. This is a deliberate temporary-mono safety valve for rapid HMD turns, not true VR stereo.
It should reduce the worst stale-eye jump while retaining normal stereo when calm. Live log proof:
`StereoSubmit: L=1 R=1 mode=40` and `Mode40: MOTION-GUARD mono pair … SameFrame: L+R from current R
frame rot=…`.

**Headset test:** turn your head quickly, then stand still. Expect less jump during the turn and depth to
return after it. If the temporary mono is worse than the jump: write **`37`** to
`gtaiv_dxvk_vr.stereo`. Hard fallback: **`30`**, then delete `gtaiv_dxvk_vr.fovadd`.

**Next concrete true-VR probe:** a read-only replay-thread call-chain trace around `VsRet=0x2C73E` that
records only validated return/caller relationships and call cadence—no function hook—until it identifies
a replay owner that can safely replay a draw batch twice with distinct view constants. Do not retry any
forbidden RVA or Mode-34 dual.

---

## Session note 2026-07-24 ~19:15 — Mode 38 AER pose submit

User: Mode 37 still monitor/huge/jump/FPS; F7 ≠ presence; Luke AER v2 Patreon.  
**Shipped Mode 38:** Mode 37 + capture-time HMD pose + `Submit_TextureWithPose`. Not AER v2 OF.  
Kill → **37** or **30**. Presence still deferred (independent cam / same-frame).

---

## Session note 2026-07-24 ~19:30 — Mode 39 low-motion FOV comfort

Mode 38 logged successful `Submit_TextureWithPose` but user still reports severe jumping. That rules out
another pose-submit variation as the best next move; it cannot make the temporally stale eye newly rendered.
**Shipped Mode 39:** Mode 37 true-FOV canvas/pair-hold, but cap the effective FOV ADD at **12°** even when
the preserved `fovadd` file says `18`. This is one comfort/FPS behavior change only: less FOV fill/crop stress,
no CanvasZoom, no new hooks, and no AER v2 optical flow.
**Test:** check `Mode39: fovadd requested=18 capped=12` plus pair promotion and mode-39 submits. Compare the
same head turns/walk against 38. Kill → **38** (restore pose/full FOV), **37** (full FOV), or **30** + delete
`fovadd` (smoothest protected baseline). Same-frame remains the sole root-cause path if this does not help.

---

## Session note 2026-07-24 ~19:00 — Mode 37 comfort retune

Mode 36 killed bars but cost **jump + FPS + huge world**. Pair-hold was still active.  
**Shipped Mode 37:** true-FOV canvas kept + Mode30 pair-hold; fovadd **18**; canvas **1536 locked** (no RT flap); pair-latch rects; WorldScale **100**. Protect 36/35; fallback 30.

---

## Session note 2026-07-24 ~18:45 — Mode 36 true-canvas FOV

Mode 35 engine FOV worked but **canvas still used stale D3DTS_PROJECTION** → black bars remained.  
**Shipped Mode 36:** publish post-`fovadd` CCam FOV → canvas tangents (no CanvasZoom). Deployed **fovadd=22** (~90% v fill). Mode **35** protected as kill.  
Also: ParseModeFile max 36; idempotent ADD; ADD only if base FOV ≤55°.

---

## Session note 2026-07-24 ~18:20 — Mode 35 FOV recompute spike

Research (Luke/Halo/L4D2/UEVR/FusionFix): **true engine FOV** kills monitor-on-face; canvas zoom / claimed FOV warp = REJECTED.  
**Shipped:** Mode **35** = Mode 30 pair-hold + chain-hook FusionFix Custom FOV CALL → ADD at `CCam+0x60` via `gtaiv_dxvk_vr.fovadd`.  
Playable fallback stays **30**. Independent VR cam still deferred.

---

## Session note 2026-07-24 ~18:00 — canvas zoom KILLED

Headset: `gtaiv_dxvk_vr.zoom` claimed-FOV warp made every head move warp the world (same class as FusionFix FOV look-up warp).  
**Code:** zoom path forced no-op (true FOV). **Deploy:** Mode **30**, `ipd=1`, `zoom=100` (ignored).  
Do **not** re-enable canvas zoom or FusionFix FieldOfView for size.

---

## Deferred (do not implement this session)

### Independent VR camera vs game-camera wall collision

**User ask:** look sideways/up near walls → game cam collides / stops; want VR view independent of game cam.  
**Verdict:** **Yes, sound idea — deferred.** See `docs/INSPIRATION_NOTES.md` (Luke Ross pitch override + render-time view fix).  
Invasive: culling, weapons, shadows, HUD. Keep PedHide + eyefwd for head embed; revisit after same-frame or when wall-look is the #1 complaint.

### Mode 34 dual

COUNT may probe; **dual stays DISABLED** (`0x4DDAD0` vsPatch=0). New read-only `VsRetStatic:` E8→`0x2C73E` scan logs safe CC-pad starts for a future same-frame attempt. Playable = **30**.

### Mode 35 theater/quad A/B

Research: theater = menus/cutscenes only (Luke/VC VR). Not the presence fix. Skip unless FOV site fails.

---

## 1. What the last tests proved

| Setting / build | World size | Fusion | Jump | Notes |
|-----------------|------------|--------|------|-------|
| Mode 26, scale 100%, IPD×scale | too big | near-perfect with F8 | mild | proportions OK after removing VS+CCam double |
| Mode 26, scale 150%, IPD×scale | **top** | **gone** | **violent** | effective eye sep ≈ 9 cm |
| Mode 26, scale 150% 6DoF only, IPD = HMD ~6 cm | too big again | jump less | better | size feel was mostly from **extra parallax**, not 6DoF |
| Mode 30 + canvas zoom 85 | less telephoto? | — | **warp on head move** | **REJECTED** — kill zoom |
| Mode 35 + fovadd | better presence | keep Mode30 path | — | engine FOV ADD; bars remain (canvas ignored ADD) |
| Mode 36 + fovadd=22 | bars gone | keep Mode30 path | **strong jump + FPS hit** | true-canvas; RT flap from FOV wobble |
| Mode 37 + fovadd=18 + scale=100 | F7 smaller lever | Mode30 pair-hold | RT locked | comfort retune; headset-verify |

**Conclusion:** F7 “WorldScale” was doing two jobs at once (L4D2-style). Cranking it for size also cranked stereo disparity → temporal stereo cannot fuse that. Decoupling was correct for fusion; size must be fixed by **another lever** (real engine FOV — Mode 35/36/37 — not claimed canvas FOV).

---

## 2. Why the world still feels huge (even with “correct” IPD)

Three separate effects stack:

1. **Letterbox / window (Mode 14 canvas)**  
   Game FOV (~59° vertical) inside G2 (~94°+). Black bars top/bottom = you look through a smaller window. Brain often reads that as “giant world / drone / monitor on face”, even when stereo geometry is right.

2. **Temporal stereo**  
   L and R from different frames. Motion (look around) → smear. Large disparity → jump. Halo explicitly disables **motion blur** for the same “stereo echo” reason.

3. **No same-frame dual eye** yet  
   Halo MCC VR, L4D2VR, BotW BetterVR all render **both eyes in one tick** with per-eye pose/FOV. We still alternate frames (Mode 14/26/30). That is why jump/smear/“half FPS feel” remain.

IPD alone cannot fix (1). Scale×IPD “fixed” (1) by fake size cues and broke fusion. Canvas zoom “fixed” (1) by lying about FOV and broke head stability.

---

## 3. What Halo-MCC-VR teaches us (relevant parts only)

Repo: https://github.com/pancreations/Halo-MCC-VR (OpenXR, D3D11 — different stack, same VR physics).

| Halo practice | Meaning for GTA IV |
|---------------|--------------------|
| **True per-eye stereo same-frame** | Our #1 remaining stereo bug is temporal. Same-frame is mandatory for “real 3D” without jump. |
| **Engine FOV ≈ 120** (user setting) | Their #1 display rule: wrong FOV = wrong scale + edge pop-in. For us: **raise Rage vertical FOV** (Mode 35 / FusionFix site), not square resolution / not canvas zoom. |
| **`resolution_scale` ≠ world/IPD** | Separate knobs. We must keep **F7 size** and **F8 IPD** separate (already started). |
| **Motion blur off** | Reduces stereo echo / smear. Check FusionFix / GTA blur settings. |
| **FSR off (breaks VR scale)** | Analog: don’t let post-process stretch fight the canvas. |
| Stereo proof toggle (F11 alternate) | They treat temporal L/R as a **dev check**, not the product. |

Do **not** copy their OpenXR/D3D11 code 1:1. Copy the **priority order**: FOV fill → same-frame stereo → HUD/input polish.

---

## 4. Recommended roadmap (one change per session)

### Phase A — Size without breaking fusion

**A1. Soft stereo scale** — shipped (`stereoscale`). Keep.

**A2. Engine FOV (Halo lesson)** — Mode **35** FOV recompute site + `fovadd`. Headset-verify.  
**A2-REJECTED:** canvas `zoom` claimed-FOV (warp).

**A3. Motion blur off** — FusionFix. Keep.

### Phase B — Kill jump (main stereo milestone)

**B1. Same-frame on replay thread** — Mode 34 dual disabled; use `VsRetStatic` E8→`0x2C73E` safe starts for next probe. Never `0x2C6AC`/`0x37BD0`/`0x1BF010`/`0x4DDAD0`. Fail → **30**.

**B2. Desktop mirror** — left eye only later.

### Phase C — After fusion + size + no jump

- Independent VR cam vs game-cam collision (**deferred** — see above).  
- HUD inward, aim-cam, third-person, OpenXR only if OpenVR solid.

---

## 5. What we will *not* do next

- Re-enable canvas zoom / FusionFix FieldOfView for “less telephoto”.  
- Stack VS patch + CCam IPD again.  
- Multiply F8 IPD by F7 WorldScale.  
- Square `commandline` without FOV fix.  
- `D3DTS_PROJECTION` widen / blind `CCam+0x60`.  
- Mode 34 dual arm without a proven VS-upload walker.  
- Invasive independent VR cam this session.

---

## 6. Suggested next session (pick ONE)

→ **Headset:** Mode **37** + `fovadd=18` + `scale=100` — bars still gone? Less jump than 36? Better FPS? World less huge?  
→ Soft kill → stereo **36** or **35** (keep fovadd). Hard kill → **30** + delete `fovadd`.  
→ If still huge: F7 to **125/150** (6DoF only) — do **not** touch IPD.  
→ If still jump: try fovadd **15**; same-frame remains the real jump kill.

Protect playable: **`stereo=30`** fallback, Mode **36/35** protect, **`ipd≥1`**, pedhide/eyefwd/stereoscale.

---

## 7. Current knobs (for the user)

| File / key | Role now |
|------------|----------|
| `gtaiv_dxvk_vr.stereo` = **`37`** (protect **`36`/`35`**, fallback **`30`**) | Comfort true-canvas / Mode36 / Mode35 / pair-hold |
| `gtaiv_dxvk_vr.fovadd` = **`18`** | Degrees ADD at CCam+0x60. Kill = delete/`0` |
| `gtaiv_dxvk_vr.ipd` = **`1`+** | F8 cycles 1,2,3,4,6,7,10 |
| `gtaiv_dxvk_vr.scale` = **`100`** | 6DoF; HIGHER = smaller world |
| `gtaiv_dxvk_vr.stereoscale` = `125` | Soft disparity |
| `gtaiv_dxvk_vr.eyefwd` = `20` | With PedHide |
| `gtaiv_dxvk_vr.pedhide` = `1` | Kill → `0` |
| `gtaiv_dxvk_vr.zoom` | **DISABLED** in code |

Baseline to protect: **Mode 30 pair-hold** + Mode 36 no-bars true FOV.
