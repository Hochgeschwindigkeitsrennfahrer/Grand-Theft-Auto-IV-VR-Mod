# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-29 — Mode **216** walk-bob damp + forward eye

### Headset
1. Stairs stick: **yes**
2. Crouch/stand stick: **yes**
3. Walk: **everything jumping** (bob still leaked)
4. Model spazz: still — possibly hide thrash on 204 dual path
5. Ask: camera forward between eyes again (vehicles weird with back offset)

### This build
- Bob zone widened to **≤8 cm** with much lower alpha (~0.03) — walk/run damp
- Stairs/crouch still fast catch-up (≥20–22 cm)
- **Smooth root XY** too (live root.xy was walk-sway)
- Eye: **+6 cm up, +3 cm forward** (no back)
- Ped hide **once per bone sample** only on 216 (not every CopyMat) — less spazz
- Kill: **`204`**

### Live
- Stereo: **`216`**

### Test
1. Walk / run — world still jumping?
2. Stairs / crouch still OK?
3. Model spazz better?
4. In car — between eyes OK?
