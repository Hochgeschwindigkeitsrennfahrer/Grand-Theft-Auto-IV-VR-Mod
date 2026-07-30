# Address fill-in sheet (paste CE results here)

Update this file at home after each CE find. Grok reads it instead of re-deriving.

Game: GTA IV Complete Edition — ASI build id: `20260731-002523` (IkPin D58/+90 LOG ONLY) — Date: 2026-07-31

| # | Symbol | Value (paste) | How verified | Status |
|---|---|---|---|---|
| 1 | Aim forward float3 (addr or ptr path) | | freeze+shoot test | empty |
| 2 | Aim-cam position float3 | | neighbour of #1 | empty |
| 3 | Aim-cam CopyMat AOB (16-24 bytes after E8) | | new writer while aiming | empty |
| 4a | `CPed` → `CPedIKManager*` offset (hex) | `D58` | IkPin PASS d~0.2-1.2 vs ctrl (20260731-002523). Prior IkRay HEAP MATCH. | PASS pin; freeze pending |
| 4b | `vecAimTarget` offset inside IK mgr (hex) | `90` | float3 at P+0x90; live P=`0DDB4840` → +90=`0DDB48D0` | PASS pin; freeze pending |
| 4c | Sample `CPed*` from `IkProbe` log | (none this boot) | ikprobe off; prior sample `0D5C8E30` | no live CPed this boot |
| 4d | Live IK mgr pointer (IkPin D58→) | `0DDB4840` | latest IkPin line this session | live (changes each boot) |
| 5 | Natives OK/MISS (from AimNative log) | 29/29 OK | aimmode=3 | done |
| 6 | Hand bone tags working (RHand dCtrl) | | HandBone log | empty |
| 7 | Weapon entity / matrix | | later | empty |
| 8 | TASK_SHOOT firing pattern enum | | scripts | empty |

## ikoffs line (ready to drop next to GTAIV.exe)

```text
# only after 4a+4b verified with freeze+arms follow:
# gtaiv_dxvk_vr.ikoffs contents:
# D58 90
____ ____
```

## Notes / failed candidates

```
2026-07-31 IkPin PASS (build 20260731-002523, GTAIV still running):
  Latest IkPin: D58->0DDB4840 +90 float3; d range ~0.226-1.206 (avg ~0.54), src=ctrl.
  Live sample POINTER (IK mgr) = 0DDB4840; CE Ctrl+G freeze target +90 = 0DDB48D0.
  No IkProbe CPed* this boot (ikprobe off) - cannot compute CPed+0xD58 slot from log.
  Status: pin PASS; freeze PENDING (watch arms). ikaim=0. No ikoffs yet.

2026-07-31 Plan B candidate (build 20260731-001945, ComputeAimOrigin = ctrl world):
  IkRay HEAP MATCH best: CPed+0xD58 → heapP +0x90 float3 dRef≈0.25, Z≈18.17
  matching ctrl world origin. Also +0xA0 similar. L2 via D58+0x20→+0x30.
  Next ONE: ikpin=1 LOG ONLY pin D58/+90 vs ComputeAimOrigin (~2 Hz).
  Status: UNVERIFIED freeze — do NOT write ikaim / do NOT drop ikoffs yet.
  ikread=0 (no PAGE_GUARD). ikaim=0. ikray=0 while pin-testing.

2026-07-30 (restart after CE sleep fail) CPed*=0D5C8E30:
  +0xBB0 -> 00ED6EDC   slot 0D5C99E0 (EXE static — skip)
  +0xBC0 -> 0D5C8E30   (self — skip)
  +0xBD0/BD4/BD8 -> 3F800000 (float 1.0 — skip)
  +0xBF0 -> 0D5C8E30   (self — skip)
  +0xBF4 NOT listed this boot (slot would be 0D5C9A24)
  +0xC2C -> 0F9D8918   slot 0D5C9A5C (heap ptr); +70 did not move with aim
  +0xC20/C24/C28 float3 @ 0D5C9A50/54/58 — MOVES with aim, freeze Active locked
    values + hard nudge: arms did NOT follow → FREEZE MISS (likely aim output/copy,
    NOT vecAimTarget). C20 is NOT 4a/4b.

  CE "Find out what accesses" @ 0D5C9A50 (CPed+0xC20 float X) while aiming:
    Count 17:  7228AD3A  8B 0C 3E   mov ecx,[esi+edi]   (READ)
    Count 4678: 00C9F1AF  89 46 70   mov [esi+70],eax    (WRITE)
  Hot path = WRITE to [esi+70]; 0D5C9A50 ≈ (CPed+0xBB0)+0x70 = CPed+0xC20.
  ~4678 writes vs ~17 reads → float3 is mostly aim OUTPUT dump, not what arm IK
  reads → matches freeze MISS. Opcode 89 46 70 @ 00C9F1AF in game module —
  ASLR: prefer AOB / RVA (hint RVA 0x89F1AF if base was 0x400000), do NOT hardcode
  abs 00C9F1AF alone.

  ASI ikwriter recheck PASS (build 20260730-230016, ikwriter=1, ikaim=0):
  167 writer-slot samples; CPed+0xC20 (0,0,0)→live world (e.g. 604,1460,18→651,1410,15)
  while user aimed; DIFF lines present — C20 live aim-like float3 under game LT.
  ALL samples grip=0 ctrl=0 — ASI aim-held never saw LT (annotation gap).
  Freeze MISS stands (arms ignore) — do NOT write ikaim to C20 / do not invent ikoffs.
  Next ONE: fix aim-held detection (vr_pad / aim_decouple) so LT/grip → ctrl/grip=1
  in AimProbe/IkWriter annotations; then reconsider Plan B target (not blind C20).

  Aim-held PASS (build 20260730-230617): IkWriter grip=1 x44, AimProbe grip=1 x22 while LT;
    ctrl still 0 (no VR pose). ikaim=0; C20 freeze MISS → no C20 writes. Optional: hunt real IK read (not C20) or stop.

Prior boot CPed*=0D5F0F60 had +0xBF4->1036D990; CE A/B +70 on BF4/C2C did not move.
Never paste 1.0.7 "BC0 70" until freeze-test proves it on CE.
ikaim stays 0; no ikoffs.
```
