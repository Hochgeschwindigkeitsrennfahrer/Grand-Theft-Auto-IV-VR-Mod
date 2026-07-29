# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-29 (home) — Mode **243** CHECKPOINT (push / backup)

### Restart rule (locked)
Always start/restart via **Quick Restart** = `restart-gtaiv.ps1 -NoPause -DirectExe`.

### Checkpoint
- **`prebuilt/mode243/`** — ASI `20260729-210905` + `stereo=243` + README  
- Mode203 `@0x4D8BF0` + cam-right IPD flip + **shared late-latch** `Submit_TextureWithPose`  
- Also keep: `prebuilt/mode241/` (stereo without late-latch)

### Headset
- Stereo like 241; UI better (still / moving); blur remains with Motion Smoothing off  
- App FPS ~**89** avg (locked ~90 Hz) — not a low-FPS issue

### Next — Mode **244** (blur)
Stamp **dual RENDER pose** (pose used while drawing L/R) on both eyes at Submit,  
not a fresh “now” sample. Late-now can lie to the compositor → soft/blurry warp.

### Kill
- From 243: `241` / `203`  
- From 244: `243` / `241`
