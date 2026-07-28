# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-28 — Mode **204** = FUSION MILESTONE

### Headset verdict
- **Fully fused**, camera perfect, no flicker
- Z-fighting in vehicles — ignore for now
- Size feels **big** vs Mode 203 (203 had better size, weaker fusion)

### Snapshots (keep both)
| Folder | Why |
|--------|-----|
| `prebuilt/mode204/` | **Fusion LKG** — stay here |
| `prebuilt/mode203/` | Scale felt better; weaker near fusion |

### Do NOT try more Mode198 hooks for fusion
Fusion is solved on `@0x4DE020`. More hooks risk breaking this.
Near-double was a bake-scope issue; 204 fixed it. **IPD is not the size fix either.**

### Size next (separate lever)
On **204**, use:
- **F11** — TrueWorldScale `100 → 125 → 150 → 175 → 200` (higher = **smaller** world)
- **F6** — stereoscale `100…130` (higher = stronger 3D / smaller feel)
- **F7** — fovadd visual presets (CropMax…Window0)

Live knobs were often: scale=100, stereoscale~115, ipd=2.

### Live
- Stereo: **`204`**
- Kill: keep **204**; restore from `prebuilt/mode204` if needed

### Friend pack / master
- Pack: `scripts\pack-mode204-zip.ps1` → `dist\gtaiv-dxvk-vr-mode204-friend.zip`
- Includes FusionFix update + `GTAIV.dxvk-cache` + stock DXVK 3.0.2 + Mode 204 ASI
- Master branch = Mode 204 fusion milestone
