# Inspiration — Vice City VR + IV First Person

Status: 2026-07-23  
Local: `inspiration/vice-city-vr-0.1.0/` (currently only `README.md` — upstream repo has no source tree yet).

---

## Vice City VR (yevhen4817)

Repo: https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr  
Stack: **reVC** (reverse-engineered VC) + **librw** (RenderWare→D3D12) + **OpenXR**  
Not: ASI in the original `gta-vc.exe`, but a separate `reVC.exe`.

### What they get architecturally right (relevant for us)

| Idea | VC VR | Transfer to GTA IV CE |
|------|-------|-------------------------|
| **Single-pass / shared prep stereo** | One world-pass preparation (visibility, anim, lighting prep) for both eyes; render into **double-wide** stereo target → OpenXR swapchains | Target image instead of alternating-eye (Mode 4). With Rage we lack renderer ownership — next spike: phase pipeline 2× **in the same frame**, not frame L / frame R |
| **No second desktop world render** | Headset active → no full flat double draw | We submit from the DXVK backbuffer; monitor present remains, but no extra game tick |
| **Menu/cutscene = theater quad** | 2D unchanged, world-locked OpenXR quad | Later: pause/menu not with wrong stereo cam |
| **HUD = separate layer** | Separate OpenXR quad | Later; for now HUD is often unreadable in mono stretch |
| **6DoF + recenter** | OpenXR; chord recenter | We have this (F9 / SteamVR) |
| **OpenXR instead of OpenVR** | Primarily Quest Link | We stay on **OpenVR/SteamVR** (Reverb G2, existing path); idea is portable |

### What we **cannot** adopt 1:1

- Custom engine port (reVC) + custom renderer (librw D3D12)  
- Single-pass stereo / VRS / DLAA in the backend  
- ASI/CLEO for the original exe are incompatible with reVC per README  

**Conclusion:** VC VR confirms BotW/L4D2VR: stereo belongs in the **renderer**, with shared prep and two views **per frame**. For CE we stay on stock DXVK + ASI and must find the Rage draw path (Mode 5 phase probe).

---

## GTA IV First Person Mod (C06alt) [Legacy / "VR" thread]

Forum: https://gtaforums.com/topic/953517-reliv-gta-iv-first-person-mod-by-c06alt-legacy-v11-v122-v13-vr/

Local Inspiration drop (2026-07-24): `inspiration/firstperson mod/`
- `FirstPerson.asi` + `FirstPerson.ini` (Oculus / HMD=1) + `ReadmeV1.3.txt`

### What the Oculus `.ini` actually contains

| Knob | Inspiration value | Notes |
|------|-------------------|--------|
| `FPX / FPY / FPZ` | `0 / 0 / 0` | Inches: right / forward-from-face / up. Zero = eyes at face center (needs head hide) |
| `HMD` | `1` | Rift/HMD drives look |
| `ForwardFOV / FootFOV / RearFOV` | `111` | Their FOV — **we do not adopt** (FusionFix FOV look-up warp rejected) |
| `HMDPhoneDistance` | `180` | Phone prop distance — ignore for VR |
| Sens / autocenter / lock | various | Our HMD + F9 already cover look; skip |
| Crosshair colors | present | Later HUD work |

### What the ASI does (from strings / CE reverse)

| Idea | Inspiration | Transfer |
|------|-------------|----------|
| FP cam | ScriptHook `ATTACH_CAM_TO_PED` + offsets | **Already better:** CopyMat + ped eye + HMD |
| Head/hair hide | `SET_DRAW_PLAYER_COMPONENT` on **ScriptHook NativeThread** | Same effect via CE helper `SetDrawPlayerComponent` (AOB) — **no** EndScene natives |
| FOV 111 | `SET_CAM_FOV` | **Do not** re-enable FusionFix FOV / blind CCam FOV |
| HMD look | Built-in Oculus DK path | We use OpenVR poses |

**Adopted (2026-07-24):**
- PedHide via CE SetDraw helper (`gtaiv_dxvk_vr.pedhide`, default ON, kill=`0`)
- Optional `gtaiv_dxvk_vr.camoff` = `x y z` cm (FPX/FPY/FPZ style)
- Keep `eyefwd` (42) until PedHide proven; then try 12–20

**Not adopted:** FOV 111, ScriptHook dependency, Oculus HID, phone distance.

**Conclusion:** Not a stereo reference. Head hide + eye offsets are the useful bits — now wired without ScriptHook.


---

## Alignment with our current status

1. Mode 0 mono + FP cam = bootstrap (like before VC VR stereo).  
2. Mode 4 alternating = explicitly the worse class per VC VR / BotW → not the goal.  
3. Mode 5 phase probe = path to "shared prep, two views per frame" without engine fork.  
4. Theater menu + HUD layer = later, after fusion.

When the VC VR **source** is published later: read `src/vr/` + librw stereo/swapchain path first — not controller physics.
