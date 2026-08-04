# CURRENT-STATE - gtaiv-dxvk-vr

## 2026-08-04 — F3 render modes: 243 / 271 / 920 / 937

**Default / kill:** `stereo=243`  
**F3 LIVE profiles:** `243` → `271` → `920` → `937`  
**Aiming:** master’s existing look-move / HMD look (unchanged).  
**909:** retired — no longer in menu or active line.

### What shipped
- Restored F3 overlay glue (`menu_bridge` / `menu_draw` / `vr_menu.h`); kept master’s `vr_menu.cpp`
- **271 AER** — one DrawWalk/frame, alternating eye, stale-eye rect reproject (from AER worktree; 272/273 not ported)
- **920 DirectBounds** — 243 seam + 1:1 BB + float `VRTextureBounds`
- **937 ParentExact** — ParentWalk AER (helper 932) + `frame%2` parity + OffAxis proj
- `config/gtaiv_dxvk_vr.menumap` defaults to the four modes above

### Test
1. Build: `.\scripts\build-asi.ps1` → deploy `out-asi\gtaiv_dxvk_vr.asi` (+ `openvr_api.dll`)
2. Copy `config\gtaiv_dxvk_vr.menumap` next to the ASI (or rely on built-in defaults)
3. Start with `gtaiv_dxvk_vr.stereo` = **243**
4. F3 → **VR render mode**: cycle **243 / 271 / 920 / 937**
5. Confirm look/aim still matches master 243 behavior on all four
6. Log lines: `Mode271:…` / `Mode920:…` / `Mode937:…`

### Kill
Return to `stereo=243`.
