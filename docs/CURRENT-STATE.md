# CURRENT-STATE — gtaiv-dxvk-vr

**Stand:** 2026-07-23 ~23:45  
**Version:** **1.5.0** — Mode 7 Spike (RT-Capture + HMD-Projection); default Stereo **aus**.

---

## Architektur (bewährt)

```
GTAIV.exe
  → d3d9.dll          = Stock DXVK 3.0.2 x32
  → FusionFix         = ASI-Loader + CE-Fixes (Graphics API = DX9 / unsere d3d9)
  → gtaiv_dxvk_vr.asi + openvr_api.dll
       → EndScene: WaitGetPoses → Mono-Submit L+R (gleiche Texture)
       → CopyMat-Hooks: FP-Cam am Ped + HMD-Orientierung + 6DoF (F9)
       → vr_move: Laufen in Blickrichtung + Right-Stick Yaw (invertiert korrigiert)
  → SteamVR → HP Reverb G2
```

| Pfad | Rolle |
|------|--------|
| Spielordner | `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV` |
| Log | `...\GTAIV\gtaiv_dxvk_vr.log` |
| Stereo-Kill-Switch | `...\GTAIV\gtaiv_dxvk_vr.stereo` (eine Ziffer 0–8) |
| IPD-Skala | `...\GTAIV\gtaiv_dxvk_vr.ipd` (Prozent 1–100); **F8** live durchschalten |
| Build | `.\scripts\build-asi.ps1` → `out-asi\gtaiv_dxvk_vr.asi` |
| Deploy + Start | `.\scripts\build-deploy-run.ps1` (Build→Kill→Deploy→Steam-Start) |
| Deploy only | `.\scripts\deploy-asi.ps1 -GameDir "<GTAIV>"` (`-Launch` startet mit) |

**Aktuell deployed:** `gtaiv_dxvk_vr.stereo` = **`0`** (Mono, ~90 FPS Ziel).

---

## Was funktioniert (behalten)

| Feature | Status | Details |
|---------|--------|---------|
| Stock DXVK 3.0.2 + Interop | Done | `ID3D9VkInteropDevice` / Texture → OpenVR Vulkan-Submit |
| Same-thread WaitGetPoses + Submit | Done | Extra Pose-Thread → `AlreadySubmitted=108` / Freeze — **nicht wiederholen** |
| Mono-Bild in Brille | Done | Gleiches BB an L+R; `errL=0 errR=0` |
| FP-Cam am Ped (`FindPlayerPed` + CopyMat) | Done | Nach ~360 Submits armed; 4/4 Call-Sites |
| HMD Orient + 6DoF relativ F9 | Done | Nur **F9** Recenter (keine Look-Toggles) |
| Eye-Forward ~0.38 | Done | Kopf/Haare teilweise „durchschauen“ ohne Natives |
| Locomotion + Stick-Yaw | Done | Stick-Yaw-Sign negiert (L/R war vertauscht) |
| HMD-Info-Log | Done | G2: recommended ~2836×2772, FOVh~94°, IPD~60 mm |
| BuildRenderList Mid-AOB | Done | Findet Fn trotz FusionFix-Prologue-Patch |

### Bekannter guter AOB (DrawScene::BuildRenderList)

- Mid-Body (überlebt FusionFix SafetyHook):  
  `83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C`
- Function start = mid − **13**
- File-Offset-Referenz (exe): mid ~`0x527EDE` → start ~`0x527ED1`  
  Runtime-Beispiel: `@ 00EAD200` (ASLR)

---

## Stereo-Modi (`gtaiv_dxvk_vr.stereo`)

| Mode | Bedeutung | Ergebnis (getestet) |
|------|-----------|---------------------|
| **0** | Aus (Default) | Mono-Submit, beste FPS (~90) |
| 1 | Probe: BuildRenderList loggen | OK |
| 2 | Dual BuildRenderList, gleiche Cam | Stabil; **zeichnet nicht zweimal** — nur Listen |
| 3 | Dual + IPD in CopyMat | IPD wirkte; kein echter Dual-Draw |
| 4 | Temporal L/R: Frame L / Frame R, StretchRect → 2 RTs, Submit | **Echte Parallaxe** (User: Tasse) — Fusion unvollständig; **FPS ~46** |
| 5 | Phase-Probe (Log-Reihenfolge) | Order: **PhaseA → PhaseC → DrawScene** |
| 6 | Same-frame PhaseA→C→Draw L dann R + BB-Submit | Stabil, Submit L=R=1; **keine Fusion**; ~50 FPS |
| 7 | Dual BuildRenderList + RT-Copy + Cam-IPD | Cam-Matrix L/R stimmt (bis 200 cm geloggt) — **BB ändert sich trotzdem nicht** |
| 8 | Mode 7 + L/R Submit getauscht | ~45 FPS / schlechter — nicht nutzen |

### Harter Befund (2026-07-24)

- F8 Sep 0→200 cm: Log `camDist=200cm`, User sieht **keinen** Bildunterschied → Capture ist kein echter Dual-Draw.
- `BuildRenderList` (PhaseA/C/DrawScene) ≠ RenderView. Listen 2× bauen + StretchRect = oft **dasselbe** Bild.
- Mode 4 (temporal) hatte echte Parallaxe — dort wird ein **ganzer Frame** mit der Cam gezeichnet.
- VSConstF-Hooks → ~45 FPS, Fusion nein → wieder **aus**.

**Mode 9 Ergebnis (VTable, 2026-07-24):**

| Phase | BuildRenderList slot | Execute-Kandidat (nächster Slot) |
|-------|----------------------|----------------------------------|
| PhaseA | `[7]=00928AC0` | `[9]=0053B750` (`[8]`=Stub) |
| PhaseC | `[8]=00D76950` | `[9]=0053E300` |
| DrawScene | `[8]=00ADD200` | `[9]=0049C250` |

Mode-9-FPS-Loch: AOB jeden Phase-Call — gefixt.  
**Mode 10:** Execute-only dual → L≠R, F8-Sep wirkte nicht. Build+Exec aus ExecA = Freeze — verboten.  
**Mode 11:** Build+Exec aus PhaseA-Build → **Blackscreen** — verboten.  
**Mode 7** = Performance-Baseline (~90 FPS, normales FOV) — Cover-FOV/Bounds wieder **aus** (fühlte sich wie kaputter FOV-Slider an).  
Mode 13 temporal bleibt experimentell (~½ FPS). Mode 11 tot. Kill **0**.

**Default im Code:** Mode **0**. Mode 4/6/7 nicht als Alltagspfad.

---

## Stereo-Erkenntnisse (wichtig)

1. **Parallaxe ≠ Fusion.** Mode 4 liefert unterschiedliche Winkel L/R, aber Bilder verschmelzen nicht sauber → Doppelbilder bleiben.
2. **`BuildRenderList` ≠ RenderView.** Zweimal aufrufen + BB kopieren = oft **dasselbe** fertige Bild. Kein BotW-äquivalenter Dual-Draw.
3. **Temporales Alternating-Eye** (Frame L, Frame R):
   - Pro Auge ~halbe Update-Rate → User sah **~46 FPS** (vorher ~90).
   - BotW lehnt Alternating bewusst ab; für uns nur Spike, kein Ziel.
4. **IPD-Rohwerte OpenVR stimmen:**  
   `L=(-0.030,…)` `R=(+0.030,…)` sepX≈60 mm.
5. **IPD über Cam-Basis (right/up/−forward) war falsch** — rechtes Auge bekam negatives X + großes Y.  
   Fix: Head-Rotation × EyeToHead-Translation, dann `OvrToGta`. Nach Fix L/R entgegengesetzt — Fusion trotzdem nicht gut (FOV/Projection fehlt).
6. **HMD-FOV → `CCam+0x60` (~94°)** → Freeze/Blackscreen nach Load — **nicht wiederholen** ohne neuen Plan.
7. Fehlende **per-eye Projection** (nur View-Offset) ist der wahrscheinlichste Restgrund für Diplopie bei korrekter Parallaxe.

### Ovr→GTA (Position/Richtungen)

```
gx = ox;
gy = -oz;
gz = oy;
```

Rage-Cam-Matrix: `right`=X, `up`=Y=**Forward**, `at`=Z=**Up**.

---

## Explizit gescheitert / verboten

| Versuch | Effekt | Regel |
|---------|--------|--------|
| Script-Natives aus EndScene (`SET_DRAW_PLAYER_COMPONENT` / PedHide) | Crash | Natives nur Script-Thread |
| FOV-Write ~94° an CCam+0x60 | Freeze nach Load | Nicht so wiederholen |
| OpenVR-Calls aus CopyMat (live `GetEyeToHead`) | Freeze | Nur gecachte Offsets |
| Pose-Thread getrennt von Submit | AlreadySubmitted / Freeze | WaitGetPoses+Submit same thread |
| BuildRenderList-Prologue-AOB | MISS (FusionFix JMP) | Mid-Body-AOB verwenden |
| Mode 4 Alltag | ~halbe FPS, Fusion unvollständig | Default 0; Dual-Draw suchen |

---

## Hotkeys / UX

- **F9** — SteamVR Recenter + 6DoF-Baseline  
- SteamVR-Dashboard zu (sonst `DoNotHaveFocus` / `errL`≠0)  
- Quick-Restart: `scripts/restart-gtaiv.*` (optional `-DirectExe` nach RGLess)  
- Start/Launcher beschleunigen: `docs/STARTUP_SPEED.md`  
- Inspiration (VC VR / C06alt): `docs/INSPIRATION_NOTES.md`  


- ntfy-Handy-Push bei Agent-Stop: Topic `cursor-henning-atldv3m1iqosbzh2` @ `https://ntfy.sh`  
  Hook: `~/.cursor/hooks.json` → `hooks/ntfy-on-stop/run.cmd`

---

## Drei BuildRenderList-ähnliche Phasen (CE exe, unique AOBs)

Alle teilen `cmp [edi+0x938], -1`, aber unterschiedliche Fortsetzungen:

| Label | File start | Mid AOB (unique) | Mid−start | Hinweis |
|-------|------------|------------------|-----------|---------|
| **DrawScene** | `0x6DC600` | `…FF 0F 84 DE 09 00 00 6A 00 6A 0C` | 13 | bisher gehookt |
| **PhaseA** | `0x527EC0` | `…FF 0F 84 76 03 00 00 80 3D` | 30 | großer Stack `0x6D8` → **SceneToGBuffer-Kandidat** |
| **PhaseC** | `0x975D50` | `…FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | 39 | dritte Phase |

RTTI-Strings in exe: `.?AVCRenderPhaseDrawScene@@`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@`.  
Virtuelle Calls (keine direkten `E8` auf die Fn) → Dual-Draw braucht korrekten `this` pro Phase.

**Mode 5** = Phase-Probe (nur Log). **Getestet 2026-07-23 — Erfolg.**

### Mode-5 Ergebnis (Log)

- Alle 3 Hooks OK: DrawScene `@00ADD200`, PhaseA `@00928AC0`, PhaseC `@00D76950`
- Stabile Reihenfolge pro Frame: **PhaseA → PhaseC → DrawScene**
- Stabile `this`-Pointer: PhaseA=`13CA5E60`, PhaseC=`13CB6580`, DrawScene=`10F352D0`
- Gelegentlich doppelte Triplets im selben `frameSeq` (vermutlich 2. Viewport / Reflection)
- `stereo` danach wieder **0**; Mode **6** als nächster Spike gebaut

### Mode 6 — Same-Frame Dual (neu)

Datei `gtaiv_dxvk_vr.stereo` = **`6`**

Ablauf (pro Frame, nach Warmup der `this`-Pointer + Eye-RTs):

1. Natürlich: PhaseA + PhaseC mit **Left**-IPD  
2. DrawScene-Hook: DrawScene Left → StretchRect RT_L → PhaseA+C+DrawScene **Right** → RT_R  
3. EndScene: Submit L/R wenn beide RTs gefüllt; sonst Mono-Fallback  

Kill-Switch: `0`. Log: `StereoSameFrame:`, `StereoSubmit: ... mode=6`.

**Risiko:** BuildRenderList füllt ggf. nicht sofort den Backbuffer → L/R können noch ähnlich sein; dann brauchen wir den echten GBuffer/Draw-Execute. Freeze → sofort `0`.

### Mode-6 Test (2026-07-23)

| | |
|--|--|
| Freeze | Nein |
| Fusion (ein Bild) | Nein — weiterhin Doppelbilder |
| FPS | ~50 (Mono ~90) |
| Stabilität | OK genug zum Laufen |
| `stereo` danach | wieder **0** |

**Deutung:** Pipeline läuft (kein Crash), aber StretchRect vom **Backbuffer** nach BuildRenderList liefert vermutlich noch kein echtes L/R-Paar (Listenbau ≠ Draw), und/oder Projection fehlt. ~50 FPS = Kosten der Doppel-Kette — erwartet.

## Nächste sinnvolle Schritte

1. **GBuffer / Execute finden** — nicht BB nach BuildRenderList; RTs der Deferred-Phase oder echter Draw-Call.  
2. Per-eye **Projection** (ohne alten FOV-Freeze-Pfad).  
3. Optional: Mode 6 nur zum Debug; Alltag = Mode **0**.

Erfolgskriterium Stereo: Parallaxe **und** Fusion, FPS akzeptabel.

---

## Kurz-Checkliste Start

1. SteamVR an, Dashboard zu  
2. `gtaiv_dxvk_vr.stereo` = `0`  
3. GTA starten → Log: `CamMatrix: 4/4 hooked`, `armed after 360`, `MonoSubmit … errL=0`, **kein** `StereoTemporal`  
4. FPS ~90 erwarten  

Kill-Switch bei Freeze: Spiel killen → stereo-Datei auf `0` → neu.
