# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-30 (home) — Motion: one bullet + G2 tip aim

**buildId `20260730-010155`** (deployed, not started)

### Headset feedback on `20260730-004820` (3D ComputeAimPoint)
1. **Two bullets** — one from stock gun, one from controller ray.
2. **Controller bullet ignores reload** / continuous fire (leave for test; explained below).
3. **Awkward aim** — pitch OK, but must hunt hits; can shoot behind; wrist bend to fire straight.
4. **Two freezes** after continuous shooting.
5. Pitch itself works.

### Freeze (logs)
- Session ended at last line: `AimOverride: EXCEPTION on FIRE_* — mouse-only fallback`
- Last successful fire: `AimOverride #420: FIRE ... via=3 (1=PED_WEAPON 2=BULLET)`
- **No** `WeapFollow:` activity (HARD-OFF still on)
- **Hypothesis:** continuous spam of **both** `FIRE_PED_WEAPON` + `FIRE_SINGLE_BULLET` (~10 Hz) plus mouse LMB + VrPad RT eventually faulted the game thread; SEH logged once then log stopped (freeze). Not weapfollow.

### Why continuous fire / no reload
We call `FIRE_PED_WEAPON` every `kFireIntervalMs` (100 ms) while the trigger is held. That **bypasses** the game’s ammo clip / reload / fire-rate logic. Left on purpose for aim testing. Fix later (ammo check or rising-edge only).

### This build — one change (aim-ray correctness)
**A) One projectile**
- Stop `SetMouseShoot` (no mouse LMB inject while natives fire)
- Drop `FIRE_SINGLE_BULLET` — only `FIRE_PED_WEAPON`
- VrPad: when `aimmode=2`, inject **RT=0** (stock pad shoot off); trigger still sets `IsVrPadShootHeld` for natives
- On FIRE exception → stop calling natives (`g_fireNativesDead`)

**B) G2 tip orientation**
- OpenVR pose is **grip**, not tip. Was using raw grip `−Z` → awkward / behind.
- New: pitch grip `−Z` toward local `+Y` by **55°** (G2 grip→aim / OpenXR tip)
- FIRE log lines include `fwd=(…) tipPitch=55`

Still hard-off: ped bone WRITE, weapfollow CREATE_OBJECT.

- `stereo=243` · `aimmode=2` · `vrpad=1` · `giveguns=1` · `weapfollow=0` · `handfollow=0`
- Kill aim: write `0` to `gtaiv_dxvk_vr.aimmode`

Log look for:
- New `ASI_BUILD_ID`
- `shootVia=FIRE_PED_WEAPON (no LMB/FIRE_SINGLE/RT)`
- `AimOverride #N: FIRE ... via=1 tipPitch=55` (via must be **1**, not 3)
- `VrPad: ... RT=0` while trigger held (grip LT can still be 255)
- **No** `WeapFollow: CREATE ghost`

### Test (concrete)
1. SteamVR on → Quick Restart GTA IV.
2. Open `GTAIV\gtaiv_dxvk_vr.log` — confirm new `ASI_BUILD_ID` and `via=1`.
3. On foot, grip to aim, trigger to shoot — expect **one** bullet (not two).
4. Point controller naturally forward (relaxed wrist) — bullets should go ahead, not behind.
5. Point up / down — pitch should still track.
6. Continuous trigger still sprays (no reload) — OK for this test; note if freeze returns.
7. Kill: `gtaiv_dxvk_vr.aimmode` = `0`, restart.

---

## 2026-07-30 (home) — Motion: controller-true 3D aim ray

**buildId `20260730-004820`** (deployed, tested — see above feedback)

### Why this step
Plan A natives worked, but aim was flattened to the horizon. This build kept full 3D controller forward (incl. pitch).

### Result
Pitch worked. Double-fire + wrong grip axis + FIRE_* exception/freeze → fixed in next build above.

---

## 2026-07-30 (home) — Stability: HARD-OFF weapfollow CREATE_OBJECT ghost

**buildId `20260730-004230`**

Ped bone WRITE hard-off. `weapfollow` CREATE_OBJECT ghost HARD-OFF after freeze/crash on `003848`. File ignored until safer path.
