#pragma once

// GTA IV CE glue stubs — not linked until DXVK + OpenVR deps exist.
// Goal: mono OpenVR Submit via IronWolf IDirect3DVR9 (see docs/IRONWOLF_API.md).

namespace gtaiv {

void LogInit(const char* path);
void LogWrite(const char* fmt, ...);

struct Config {
  bool openvr_enabled = true;
  bool verbose = true;
  bool mono_submit = true;
};

bool LoadConfig(const char* path, Config* out);

// Returns false until OpenVR + VR-DXVK mailbox are wired.
bool OpenVrInit();
void OpenVrShutdown();

// Call once per frame when we have IDirect3DDevice9* + eye/backbuffer surface.
// Implementation: TODO after submodule + flat d3d9.dll boot.
bool TryMonoSubmitPlaceholder(/* IDirect3DDevice9* device */);

}  // namespace gtaiv
