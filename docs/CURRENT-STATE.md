# CURRENT-STATE — gtaiv-dxvk-vr

## 2026-07-28 — Mode **169** + gplasync A/B (ready to headset-test)

### Live setup (after AFK prep)
- **stereo=`169`** / buildid `20260728-mode169+gplasync-ab`
- **d3d9.dll** = Ph42oN weekly **`v3.0-gplasync`** x32 (interop + `enableAsync`)
- Stock **3.0.2** saved as `d3d9.dll.stock-302` + `backups/dxvk-stock-302/`
- `dxvk.conf`: `dxvk.enableAsync = true`
- FusionFix cache: `GTAIV.dxvk-cache` next to EXE
- GitHub backup: branch `backup/pre-gplasync-20260728` (+ follow-up Mode169 commit)

### Mode 169 = safe flicker (from 168 logs)
| Keep from 168 | Drop (caused DEVICE_LOST / flash) |
|---------------|-------------------------------------|
| atomic L+R pair | everyN=1 (back to dualn=2) |
| no tiny-RT skip-gate | FlushRenderingCommands per eye |
| Mode167 car + HUD | hitch → LIVELOOK mono (hitch → HOLD last stereo) |

Kill: **`167`** / `162`. Do **not** re-enable 168.

### Rollback (one click)
```text
powershell -ExecutionPolicy Bypass -File scripts\restore-stock-dxvk.ps1
```
Re-install gplasync later:
```text
powershell -ExecutionPolicy Bypass -File scripts\install-gplasync-ab.ps1
```
Docs: `docs/GPLASYNC_AB.md`

### Test order when back
1. SteamVR on, headset awake
2. Launch GTA IV — flat window ~1 min (watch for crash / black)
3. Enter world, look for `Mode169` / `SAFE` in `gtaiv_dxvk_vr.log`
4. Check `GTAIV_d3d9.log` starts with gplasync / no DEVICE_LOST
5. Drive near water — compare flicker vs 167
6. If freeze/black → restore stock, set stereo=`167`, report log lines

---

## 2026-07-28 afternoon — Mode **168** freeze (DEVICE_LOST)

### Log verdict
- Session: **stereo=168** / `20260728-mode168-flicker`
- ASI stayed “healthy” to the end (`errL=0 errR=0`, dual `haveL=1 haveR=1`, ~14 min, `present≈10138`)
- **`GTAIV_d3d9.log`:** hundreds of `VK_ERROR_DEVICE_LOST` → GPU/driver reset = hard freeze
- Cause: Mode168 **everyN=1** (DrawScene×2 every frame) + **FlushRenderingCommands per eye** overloaded the GPU
- LIVELOOK hit ×2 near end = hitch → mono BB flash (extra flicker symptom)

### Immediate kill
Set `gtaiv_dxvk_vr.stereo` → **`167`** (or `162`). Do **not** keep testing 168 until Flush/everyN redesigned.

### GPLAsync / Nexus / shader cache
See investigation notes below (we do **not** ship gplasync today).

---

## Investigation — GPLAsync + FusionFix shader cache

### Do we have GPLAsync?
**No.** Live `d3d9.dll` is stock DXVK with `ID3D9VkInterop` (no `enableAsync` / `gplasync` strings). Project rule: async = stutter tool, not VR image path (`docs/CONSTRAINTS.md` §7).

### Nexus mod (DXVK 2.6.2 + GPLAsync)
- Older than our **3.0.2** base
- Helps **shader stutter**, not stereo flash / dual-eye capture bugs
- Swapping to 2.6.2 risks flat/VR regressions we already escaped

### Ph42oN [dxvk-gplasync](https://gitlab.com/Ph42oN/dxvk-gplasync)
- Packaged releases through **v2.7.1-1** (includes x32)
- Master **patch** updated toward DXVK 3.0 — **no ready 3.0.2 binary drop** in releases folder
- Useful later only if we **build** patched 3.0.2 ourselves (Win32) and A/B flat+VR

### Digger1955 dxvk-gplall
- Caps ~**2.6.8** — reject for our 3.0.2 interop stack

### `inspo/dxvk cache/GTAIV.dxvk-cache` (~100 KB)
- FusionFix 5.0 multi-vendor prewarm — **worth a careful A/B for stutter**
- Cache is **DXVK-version sensitive**; may be ignored or partial on 3.0.2
- Does **not** fix Mode168 DEVICE_LOST or dual-eye flicker
- Install: copy next to `GTAIV.exe` / `d3d9.dll` (backup first); first launch may still compile

### What actually flickered / froze
| Symptom | Source |
|---------|--------|
| Hard freeze | `VK_ERROR_DEVICE_LOST` under Mode168 load |
| Hardcore flicker | Dual-eye capture / LIVELOOK mono swaps (ASI), not missing async |
| Shader hitch (separate) | Cache / async can help **that** class only |

---
