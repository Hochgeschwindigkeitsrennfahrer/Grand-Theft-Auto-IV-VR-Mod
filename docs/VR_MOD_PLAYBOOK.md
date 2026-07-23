# VR-Mod Playbook — Orientierung für GTA IV CE

Ziel: vernünftige VR-Anzeige wie bei erfolgreichen Flat→VR-Mods.  
**Leitbild: BotW BetterVR** (Third-Person-Spiel → echte Stereo-VR).

**Aktueller Betriebsstand / Verbote / Modi:** → [`CURRENT-STATE.md`](CURRENT-STATE.md)

---

## Was die Referenz-Mods wirklich tun

| Mod | Ausgang | Kern-Trick | Für uns relevant |
|-----|---------|------------|------------------|
| **BotW BetterVR** | Third-Person (Cemu) | **Render-Loop 2×** bevor Game-State tickt; Cam L/R + FOV per Patch; Capture → Compositor | **Wichtigstes Vorbild** |
| **L4D2VR** | First-Person (Source) | Hook `RenderView` → RT left/right, origin ± IPD, FOV/Aspect, Submit L/R | Stereo-Pipeline-Muster |
| **HL2 VR Mod** | First-Person (Source) | Engine-VR / Dual-Eye + Motion Controls | Komfort/Input |
| **SubmersedVR** | First-Person (Unity) | Unity Stereo / HUD→Welt | HUD-Ideen; kein DXVK-Vorbild |
| **Vice City VR** (2026) | reVC + librw D3D12 | **Single-pass / double-wide stereo** + OpenXR; shared visibility/prep; Menü=Theater-Quad; HUD=eigene Layer | Architektur-Vorbild (nicht ASI-portierbar) — siehe `INSPIRATION_NOTES.md` |
| **C06alt IV FP** | ScriptHook ASI | FP-Cam / Offsets / ggf. Mesh | Head-Hide auf Script-Thread; Stereo eher nicht |

Gemeinsamer Nenner:

1. **Echtes Stereo** = Szene **zweimal zeichnen** (kein Alternating-Eye als Ziel).  
2. **Cam pro Auge**: Pose + EyeToHead (IPD) + **HMD-Projection**.  
3. **Game-Logic 1×** pro Stereo-Paar.  
4. Capture/Submit getrennt vom Tick.  
5. Avatar-Kopf ausblenden oder TP-Körper.

Mono-Submit = Bootstrap, kein Endzustand.

---

## BotW → GTA IV Mapping

```
BotW                                      GTA IV CE (Ziel)
─────────────────────────────────────────────────────────────
render twice / tick once               →  echten World-Draw 2× (nicht nur BuildRenderList)
Cam + FOV per eye                      →  CopyMat + IPD + Projection (FOV-Write bisher freeze)
Capture → Compositor                   →  DXVK Interop → OpenVR Submit L/R
```

### Nicht kopieren
Cemu/PowerPC, Vulkan-Layer-in-Layer, D3D12-Shim.  
Stack bleibt: **Stock DXVK 3.0.2 + ASI + OpenVR**.

---

## Was wir schon ausprobiert haben (Stand 2026-07-23)

| Ansatz | Ergebnis |
|--------|----------|
| Dual `BuildRenderList` | Hook OK (Mid-AOB). Baut Listen 2×, **zeichnet Welt nicht 2×**. |
| Mode 4 temporal (Frame L / Frame R + StretchRect + Submit) | **Parallaxe ja** (User verifiziert). Fusion nein. FPS ~halbiert (~90→~46). |
| IPD via Cam-Basis right/up | **Falsch** (R-Auge negatives X). |
| IPD = R_head × EyeToHead, dann OvrToGTA | L/R-Vorzeichen plausibel; Fusion trotzdem schlecht → Projection/FOV fehlt. |
| FOV → CCam+0x60 ~94° | Freeze — zurückgezogen. |

**Lehre:** Nächster Spike muss eine Funktion sein, die die **Szene wirklich zeichnet** (L4D2VR-`RenderView`-Äquivalent), zwei RTs **im selben Frame**, volle Eye-Rate. Alternating-Eye nicht als Zielqualität.

String in `GTAIV.exe` vorhanden: `SceneToGBuffer`, `CRenderPhaseDrawScene` — noch nicht als sicherer Dual-Draw-Hook genutzt.

---

## Meilensteine

### M1 — Stereo-Fundament (nächster Fokus)
1. Draw-Pass finden/hooken (nicht nur BuildRenderList).  
2. Pro Frame: Cam L → draw → RT_L; Cam R → draw → RT_R.  
3. Submit beide; Tick nur 1×.  
4. Per-eye Projection ohne Freeze-Pfad.

### M2 — Anzeige-Qualität
FOV/Aspect, Near-clip, HUD, SteamVR SS / Recommended RT size.

### M3 — Körper / UI
Head-Hide auf Script-Thread; optional TP-Körper.

### M4 — Komfort
Snap-turn, Vignette, Fahrzeug-Cam.

---

## Entscheidungsregeln

1. Lieber 2× Render + 1× Tick als Mono-Cinema.  
2. **Kein Alternating-Eye** als Ziel (BotW; bei uns FPS-Falle).  
3. Eine Verhaltensänderung pro riskantem Test.  
4. Keine Natives aus EndScene.  
5. Stock-DXVK behalten, solange Interop reicht.
