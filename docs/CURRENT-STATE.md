# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-28 — Mode **178** lab (same-frame full cam freeze)

### Live
- Stereo file: **`178`** (kill: **`170`**)
- Stack: Mode170 dual + eye-canvas + **freeze FULL pre-IPD cam** (basis+pos); only ±IPD differs between L/R DrawScene

### Also in ASI (not live; switch sidecar to test)
| stereo | Change vs previous |
|--------|-------------------|
| **179** | 178 + skip canvas `ColorFill` (keep cover crop) |
| **180** | 179 + phone Submit bounds `uMin=0.14` `vMin=0.42` |

### Deploy proof (buildId `20260728-155849`)
Log confirmed: `StereoMode: 178`, `Mode178: dual cam freeze begin … fullBasis=1`, `full cam freeze capture`, `DRAWSCENE dual … everyN=1`, `StereoSubmit L=1 R=1 mode=178`. Inter-eye cam delta before overwrite often `dpos≈0` (origin freeze already pins ped eye).

### Headset test (you)
1. SteamVR on → play until in-world (game may already be running).
2. Compare to 170: depth still fused? flicker better on foot / driving?
3. Bad → put `170` in `gtaiv_dxvk_vr.stereo` next to `GTAIV.exe` and report.
4. Still flickers hard → try `179` (no ColorFill). Phone next → `180`.

### Locked (do not re-try as “stable”)
| Path | Result |
|------|--------|
| BB mono both eyes (174/175) | Smooth, **no fusion / warp** on G2 |
| Dual without eye-canvas (176) | Flicker, **no 3D** |
| AER (177) | Flicker, no fusion, head warp |
| Mono-pair / sameL (172/173) | Still flickered |
| Ped **origin** freeze only (171) | Still flickered on foot |

### Not claimed until headset confirms
- Flicker-free stereo
- Perfect same-moment world (sim may still advance between DrawScenes outside cam)

---

## Prior: Mode **170** on `clean/mode-170`

### Headset truth (Mode 170)
- Strong FPS, clear **3D** via dual+canvas
- **Driving can still flicker**
- Kill: stereo **`167`** or **`0`**

### Fallbacks
| stereo | Role |
|--------|------|
| **170** | Fresh dual everyN=1 (3D + flicker) |
| **167** | Safer car/HUD family |
| **120** | Clean LKG dual everyN=2 |
| **0** | Mono / flat |

---

## Wins locked (context)
- Mono-Submit milestone done (stock DXVK 3.0.2 + interop)
- Mode **120** clean DrawScene dual was the smooth LKG before FP experiments
- Mode **170** = fresh dual every frame (performance-oriented dual, known flicker)
