# Decoupled Aiming Plan (offline prep, 2026-07-29)

Goal chain: **decouple aim from head → weapon/hand follows the motion controller →
shoot along the weapon forward.** This doc is the full technical basis so home
sessions never start from zero. Written on the work PC: **no game, no headset, no
Cheat Engine** — every address marked UNVERIFIED must be confirmed at home.

Safety frame: singleplayer/offline only, Win32/x86, OpenVR. Everything gated by
`gtaiv_dxvk_vr.aimmode` (default **0 = off**); kill switch = delete the file or
write `0`. Never ship online.

---

## 1. Reference pattern — Holydh's GTA SA UEVR plugin (WeaponManager.cpp)

Source: https://github.com/Holydh/UEVR_GTASADE (`WeaponManager.cpp`, `MemoryManager.cpp`).
SA addresses do NOT transfer — the PATTERN does:

1. **Attach the weapon mesh to the motion controller** — in UEVR via
   `UObjectHook::get_or_add_motion_controller_state(weaponMesh)`
   (WeaponManager.cpp `UpdateActualWeaponMesh`). For us there is no UEVR; the
   equivalent is moving the weapon **bone/entity matrix** or letting the game IK
   solve toward a controller-derived target (§4).
2. **Compute a true barrel ray** — two points along the barrel in mesh-local
   space, transformed to world (`ProcessAiming`, the big per-weapon
   `point1Offsets/point2Offsets` table). We can start simpler: controller pose
   forward IS the ray; per-weapon fine offsets can come later.
3. **Replace the game's aim data in memory** — SA keeps an *aim forward vector*
   and an *aim camera position* the shooting code reads. Holydh:
   - **NOPs the game instructions that write those floats**
     (`MemoryManager.cpp NopMemory(aimingForwardVectorInstructionsAddresses …)`)
   - then **writes controller-derived values every frame**
     (`ProcessAiming`: `*aimForwardVectorAddresses[i] = calculatedAimForward…`).
   - When NOT aiming, he writes the camera matrix values back so the radar/UI
     stay correct — i.e. the override is *conditional on aim state*.
4. **Detect shots without a hook** — hardware breakpoints (DR0/DR1) on the two
   shoot instructions + a vectored exception handler set a `FirstWeaponIsShooting`
   flag (`MemoryManager.cpp InstallBreakpoints/ExceptionHandler`). Used only for
   recoil feel — NOT needed for our v1.
5. **Vehicle special case** — in cars he restores the game's own writers
   (`RestoreVehicleRelatedMemoryInstructions`) because free aim in cars fights
   the driveby system. We should equally scope v1 to **on-foot only**.

Transfer summary: find IV's aim-data floats (or a better seam), suppress/beat the
game writer, write our controller ray, keep radar/camera consistent when not
aiming, vehicles excluded in v1.

---

## 2. What GTA IV already gives us (better seams than raw float writes)

GTA IV is script-richer than SA. Three candidate seams, **ranked**:

### Plan A (RECOMMENDED FIRST): natives — zero memory writes

IV has script natives that do exactly what we need (names verified in IV-Network
`IV-Natives.txt`; CE availability must be proven at home, but our native path
already works — PedHide uses `SET_DRAW_PLAYER_COMPONENT` hash `0x3EFE3DC8`
successfully, `src/asi/ped_hide.cpp:19,62`):

| Native | Args | Use |
|---|---|---|
| `TASK_AIM_GUN_AT_COORD` | ped, x, y, z, durationMs | Aim the ped's gun at the controller-ray point. Repeat while aim button held. |
| `TASK_SHOOT_AT_COORD` | ped, x, y, z, durationMs, firingPattern | Fire at the controller-ray point (decoupled shot v1!). |
| `FIRE_SINGLE_BULLET` | x, y, z, tx, ty, tz, energy | Spawn a bullet from ANY point along ANY direction — full decouple, even from the controller muzzle. |
| `GET_PED_BONE_POSITION` | ped, bone, ox, oy, oz, &out | Read hand/weapon bone world pos (laser origin, debugging). |
| `FIRE_PED_WEAPON` | ped, x, y, z(?4 args) | Fire the equipped weapon at a coord (verify signature at home). |

Loop: `aimPoint = controllerPos + controllerFwd * 25m` → `TASK_AIM_GUN_AT_COORD`
each ~150 ms while the grip is held; trigger → `TASK_SHOOT_AT_COORD` or
`FIRE_SINGLE_BULLET`. The game animates Niko toward the point (engine IK does the
arms — free VRIK-lite!). Resolution path for handlers already exists:
`GetNativeHandlerPtr(hash)` / `GetNativeHandlerByName("NAME")` via ScriptHook
(`src/asi/native_invoke.h:21-25`) — the **name-based** route avoids hash hunting.

Risks: task natives may fight player input (TASK_* on the player sometimes needs
`CLEAR_CHAR_TASKS` or specific task slots); firing pattern enums unknown;
timing/thread rules same as PedHide (call from our game-thread hook sites, never
from EndScene raw). All testable in Session 2 with zero crash risk beyond a task
that does nothing.

### Plan B: IK aim-target write (engine solver does the arms)

IV-Network reversing (`IV-Reversing.txt`): `CPed + 0xBC0 = CPedIKManager*`,
`CPedIKManager + 0x70..0x80 = vecAimTarget (4 floats)` — **UNVERIFIED for CE**
(1.0.7-era offsets; our CE CPed offsets differ, e.g. m_pVehicle 0xB30, heading
0xAA0 — `src/asi/vr_move.cpp:42-45`). If the CE equivalent is found (CE recipe
§5), writing the controller-ray point there every frame while aiming may steer
the aim/arms with NO instruction patching. Cheap probe, cheap kill.

### Plan C: aim-cam forward override (Holydh-closest, ours-flavored)

IV's aiming runs through a dedicated aim CCam — the SAME CopyMat family we
already hook. `docs/HANDOFF_GROK.md` §4.6 records that aiming switches to a 5th,
un-hooked CopyMat call site. That gives a double win:

1. Find + hook the **aim-cam CopyMat site** (same `E8` pattern family, different
   context bytes — recipe §5.3). Fixes "aiming leaves first person" AND gives us
   the exact matrix the shooting code derives its direction from.
2. In the hook, replace the matrix **forward row** (RAGE layout: row `up` is
   forward — `src/asi/cam_matrix.cpp:29-34`) with the controller forward and the
   position with the controller/muzzle pos, mode-gated. Our existing late
   re-apply site (`HookCamFovSite`, `cam_matrix.cpp:1744-1803`) proves we can
   win write-order races against the cam pipeline without NOPs.

This is the Holydh pattern minus instruction NOPs (we hook the producer instead
of patching the consumers). NOPs stay a last resort (§6).

### Plan D (last resort): shoot-direction function hook

CE "what accesses" on the bullet start/end vectors during a shot → find IV's
`CWeapon::Fire`-equivalent → mid-hook and rewrite the direction args. Highest RE
effort, brittle across patches; only if A-C all miss (unlikely: A alone probably
ships the feature).

---

## 3. What we already have in THIS repo (reusable inventory)

| Piece | Where | Reuse for aiming |
|---|---|---|
| AOB scan + call-site hooking | `src/asi/aob.h:9-16` (`FindPattern`, `HookCallSite`) | Find aim-cam CopyMat site; any new pattern |
| Native invoke (hash + name via ScriptHook) | `src/asi/native_invoke.h:10-25`, proven in `src/asi/ped_hide.cpp:60-67` | Plan A natives |
| Player ped pointer | `FindPlayerPed` AOB `src/asi/cam_matrix.cpp:1415-1421` | ped arg for natives / IK writes |
| Ped bone world pos (any bone tag) | `ResolvePedGetBonePos` + `TryGetPedHeadBoneEye` `src/asi/cam_matrix.cpp:371-544` (HEAD=0x4B5) | Hand/weapon bone read — find hand bone tags at home, same thiscall |
| Live cam matrix + forward | `g_liveCamMat`, `BuildLiveViewMatrix16` `src/asi/cam_matrix.cpp:1598-1625`, `GetLastStereoCamPos` `:1589` | "cam vs aim" comparisons; radar-consistent fallback |
| OpenVR→GTA axis mapping | `OvrToGta` `src/asi/cam_matrix.cpp:100-104` (x, -z, y) | Controller pose conversion (same mapping) |
| HMD pose cache pattern | `src/asi/hmd_pose.cpp` | Template for the controller pose cache |
| **Controller poses (almost free)** | `TryMonoSubmit` already fetches ALL device poses per frame (`src/asi/openvr_mono.cpp:219-226`) — only the HMD is stored (`hmd_pose.cpp:23-35`) | `aim_decouple.cpp` consumes the same array — DONE in scaffolding |
| Float probe template | `ProbeCamFov` `src/asi/cam_matrix.cpp:1115-1171` | Same technique to scan candidate aim floats around a struct |
| Config-file pattern (read once, log) | e.g. `src/asi/stereo_config.cpp:2343-2360` | `gtaiv_dxvk_vr.aimmode` |
| Game-thread call sites for safe writes | CopyMat hook (`cam_matrix.cpp:1230`), FOV site (`:1652`), DrawWalk (`stereo_render.cpp:2265`) | Where Plan B/C writes and Plan A native calls run |

**NEW scaffolding added this session (compile-safe, off by default):**
`src/asi/aim_decouple.h/.cpp` — see §7. Wired into `openvr_mono.cpp` (pose cache
+ per-frame update) and `scripts/build-asi.ps1`.

---

## 4. Architecture (target state)

```
OpenVR WaitGetPoses (openvr_mono.cpp:226)
  └ AimDecoupleOnPoses: cache right (or left) controller pose        [DONE stub]
        │  OvrToGta axis mapping (cam_matrix.cpp:100)
        ▼
GetControllerAimPose() → world pos + fwd/up                          [DONE stub]
        ▼
ComputeAimForward() → aim ray = controller fwd (fallback: cam fwd)   [DONE stub]
        ▼
ApplyAimOverride()  — per aimmode:                                   [stub, home]
   A: natives  TASK_AIM_GUN_AT_COORD / TASK_SHOOT_AT_COORD / FIRE_SINGLE_BULLET
   B: write CPedIK vecAimTarget (CE offset needed)
   C: aim-cam CopyMat hook — replace fwd row + pos
        ▼
Game shoots along weapon/controller ray; radar/cam stay consistent
(when not aiming: no override — Holydh does the same)
```

Weapon/hand VISUAL following (weapon model at the controller) is a SEPARATE later
step (Session 3+): options are hand-bone IK via Plan B target, or attach-object
natives; do not block shooting on it — BotW/L4D2 lesson: decoupled shooting is
playable before hand visuals are perfect.

---

## 5. HOME RE session script (Cheat Engine, for a non-programmer, with Grok 4.5)

Preparation: game running windowed, singleplayer, save often. CE 7.x as admin,
attach to `GTAIV.exe`. **Change nothing else while scanning.** All finds go into
`docs/CURRENT-STATE.md` + the checklist table (§6).

### 5.1 Find the aim forward vector (float3)

1. Give Niko a pistol. Stand still. **Aim (RMB/LT) at the horizon due "screen
   north" and hold.**
2. CE: Scan Type `Unknown initial value`, Value Type `Float`. First scan.
3. Turn ~90° right while aiming; `Changed value`. Return to the original
   direction; `Unchanged value` (tolerance). Repeat turn/return 3-4×.
4. Look up/down while aiming: the Z component of a forward vector goes toward
   +1/-1. To isolate: aim level → scan `Value between -0.1 and 0.1`; aim up →
   `Value between 0.7 and 1.0`. This finds the **Z** float; the X,Y floats are
   the 4 and 8 bytes before it (check in Memory View — a normalized float3:
   x²+y²+z²=1).
5. Verify: freeze the three floats while aiming and shoot — if bullets ignore
   your mouse and fly the frozen direction, this IS the shoot-direction source.
   If bullets still follow the mouse, it was only a display copy — keep the
   address anyway (label "aim display"), continue scanning: use CE "Find out
   what writes to this address" on the frozen candidate to locate the producer
   function; its OTHER written addresses often include the real one.
6. Pointer-map it: right-click → `Pointer scan for this address`, max level 3.
   Save the .PTR. Also note the writing instruction module+offset (this is the
   Plan-D hook site and the AOB anchor). Restart the game, verify the pointer
   path resolves again.

### 5.2 Compare with camera forward (are they the same family?)

Our log already prints the live cam matrix (aimmode=1 probe prints cam fwd next
to controller fwd — §7). In CE, the aim floats found in 5.1: while NOT aiming,
walk and turn — if they always mirror the camera forward, they are the shared
cam/aim family (IV likely: aim cam = CCam; then Plan C is the natural seam). If
they only update while aiming, they are weapon-side (Plan D data). Either answer
is progress — record it.

### 5.3 Find the aim-cam CopyMat call site (Plan C, also fixes HANDOFF §4.6)

1. Build with `aimmode=1` (probe logging ON) and stereo mode unchanged.
2. In game: aim a pistol. Our 4 hooked sites stop being the active writer
   (HANDOFF_GROK §4.6: aim switches to third person / different path).
3. In CE: attach, `Find out what writes` to the live cam matrix address (get it
   from our log line `CamMatrix: FP lock` pos, or from the FovSite log `self=%p`
   → matrix at self+0x10). While aiming, a NEW writer appears — that is the
   aim-cam CopyMat (same function) called from a 5th site. Note the **caller**
   (Alt+click the instruction → stack/trace or use "break and trace") — the
   `E8 …` call site address, then copy 16-24 bytes AFTER the call from Memory
   View as the new AOB (mirror the 4 patterns at `cam_matrix.cpp:1435-1438`).
4. At home in code: `HookOneCall("aim_cam", "<new AOB>")` — one line next to the
   existing four. Test that aiming now stays first-person BEFORE any aim
   override work.

### 5.4 Find CE's CPedIKManager (Plan B)

1. Get the player CPed address (our log `CamMatrix: FindPlayerPed @ …` gives the
   function; simpler: CE pointer for `[GTAIV.exe+?]` — at home ask Grok to add a
   one-line log printing `g_FindPlayerPed(0)` — actually easiest: aimmode=1
   probe already logs the ped root pos; CE-scan that float3 to land inside CPed).
2. In Structure Dissect, walk +0xB80..+0xC40 for a pointer to a small struct with
   a plausible world-coord float3 around +0x60..+0x90 that моves to where you
   aim (vecAimTarget candidate; 1.0.7 said CPed+0xBC0 → +0x70, CE WILL differ).
3. Write test: freeze/poke the candidate to a fixed nearby coordinate while
   aiming — if Niko's arms/torso track the poked point, Plan B is alive.

### 5.5 Shot detection later (recoil feel only)

Skip in v1. If wanted: Holydh's HW-breakpoint pattern (`MemoryManager.cpp`) on
the shoot instruction found in 5.1-verify. Not needed for TASK_SHOOT/FIRE paths.

---

## 6. Missing addresses checklist

| # | Symbol | Purpose | Known in other mods? | Status | How to find at home |
|---|---|---|---|---|---|
| 1 | Aim forward vector (float3) | Read by shoot code; Plan C/D write target | GTA SA: yes (Holydh `aimForwardVectorAddresses`); IV: no public CE address found | **UNVERIFIED / missing** | §5.1 CE scan (aim + turn + freeze test) |
| 2 | Aim-cam position (float3) | Bullet origin family | GTA SA: yes (`cameraPositionAddresses`) | missing | Neighbors of #1 in the same struct (CCam?) |
| 3 | Aim-cam CopyMat call site (AOB) | 5th CopyMat hook; fixes aim→3rd-person; Plan C seam | Ours: 4 sibling sites `cam_matrix.cpp:1435-1438` | missing | §5.3 "find what writes" while aiming |
| 4 | CE `CPedIKManager` offset + vecAimTarget | Plan B engine-IK aim | IV 1.0.7: CPed+0xBC0 → +0x70 (IV-Network) — **UNVERIFIED CE** | candidate | §5.4 struct dissect |
| 5 | Native handlers: `TASK_AIM_GUN_AT_COORD`, `TASK_SHOOT_AT_COORD`, `FIRE_SINGLE_BULLET`, `GET_PED_BONE_POSITION` | Plan A | Names+argcounts: IV-Network `IV-Natives.txt`; CE handler table reachable via ScriptHook `GetNativeHandlerByName` (`native_invoke.h:25`) | resolvable at home | Log `GetNativeHandlerByName("TASK_AIM_GUN_AT_COORD")` etc. once at init (aimmode=1) |
| 6 | Hand/weapon bone tags (CE) | Laser origin, hand visuals | HEAD=0x4B5 works (`cam_matrix.cpp:357`); IV bone tag lists exist (BONETAG_R_HAND etc.) in IV SDK headers | partially known | Try common IV tags via existing `PedGetBonePos` + log distances; or `GET_PED_BONE_POSITION` native |
| 7 | Weapon entity/model matrix | Move weapon visual to controller | UEVR-SA: via engine objects (not transferable) | missing (later) | CE: find weapon render matrix while swinging; only Session 3+ |
| 8 | Firing pattern enum for TASK_SHOOT_AT_COORD | Plan A shoot | ScriptHookDotNet docs / IV decompiled scripts | lookup | Search decompiled IV scripts (`main.sc` dumps) for TASK_SHOOT_AT_COORD usage |

Forbidden/irrelevant: all `PLAN_NEXT.md` forbidden RVAs stay off limits; none of
them are aim-related — do not "reuse" them as shortcuts.

---

## 7. Scaffolding shipped this session (work PC, compile-safe, OFF by default)

New files (`src/asi/aim_decouple.h`, `src/asi/aim_decouple.cpp`), added to
`scripts/build-asi.ps1`, wired in `src/asi/openvr_mono.cpp` (pose cache after
`UpdateHmdPose`, update near the hotkey polls). **Not compiled here (no VS on
the work PC)** — first home build must verify.

Interfaces (header):
- `GetAimMode()` — reads `gtaiv_dxvk_vr.aimmode` once (0 off / 1 probe-log /
  2 override-reserved).
- `AimDecoupleOnPoses(poses, count)` — caches right/left controller pose from
  the SAME WaitGetPoses array the HMD uses. No extra OpenVR calls.
- `GetControllerAimPose(pos, fwd, up)` — controller pose in GTA axes
  (OvrToGta mapping identical to `cam_matrix.cpp:100`).
- `ComputeAimForward(fwd)` — controller forward, fallback HMD forward.
- `ResolveWeaponOrHandMatrix(out16)` — stub, returns false (needs checklist #6/#7).
- `ApplyAimOverride()` — **refuses** (logs once) unless aimmode=2 AND a future
  verified address/native handler is present. No game memory writes exist in the
  stub at all.
- aimmode=1 probe: ~1 Hz log line `AimProbe: ctrlFwd=(…) camFwd=(…) ctrlPos=(…)`
  for §5.2 comparisons and to prove the controller pose path on the G2.

Config placeholders (all optional files next to the ASI):
- `gtaiv_dxvk_vr.aimmode` — 0/1/2 (default 0)
- `gtaiv_dxvk_vr.aimhand` — 0 right (default) / 1 left
- (future, only when addresses verified) `gtaiv_dxvk_vr.aimaddr` — NOT read yet;
  do not invent values.

---

## 8. Safety rules (Track B)

1. Singleplayer offline only; never distribute with online play instructions.
2. One behavior change per build; every step has kill = `aimmode 0`.
3. Native calls only from proven game-thread sites (PedHide precedent), never
   raw from EndScene.
4. No hard-coded fake addresses: everything from §6 enters the code only after
   the CE session verifies it, labeled with date + game version.
5. Vehicles excluded from aim override v1 (Holydh lesson).
6. Do not break Mode 14 canvas, Mode 216/204 camera paths, or any protected
   baseline; aim code must be a pure add-on.
