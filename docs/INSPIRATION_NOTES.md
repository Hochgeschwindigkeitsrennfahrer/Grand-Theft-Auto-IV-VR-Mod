# Inspiration — Vice City VR + IV First Person

Stand: 2026-07-23  
Lokal: `inspiration/vice-city-vr-0.1.0/` (aktuell nur `README.md` — Upstream-Repo hat noch keinen Source-Tree).

---

## Vice City VR (yevhen4817)

Repo: https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr  
Stack: **reVC** (reverse-engineered VC) + **librw** (RenderWare→D3D12) + **OpenXR**  
Nicht: ASI in die Original-`gta-vc.exe`, sondern eigener `reVC.exe`.

### Was sie architektonisch richtig machen (für uns relevant)

| Idee | VC VR | Übertrag auf GTA IV CE |
|------|-------|-------------------------|
| **Single-pass / shared prep stereo** | Eine World-Pass-Vorbereitung (Visibility, Anim, Lighting prep) für beide Augen; Render in **double-wide** Stereo-Target → OpenXR Swapchains | Zielbild statt Alternating-Eye (Mode 4). Bei Rage fehlt uns der Renderer-Besitz — nächster Spike: Phase-Pipeline 2× **im selben Frame**, nicht Frame L / Frame R |
| **Kein zweites Desktop-World-Render** | Headset aktiv → kein voller Flat-Doppel-Draw | Wir submitten vom DXVK-BB; Monitor-Present bleibt, aber kein extra Game-Tick |
| **Menü/Cutscene = Theater-Quad** | 2D unverändert, world-locked OpenXR Quad | Später: Pause/Menü nicht mit falscher Stereo-Cam |
| **HUD = eigene Layer** | Separates OpenXR Quad | Später; jetzt HUD oft unlesbar im Mono-Stretch |
| **6DoF + Recenter** | OpenXR; Chord recenter | Haben wir (F9 / SteamVR) |
| **OpenXR statt OpenVR** | Primär Quest Link | Wir bleiben bei **OpenVR/SteamVR** (Reverb G2, vorhandener Pfad), Idee ist portierbar |

### Was wir **nicht** 1:1 übernehmen können

- Eigenen Engine-Port (reVC) + eigenen Renderer (librw D3D12)  
- Single-pass Stereo / VRS / DLAA im Backend  
- ASI/CLEO der Original-Exe sind laut README mit reVC inkompatibel  

**Fazit:** VC VR bestätigt BotW/L4D2VR: Stereo gehört in den **Renderer**, mit shared prep und zwei Views **pro Frame**. Für CE bleiben wir bei Stock DXVK + ASI und müssen den Rage-Draw-Pfad finden (Mode-5-Phasenprobe).

---

## GTA IV First Person Mod (C06alt) [Legacy / „VR“-Thread]

Forum: https://gtaforums.com/topic/953517-reliv-gta-iv-first-person-mod-by-c06alt-legacy-v11-v122-v13-vr/

| Aspekt | Nutzen für uns |
|--------|----------------|
| FP per ScriptHook / ASI | **Schon abgedeckt** besser: CopyMat + Ped-Eye + HMD |
| `firstperson.ini` Offsets | Ideen für Eye-Height / Forward (wir: `kEyeHeight`, `kEyeForward`) |
| Script-Thread Mesh/Head hide | Genau unser geplanter Weg (Natives **nicht** aus EndScene) |
| Alte „VR“-Builds | Oft SBS/Cinema, kein Rage Dual-Draw — wenig für echtes Stereo |
| Versionen 1.0.7 / 1.0.8 | CE-Offsets anders; Blind-Install riskant neben FusionFix |

**Fazit:** Kein Stereo-Vorbild. Nützlich als Referenz für **Skript-seitiges** Head-Hide / Bone-Offsets, sobald wir einen ScriptHook-/Native-Thread haben.

---

## Abgleich mit unserem Stand

1. Mode 0 Mono + FP-Cam = Bootstrap (wie vor VC-VR-Stereo).  
2. Mode 4 Alternating = von VC VR / BotW explizit die schlechtere Klasse → nicht Ziel.  
3. Mode 5 Phase-Probe = Weg zum „shared prep, two views per frame“ ohne Engine-Fork.  
4. Theater-Menü + HUD-Layer = später, nach Fusion.

Wenn der VC-VR-**Source** später öffentlich wird: vor allem `src/vr/` + librw Stereo/Swapchain-Pfad lesen — nicht die Controller-Physik zuerst.
