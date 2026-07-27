#pragma once

struct IDirect3DDevice9;

namespace asi
{
enum class VrBackend
{
    Off,
    OpenVr,
    OpenXr,
};

// Reads gtaiv_dxvk_vr.backend once. Missing or unrecognized values are Off;
// OpenVR must be requested explicitly so a flat recovery never starts SteamVR.
VrBackend GetVrBackend();
bool IsOpenXrBridgeRequested();
bool IsOpenVrBridgeRequested();
void PublishOpenXrFrame(IDirect3DDevice9* device);
void ShutdownOpenXrBridge();
}
