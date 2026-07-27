# Session Prep — 2026-07-27 (Modes 122–136)

**Tree:** `C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr-clean` only.  
**OpenVR:** **v2.12.14** / `IVRSystem_023` (SteamVR 2.12). Do **not** use 2.15 (VR_Init **105**).  
**Build (live):** `20260727-mode136-latelatch` · family binary (modes 120–136).  
**Boot default after Mode136 deploy:** stereo **`136`** · dualn **`2`**.  
**Kill always:** write **`120`** into `gtaiv_dxvk_vr.stereo` (+ dualn **`2`**). Harder: `51` / `45` / `0`.

**FusionFix:** `plugins\GTAIV.EFLC.FusionFix.cfg` has `SkipMenu=1` / `SkipIntro=1` / `FieldOfView=0` (kept).

**Mode 131 = FAILED (HMD starve):** Desktop Present ~90 smooth; headset hardcore freeze. Cause: EndScene BB→L+R only every **3rd** mono frame + POSEHOLD-CALM stale stereo → HMD starves (same class as Mode 122 HOLD snap).

**Mode 132:** better than 131 (every-frame BB→L+R) but desktop still feels smoother than HMD — Present shows live BB after WaitGetPoses; HMD pays StretchRect×2 + Submit during monolook.

**Mode 133 = FAILED (same-tex):** Headset **even worse** — extreme jumping, fusion completely gone. Cause: one StretchRect BB→L then Submit **same** texture for L+R. Do **not** use.

**Mode 134 = FAILED (HOLD freeze):** Still strong HMD freeze; desktop fine. Cause: dualn=3 off-tick **HOLD** of previous L/R + monolook not covering enough motion → Present shows live BB, HMD gets stale eye RTs. Same class as 131.

**Mode 135 (prior):** ALWAYS-FRESH — everyN=**1** dual; look → BB×2; never bare HOLD. Gameplay better; **head look still unnatural** (monolook↔dual + Submit_Default). Superseded by **136** for look-smooth test.

**Mode 136 (live):** LATE-LATCH — Mode **120** content (dualn=2, HOLD, **no monolook**) + every Submit uses `Submit_TextureWithPose` with HMD pose sampled **immediately before Submit**.

---

## How to switch stereo N (DE)

Game folder:

`C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV\`

1. Quit GTA fully (or Desktop **GTA IV Quick Restart** after file write).
2. Open Notepad → write **only** the number (e.g. `136`) → Save As  
   `gtaiv_dxvk_vr.stereo` (next to `GTAIV.exe`, ANSI/UTF-8 ok).
3. Optional: `gtaiv_dxvk_vr.dualn` = `2` / `3` / `4`  
   (Mode **135** forces **1**; Mode **121/127–134** force 3; Modes **122/125** force 4 — file ignored for those. Mode **136** uses file/default **2** like 120).
4. Start SteamVR → start GTA → check log:
   - `ASI_BUILD_ID 20260727-mode136-latelatch`
   - `StereoMode: 136`
   - `VR_Init OK`
   - `CLEAN LATE-LATCH` · `everyN=2`
   - `LateLatch: eye=L TextureWithPose=1 err=0` (and R)
5. Feel check → if bad: stereo **`120`**, dualn **`2`**, restart.

**No FPS HUD.** NVIDIA/RTSS overlays often fail in Mode 120+ — use `gtaiv_dxvk_vr.perf.csv` (`debug=1`).

---

## Mode table

| stereo | Role | dualn | What it should feel like | Kill |
|--------|------|-------|--------------------------|------|
| **120** | **LKG frozen** — last good smooth | **2** (file/default) | Smooth ~90, soft bars/crop per F7 worldscale; best baseline | 51/45/0 |
| **121** | Experiment (prior) | **3** forced | Lighter dual / more HOLD; may feel flatter on turns | **120** |
| **122** | **Perf / look stutter** | **4** forced | Less hitch when looking; **stale** HOLD (can snap) | **120** |
| **123** | **Vollbild / fill** | 2 (file) | fill≈100%h, mild V bars; explore can half-lock ~60–70 | **120** |
| **124** | **Zoom / worldscale** | 2 (file) | Starts at MatchH6; F7 dezoom ladder | **120** |
| **125** | **Combo 122+123** | **4** forced | Look HOLD + cover fill (stale HOLD risk) | **120** |
| **126** | **FPS + live look** | 2 (file) | 123 fill + POSEHOLD + **live BB both eyes**; stereoscale 135 | **120** |
| **127** | **FPS tight + LIVELOOK** | **3** forced | tighter POSEHOLD; LIVELOOK can **ghost** on look | **120** |
| **128** | **FPS no LIVELOOK** | **3** forced | ~90 FPS, stutter gone; look **double-image** (HOLD L≠R age) | **120** |
| **129** | **L/R sync** (prior) | **3** forced | look→same-tick dual (still diplopia — sequential L/R) | **120** |
| **130** | **monolook sync** (interim OK) | **3** forced | look→identical BB both eyes ≥350ms; ghost↓; pitch jumpy + FPS dips | **120** |
| **131** | **FAILED — HMD starve** | **3** forced | BB every 3rd + POSEHOLD-CALM → headset freeze, desktop smooth | **120** |
| **132** | HMD-every monolook | **3** forced | 131 pitch-stable; BB→both **every** EndScene; desktop still ahead of HMD | **120** |
| **133** | **FAILED — same-tex** | **3** forced | BB→L + same Submit L+R → extreme jump, fusion gone | **120** |
| **134** | **FAILED — HOLD freeze** | **3** forced | 132 path + dualn=3 HOLD → HMD freeze, desktop fine | **120** |
| **135** | ALWAYS-FRESH (prior) | **1** forced | every-frame dual; look/pose→BB×2; never bare HOLD; head still unrund | **120** |
| **136** | **LATE-LATCH (live)** | **2** (file/default) | Mode120 content + TextureWithPose late-latch each Submit | **120** |

Same ASI for all. Mode **120–135 code paths not mutated** — 136 is additive.

---

## Mode 136 test (DE) — do this now

1. Headset on · Desktop **GTA IV Quick Restart** (already **stereo=`136`**, dualn=`2` if untouched).
2. Log check (Notepad → `gtaiv_dxvk_vr.log` next to `GTAIV.exe`):
   - `ASI_BUILD_ID 20260727-mode136-latelatch`
   - `StereoMode: 136`
   - `CLEAN LATE-LATCH`
   - `LateLatch: … TextureWithPose=1 err=0`
   - `everyN=2` · **no** monolook / LIVELOOK lines
3. Stillstand: Gameplay weiter smooth ~90? (`perf.csv` if overlay fails)
4. Langsam + schnell Yaw **und** Pitch: natürlicher als **135**? Weniger „Stufen“?
5. Kill-Vergleich: stereo **`120`** + dualn **`2`**, restart — 136 besser beim Umschauen?

Full feel = your call. Agent already proved log lines above on this deploy.

---

## Prepared next (do **not** deploy yet)

### Mode 137 — Cam/Pose smoothing (blueprint only)
**When:** only if 136 look still judders.  
**Idea:** EMA / One-Euro on HMD→Game-Cam (Yaw/Pitch only); separate **Predicted** pose for cam from **Tracking** pose for Submit (temporal coherence).  
**Touch:** `look_move.cpp` / `hmd_pose.cpp` / cam CopyMat — **not** Mode 120/136 submit path.  
**Kill:** `120`. dualn stay 2. No monolook.

### Mode 138 — True same-moment stereo (blueprint only)
**When:** L/R micro-judder remains while turning.  
**Idea:** freeze game time between L and R DrawScene (safe hook only) **or** VS-IPD-only look path (no second full Draw).  
**Never:** BuildRootA×2.  
**Kill:** `120`.

### Mode 139 — Cover FOV / Zoom (blueprint only)
**When:** bars/zoom annoyance — **separate** from look-smooth.  
**Idea:** cover-matched FOV without Mode83 crop (123-class); Zoom via honest `gameTan` / stereoscale.  
**Do not** mix with 136/137 look changes in one build.  
**Kill:** `120`.

### Do not repeat
131 every-3rd HMD-starve · 133 same-tex · 122 stale LOOKHOLD · tight LIVELOOK flicker · FOV lies / SoftInset Cinema.

---

## Constraints

- Win32/x86 only · logs next to EXE · FusionFix SkipMenu OK · no OpenXR · Mode **120** frozen · Kill **120**.
- OpenVR **2.12.14** · no FPS HUD · ntfy only when build ready.
