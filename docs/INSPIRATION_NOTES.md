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

| Aspect | Use for us |
|--------|----------------|
| FP via ScriptHook / ASI | **Already covered** better: CopyMat + ped eye + HMD |
| `firstperson.ini` offsets | Ideas for eye height / forward (we use: `kEyeHeight`, `kEyeForward`) |
| Script-thread mesh/head hide | Exactly our planned path (natives **not** from EndScene) |
| Old "VR" builds | Often SBS/cinema, not Rage dual draw — little value for true stereo |
| Versions 1.0.7 / 1.0.8 | CE offsets differ; blind install risky alongside FusionFix |

**Conclusion:** Not a stereo reference. Useful as a reference for **script-side** head hide / bone offsets once we have a ScriptHook/native thread.

---

## Alignment with our current status

1. Mode 0 mono + FP cam = bootstrap (like before VC VR stereo).  
2. Mode 4 alternating = explicitly the worse class per VC VR / BotW → not the goal.  
3. Mode 5 phase probe = path to "shared prep, two views per frame" without engine fork.  
4. Theater menu + HUD layer = later, after fusion.

When the VC VR **source** is published later: read `src/vr/` + librw stereo/swapchain path first — not controller physics.
