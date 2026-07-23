# src/gtaiv/

GTA-spezifisches Glue (Log, Config, später OpenVR-Submit).

| Datei | Status |
|-------|--------|
| `glue.h` / `glue.cpp` | Stubs — loggen, noch **kein** Link in eine DLL |
| Build | Erst nach flacher x86-`d3d9.dll` aus `dxvk/` |

Meilenstein: Stubs an IronWolf-Mailbox + `IVRCompositor::Submit` anbinden (`docs/IRONWOLF_API.md`, `docs/L4D2VR_MAP.md` → `vr.cpp`).
