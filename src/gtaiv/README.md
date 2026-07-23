# src/gtaiv/ + src/asi/

| Pfad | Rolle |
|------|--------|
| `glue.*` | Stubs (Config/OpenVR später) |
| `../asi/` | **Interop-Probe-ASI** → `out-asi/gtaiv_dxvk_vr.asi` |

Build: `.\scripts\build-asi.ps1`  
Deploy: `.\scripts\deploy-asi.ps1 -GameDir "...\GTAIV"`  

Strategie: `docs/VR_STRATEGY.md` (DXVK 3.0.2 + `ID3D9VkInterop*`).
