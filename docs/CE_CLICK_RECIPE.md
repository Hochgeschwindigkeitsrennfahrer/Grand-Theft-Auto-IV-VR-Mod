# Cheat Engine — click-by-click (non-programmer)

Do this at home with Grok watching the log. Save often. Game windowed. CE as admin, attach to `GTAIV.exe`.

Fill results into [`ADDRESS_SHEET.md`](ADDRESS_SHEET.md).

---

## Prep (once)

1. Build + deploy ASI.
2. Next to `GTAIV.exe` create:
   - `gtaiv_dxvk_vr.stereo` = `243`
   - `gtaiv_dxvk_vr.aimmode` = `1` (probe only — no shooting natives)
   - `gtaiv_dxvk_vr.ikprobe` = `1`
3. Start SteamVR + game, load a save on foot with a pistol.
4. Open `gtaiv_dxvk_vr.log`. You want:
   ```text
   IkProbe: CPed*=0x........
   IkProbe: CPed+0xBxx -> 0x........ (candidate ikMgr)
   ```
5. Copy the `CPed*` hex into Notepad — that is your ped base for every recipe below.

---

## Plan B — CPedIKManager / vecAimTarget (highest value)

Goal: find two numbers for `gtaiv_dxvk_vr.ikoffs`: `<ikMgrOff> <vecOff>`.

### Steps

1. In CE: Memory View → Ctrl+G → paste the `CPed*` from the log → OK.
2. Right-click the address → **Dissect data/structures** → Structures → Define new structure → name `CPed_CE`.
3. Add elements until you cover at least `+0xC40`. Look for **pointer** fields between `+0xB80` and `+0xC40` (the log already lists candidates — start with those offsets).
4. For each candidate pointer `P`:
   - Ctrl+G to `P`.
   - Dissect a small struct (~0x100 bytes).
   - Find a **float3** (three floats) that looks like world coords (hundreds, not 0/1).
5. Verify the float3:
   - Stand still, **aim** (LT/RMB) at a wall in front.
   - In CE, **freeze** the three floats to a point **to your right** (add ~5 to X, leave Y/Z).
   - If Niko's arms/torso track the frozen point → **this is vecAimTarget**. Note:
     - `ikMgrOff` = offset of the pointer slot inside CPed (e.g. `BC0`)
     - `vecOff` = offset of the float3 inside the ik manager (e.g. `70`)
6. Unfreeze. Write next to the ASI (hex, no `0x`):
   ```text
   gtaiv_dxvk_vr.ikoffs   ->  BC0 70     (YOUR numbers)
   gtaiv_dxvk_vr.ikaim    ->  1
   gtaiv_dxvk_vr.aimmode  ->  2
   ```
7. Restart. Log must show `IkAim: offsets loaded …` then `IkAim #N: wrote (…)`.
8. Headset: grip-aim and move the controller — arms should track before you fire.
9. Kill: `ikaim=0`. If EXCEPTION in log → offsets wrong, do not retry with 1.0.7 paste.

**Never** enable `ikaim` with the 1.0.7 hint `BC0 70` unless CE confirmed those exact numbers on your CE build.

---

## Plan C — aim-cam CopyMat site (aim leaves FP)

1. `aimmode=1`. Note a log line that prints a cam matrix / `self=%p` (FovSite / CamMatrix).
2. CE: "Find out what writes to this address" on the live cam matrix (pos or forward float).
3. On foot **without** aiming: note the writers (our 4 sites).
4. **Aim** with pistol: a **new** writer appears → that caller's `E8` site is the 5th CopyMat.
5. Copy 16–24 bytes **after** that `E8` from Memory View as AOB.
6. Paste into [`ADDRESS_SHEET.md`](ADDRESS_SHEET.md) row #3. Grok adds `HookOneCall("aim_cam", "…")` next to the other four — **one behaviour change**, test FP-while-aiming before any override.

---

## Aim forward float3 (Plan D fallback)

1. Aim at horizon "north", Unknown initial value, Float.
2. Turn 90° while aiming → Changed. Return → Unchanged. Repeat.
3. Pitch up/down to isolate Z. Neighbours = X/Y (normalized float3).
4. Freeze while aiming and shoot — if bullets ignore mouse, you found the shoot direction.
5. Pointer-scan max level 3, save `.PTR`. Fill sheet row #1.

---

## Hand bone tags (read-only — already coded)

Log already probes `RHand=0x4D0`, `RForearm=0x4C9`, `RUpperArm=0x4C8`, `LHand=0x4C3`, `HEAD=0x4B5` via `PedGetBonePos` when aimmode≥2.

Confirm in log:
```text
HandBone #N: RHand=0x4D0 pos=(…) dHead=… dCtrl=…
```
If `RHand` FAIL forever → bone thiscall dead (separate bug). Do **not** write bones.

---

## Safety

- One CE find per evening if tired.
- After any poke that freezes/crashes: kill `ikaim`, `aimmode=0`, restart from last save.
- Do not NOP random instructions. Do not paste forbidden RVAs from `PLAN_NEXT.md`.
