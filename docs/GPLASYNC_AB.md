# gtaiv-dxvk-vr — DXVK / gplasync A/B precautions
#
# Stock (proven VR): DXVK 3.0.2 d3d9.dll with ID3D9VkInterop
#   backups/dxvk-stock-302/d3d9.dll.stock-302
#   also next to GTAIV.exe as d3d9.dll.stock-302 after first backup
#
# Newest gplasync with interop (preferred A/B):
#   Ph42oN weekly artifact → v3.0-gplasync x32
#   thirdparty/dxvk-gplasync/artifacts-main/.../x32/d3d9.dll
#
# Packaged fallback (older):
#   v2.7.1-1-gplasync x32 (also has ID3D9VkInterop)
#
# Install:
#   powershell -ExecutionPolicy Bypass -File scripts\install-gplasync-ab.ps1
#
# Rollback to stock 3.0.2:
#   powershell -ExecutionPolicy Bypass -File scripts\restore-stock-dxvk.ps1
#
# Shader cache (FusionFix 5.0 multi-vendor):
#   inspo/dxvk cache/GTAIV.dxvk-cache → copied next to GTAIV.exe
#   Version-sensitive; may be ignored on 3.0.x. Helps stutter, not flicker.
#
# Async does NOT fix stereo L/R flash. Use Mode 169 for that.
