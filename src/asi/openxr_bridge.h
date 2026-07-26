#pragma once

struct IDirect3DDevice9;

namespace asi
{
bool IsOpenXrBridgeRequested();
void PublishOpenXrFrame(IDirect3DDevice9* device);
void ShutdownOpenXrBridge();
}
