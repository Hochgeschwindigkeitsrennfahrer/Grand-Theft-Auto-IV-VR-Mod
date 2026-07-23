#pragma once

struct IDirect3DDevice9;

namespace asi {

void TryMonoSubmit(IDirect3DDevice9* device);
void OpenVrShutdown();

// Call from ASI worker thread (not D3D thread).
bool InitOpenVrEarly();

}  // namespace asi
