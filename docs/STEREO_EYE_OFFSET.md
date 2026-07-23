# Stereo fusion — Mode 13 (L4D2VR / BotW)

## Mode overview

| Mode | Role |
|------|--------|
| **7** | Original: same-frame RT dual (do not touch) |
| **4** | Temporal L/R (legacy) |
| **13** | **StereoFusion** — recommended: temporal + cover-FOV + TextureBounds, L4D2 scale |

## L4D2VR

```text
halfIpd = (IPD * IpdScale * VRScale) / 2
origin  = HMD ± right*halfIpd + forward*(-EyeZ*VRScale)
FOV     = cover FOV from GetProjectionRaw (both eyes)
Submit  = TextureBounds per eye
```

**Higher VRScale** → more offset in game units → world feels **smaller**.  
Default Mode 13: **VRScale=100%**, Sep = HMD IPD (~6 cm).

## BotW BetterVR

- Patch cam per eye, **draw full frame** (for us: temporal Mode 13/4)
- Patch FOV/projection along with it (for us: cover-FOV + bounds)
- Alternating is not the end goal — Mode 13 is an intermediate step until same-frame draw

## Tuning

- **F7** VRScale (50–300%)
- **F8** Sep cm
- Kill switch **0**
