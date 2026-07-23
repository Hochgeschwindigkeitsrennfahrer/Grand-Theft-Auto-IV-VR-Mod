#pragma once

// GTA IV CE glue — OpenVR Submit via Stock-DXVK ID3D9VkInterop* (docs/VR_STRATEGY.md).
// Not IronWolf IDirect3DVR9 for v0 on this machine.

namespace gtaiv {

void LogInit(const char* path);
void LogWrite(const char* fmt, ...);

struct Config {
  bool openvr_enabled = true;
  bool verbose = true;
  bool mono_submit = true;
};

bool LoadConfig(const char* path, Config* out);

bool OpenVrInit();
void OpenVrShutdown();

// After ASI hooks Present: QI ID3D9VkInteropDevice, fill VRVulkanTextureData, Submit.
// Placeholder until ASI is linked.
bool TryMonoSubmitPlaceholder(/* IDirect3DDevice9* device */);

}  // namespace gtaiv
