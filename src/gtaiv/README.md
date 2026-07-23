# src/gtaiv/ + src/asi/

| Path | Role |
|------|------|
| `glue.*` | Stubs (config/OpenVR later) |
| `../asi/` | **Interop probe ASI** → `out-asi/gtaiv_dxvk_vr.asi` |

Build: `.\scripts\build-asi.ps1`  
Deploy: `.\scripts\deploy-asi.ps1 -GameDir "...\GTAIV"`  

Strategy: `docs/VR_STRATEGY.md` (DXVK 3.0.2 + `ID3D9VkInterop*`).
