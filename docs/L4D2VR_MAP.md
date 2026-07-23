# L4D2VR — was wir abgucken (Datei-Map)

Repo: https://github.com/sd805/l4d2vr  
DXVK-Submodule dort: https://github.com/sd805/dxvk @ `d856042…`

**Nicht** deren Release-`d3d9.dll` in GTA legen. Nur Muster lesen und für GTA neu schreiben.

---

## Repo-Struktur (relevant)

| Pfad | Rolle |
|------|--------|
| `l4d2vr.sln` | VS-Solution, **x86** |
| `dxvk/` | VR-DXVK (IronWolf-basiert + L4D2-Hacks + async) |
| `L4D2VR/` | Spiel-Hooks (Source) + OpenVR-Nutzung |
| `thirdparty/` | u. a. OpenVR |

---

## Konkret in `L4D2VR/` (Dateinamen)

| Datei | Typische Rolle |
|-------|----------------|
| [`vr.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/vr.cpp) / `vr.h` | OpenVR-Init, Submit, Poses — **Hauptstelle für Abgucken** |
| [`hooks.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/hooks.cpp) / `hooks.h` | Engine-/D3D-Hooks (Source-spezifisch) |
| [`game.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/game.cpp) / `game.h` | Spielzustand |
| [`dllmain.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/dllmain.cpp) | DLL-Entry |
| `offsets.h`, `sigscanner.h` | Source-Offsets — **nicht** für GTA wiederverwenden |
| `SteamVRActionManifest/` | SteamVR-Input-Manifest |
| `manifest.vrmanifest` | SteamVR-App-Manifest |

Zuhause zuerst **`vr.cpp`** lesen (Submit), dann entscheiden, wo unser GTA-Glue hängt (ASI neben DXVK oder Code im Fork).

---

## Wonach suchen (wenn Repo geklont)

Im Ordner `L4D2VR/` nach typischen Symbolen greppen:

| Suchbegriff | Warum |
|-------------|--------|
| `VR_Init` / `VR_Shutdown` | OpenVR starten |
| `IVRCompositor` / `Submit` | Bild ins Headset |
| `GetVRDesc` / `Direct3DCreateVRImpl` / `IDirect3DVR9` | IronWolf-Mailbox |
| `TextureType_Vulkan` / `VRVulkanTextureData` | Submit-Payload |
| `TrackedDevicePose` / `GetEyeToHeadTransform` | Pose (Kamera später) |

Im Ordner `dxvk/`:

| Suchbegriff | Warum |
|-------------|--------|
| `d3d9_vr` | Mailbox-Implementierung |
| `async` | Stutter-Patches (optional übernehmen) |
| Viewport / resolution override | **L4D2-spezifisch** — nicht blind nach GTA kopieren |

---

## Was wir übernehmen vs. verwerfen

| Übernehmen | Verwerfen / neu für GTA |
|------------|-------------------------|
| OpenVR-Init + Submit-Reihenfolge | Source-Engine-Hooks |
| `GetVRDesc` → Vulkan-Texture → `Submit` | L4D2 HUD/Menü/Viewport-Hacks in DXVK |
| x86-`d3d9.dll` als Deliverable | VAC/`-insecure`-Annahmen |
| Async-Idee | GTR2/IronWolf-Renn-Flags |

---

## Empfohlene Reihenfolge zu Hause

1. `git clone --recurse-submodules https://github.com/sd805/l4d2vr.git` (nur lesen)  
2. Submit-Pfad in `L4D2VR/` finden und Notizen hier ergänzen (Dateiname + Funktion)  
3. Parallel: unser `dxvk` Submodule bauen (`init-submodules.ps1`)  
4. Unser Glue in `src/gtaiv/` nach dem gleichen Submit-Muster  

Siehe auch `docs/IRONWOLF_API.md`, `docs/HOME_CURSOR.md`.
