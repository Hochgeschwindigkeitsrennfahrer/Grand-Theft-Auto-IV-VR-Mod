# Stereo fusion — Mode 13 (L4D2VR / BotW)

## Mode-Übersicht

| Mode | Rolle |
|------|--------|
| **7** | Original: same-frame RT dual (nicht anfassen) |
| **4** | Temporal L/R (Legacy) |
| **13** | **StereoFusion** — empfohlen: temporal + Cover-FOV + TextureBounds, L4D2-Scale |

## L4D2VR

```text
halfIpd = (IPD * IpdScale * VRScale) / 2
origin  = HMD ± right*halfIpd + forward*(-EyeZ*VRScale)
FOV     = cover FOV from GetProjectionRaw (beide Augen)
Submit  = TextureBounds pro Auge
```

**VRScale höher** → mehr Offset in Game-Units → Welt wirkt **kleiner**.  
Default Mode 13: **VRScale=100%**, Sep = HMD-IPD (~6 cm).

## BotW BetterVR

- Cam pro Auge patchen, **ganzen Frame** zeichnen (bei uns: temporal Mode 13/4)
- FOV/Projection mitpatchen (bei uns: Cover-FOV + Bounds)
- Kein Alternating als Endziel — Mode 13 ist Zwischenstufe bis same-frame Draw

## Tuning

- **F7** VRScale (50–300 %)
- **F8** Sep cm
- Kill-Switch **0**
