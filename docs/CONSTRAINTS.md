# Harte Regeln (Constraints)

Nicht nochmal auf die harte Tour lernen.

1. **GTA IV CE = Win32 (32-bit).** Alles in den Prozess: **x86**.  
2. **Compositor = OpenVR über SteamVR.** Wie L4D2VR/HL2VR. OpenXR ist für dieses Projekt erst später optional.  
3. **Weder OpenVR noch OpenXR nehmen rohe D3D9-Texturen.** Submit braucht Vulkan- (oder D3D11-)Bilder aus dem DXVK-Pfad.  
4. **Stock-DXVK / Steam-„Vulkan-Guide“ = nur FPS.** VR braucht einen Fork mit Mailbox + unser Glue.  
5. **Fremde `d3d9.dll` (z. B. L4D2VR-Release) in GTA legen ≠ VR.** Falsches Spiel, falsche Hooks.  
6. **FusionFix** = CE-Basis / ASI-Loader — kein VR. Vulkan-Toggle von FusionFix aus, wenn unsere DLL aktiv ist.  
7. **Async / gplasync** = Stutter, nicht „Bild in die Brille“.  
8. **First-Person-Kamera** später: CE-Offsets nicht 1:1 von alten IV-SDK-Versionen; AOB/Natives.  
9. **Reverb G2 + OpenVR** ⇒ SteamVR muss die Brille sehen.  
10. **Eine Verhaltensänderung pro Test-Build**; Logs pasten.  
11. Entwicklung **offline / Singleplayer**.  
12. Firmen-PC: riskante Netz-Tools meiden; Submodule/Push zu Hause.
