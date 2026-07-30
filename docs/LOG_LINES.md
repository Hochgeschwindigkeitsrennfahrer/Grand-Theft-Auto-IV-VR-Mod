# Log line cheatsheet (search these; do not re-explain)

| Session | Must see | Bad / kill |
|---|---|---|
| 0 | build OK, ASI next to exe | cl error → fix signatures only |
| 1 yaw | `AimOverride … camYaw=±NNdeg` changes when you stick-turn | camYaw stuck 0 while turning |
| 1 A/B | `(yawfix OFF)` with aimyaw=0 | — |
| 2 | `VrInput: manifest loaded` then `VrInput: ACTIVE` | ACTIVE never → SteamVR bindings |
| 2 | `tip=IVRInput` on FIRE | `tip=legacy+pitch` = vrinput not live |
| 3 | `aimmode=3` + FIRE #N ~1×/frame | #N jumps many per frame |
| 4 | `ammogate=1` / `AmmoGate: clip empty` | EXCEPTION → gate open, note ABI |
| 5 | `MenuBridge: profile N -> stereo M` | log switches, image not → restart |
| 6 probe | `IkProbe: CPed*=` | no line → FindPlayerPed miss |
| 6 write | `IkAim: offsets loaded` then `IkAim #N: wrote` | EXCEPTION → ikaim=0 immediately |

Kill files: `aimmode` `vrinput` `aimyaw` `ammogate` `ikaim` `ikprobe` → `0`; keep `stereo=243`.
