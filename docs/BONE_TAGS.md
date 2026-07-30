# IV ped bone tags (reference — read-only)

Used by `ProbeHandBones` / `TryGetPedBoneWorldPos`. **Do not write these.**

| Tag | Hex | Notes |
|---|---|---|
| HEAD | `0x4B5` | Proven via cam_matrix Mode 216 |
| R_UPPERARM | `0x4C8` | probed |
| R_FOREARM | `0x4C9` | probed |
| R_HAND | `0x4D0` | probed — primary for dCtrl |
| L_HAND | `0x4C3` | probed |
| R_FINGER0.. | common IV SDK | try only if R_HAND OK |
| NECK | `0x4B4` | optional |

Source family: GTAMods Ped_Bones (IV) / IV SDK BONETAG_*.

If `HandBone` logs FAIL for every tag → PedGetBonePos thiscall is dead; fix that
before any hand visual work. Never fall back to GET_PED_BONE_POSITION native on CE
(already known FAIL).
