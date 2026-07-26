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

## Luke Ross R.E.A.L. VR (local Inspiration drop — techniques only)

Local: `inspiration/real vr all mods/` (gitignored). Contains `RealVR.ini`, per-game folders (GTA 5, Elden, CP2077, …), `RealVR64.dll`, ASI/injectors.  
**Do NOT copy or redistribute closed binaries as our product.** Learn knobs / docs / FAQ ideas only.

### Transferable ideas (from GTA V RealVR.ini + public LukeRoss00/gta5-real-mod FAQ)

| Idea | Luke Ross | Transfer to GTA IV CE (ours) |
|------|-----------|------------------------------|
| **FOV must match HMD** | `UniversalFOVFix`; wrong FOV = warp / wrong scale / edge pop-in | Same lesson as Halo. Our Mode 17 hit recompute fight — still deferred. **Never** fake canvas FOV (zoom warp killed 2026-07-24). |
| **Square render** | GTA5 `commandline` `-width 1080 -height 1080` | Useful **with** real engine FOV later; alone shrinks image (we already know). |
| **Alternate-eye stereo (AER)** | Temporal L/R + compositor timewarp / `Submit_TextureWithPose` | We already run temporal (Mode 14/26/30/37). **Mode 38** stamps each eye with capture-time pose via `Submit_TextureWithPose` (OpenVR #1253). |
| **AER v2** (Patreon) | Optical-flow / depth synthesize intermediate frames so both eyes feel “fresh” at 60 fps | **Not copyable** here (closed + CUDA/OF stack). Learn: legacy AER = our temporal; v2 = synthesis we skip. |
| **Override pitch limits** | Game clamps pitch in vehicles → HMD look blocked | Related to wall-collision / look fights below. |
| **Supplementary view fix at render** | Cam resists Change → fix during draw | Our CopyMat override is the partial analog. |
| **Hide / fix body at camera** | Helmet/masks / close body coords; neck stump warning | We have **PedHide** (SetDraw). Keep. |
| **Decoupled 3rd-person look** | Orbit cam vs HMD look separate | FP path different; idea maps to independent VR cam (deferred). |
| **Cutscene theater / dynamic stereo** | Flat screen or adaptive FOV modes | Later menus/cutscenes. |
| **Do not multiply scale×IPD** | Separation ≠ world scale | Already our rule (F7 vs F8). |

### Deferred: independent VR camera vs game-camera collision

**User hypothesis (2026-07-24):** inject a VR view independent of the game camera, because the game cam collides with walls when looking sideways/up.

**Opinion (agent):** **Sound, keep deferred.** Rage’s gameplay camera is constrained (collision, pitch clamps, animation takeovers). Driving HMD look through that object will always fight walls/ceilings. Luke Ross’s own FAQ describes the game “wrestling” for camera ownership and needing supplementary render-time view fixes + pitch overrides — same problem class. True VR decoupling = pose for **submit/view**, while game cam stays for AI/culling/collision (or a soft follow). That is invasive (culling, shadows, weapons, HUD). **Do not implement this session.** Spike later only after Mode 30 is solid and same-frame progress exists.

**When to revisit:** after same-frame stereo or when wall-look remains the top headset complaint with PedHide + eyefwd tuned.

---

## Alignment with our current status

1. Mode 0 mono + FP cam = bootstrap (like before VC VR stereo).  
2. Mode 4 alternating = explicitly the worse class per VC VR / BotW → not the goal.  
3. Mode 5 phase probe = path to "shared prep, two views per frame" without engine fork.  
4. Theater menu + HUD layer = later, after fusion.
5. **Playable now:** Mode **30** pair-hold + PedHide + ipd≥1. Canvas zoom **OFF**. Independent VR cam = deferred.
6. **Mode 35 (2026-07-24):** engine FOV via FusionFix recompute CALL + `fovadd` ADD at CCam+0x60
   (Halo/Luke lesson). Stereo default stays **30** unless headset prefers 35.

### More public-repo notes (2026-07-24 evening)

| Repo | Monitor-on-face fix | 6DoF | Transfer |
|------|---------------------|------|---------|
| Luke Ross GTA5 FAQ | FOV must match HMD; UniversalFOVFix | HMD pose + pitch overrides | FOV site, not canvas lie |
| Halo-MCC-VR | Engine FOV 120 | True per-eye same-frame | FOV first, then same-frame |
| L4D2VR | GetProjectionRaw → cover FOV + TextureBounds | VRScale + eye origins | We already split F7/F8 |
| UEVR | Per-eye projection matrices | Full 6DoF | Rage has no UE stereo path |
| FusionFix fixes.ixx | `CCam+0x60 += n*5` after cam process | N/A | **Mode 35 chain-hook** |

When the VC VR **source** is published later: read `src/vr/` + librw stereo/swapchain path first — not controller physics.

---

## Cross-mod research reconciliation (2026-07-24 ~19:45)

- **L4D2VR / HL2VR-style Source mods:** own the engine's `RenderView` seam. They make two
  `CViewSetup`s (eye origin, HMD-cover FOV, per-eye RT) and render left then right before the
  next simulation tick. Their `GetProjectionRaw`-derived texture bounds keep the engine FOV and
  submitted texture geometry consistent. Rage currently gives us no equivalent safe render-view
  hook, so copying that hook is not an option.
- **UEVR:** explicitly ranks native stereo first, synchronized sequential second, and
  alternating/AFR last because AFR advances game time between eyes and causes eye desync/nausea.
  This exactly matches the Mode 30/37/38 temporal jump. A pose-aware `Submit` can improve
  rotational reprojection, but cannot make an old eye a same-tick eye.
- **Transferable now:** sample one pose epoch on the submit/render thread; keep each eye's view,
  projection/tangents, canvas placement, and submit bounds coherent; render only once per eye;
  allocate eye RTs once and keep their size stable. Mode 37 already does the stable pair-held,
  true-tangent parts. Mode 39 changed only engine FOV and therefore cannot solve temporal jump.
- **Not transferable to stock GTA IV CE:** Source `RenderView`, UE native stereo interfaces,
  Vulkan multiview, hidden-area masks, and engine-owned per-eye culling. They require renderer
  ownership we do not have through the DXVK interop submit path.
- **Real remaining path to true VR:** find a safe Rage replay-thread seam that renders both
  camera views from one game tick with distinct view/projection data. Only after that should an
  independent head-owned render camera be spiked; it otherwise risks culling, collision, weapon,
  HUD, and shadow bugs.
