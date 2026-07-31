#pragma once

#include <d3d9.h>

// Minimal 2D overlay renderer for the in-game VR menu (docs/menu-integration/).
//
// Why hand-rolled: the ASI links d3d9.lib only (scripts/build-asi.ps1) and the
// legacy D3DX runtime is not installed on a modern machine, so ID3DXFont and
// ID3DXSprite are both out. Everything here is plain D3D9 fixed function:
// a GDI-baked glyph atlas in one managed A8R8G8B8 texture plus pre-transformed
// (XYZRHW) quads pushed through a single DrawPrimitiveUP per frame.
//
// All calls must come from the D3D9 render thread (the EndScene hook).
// Nothing here talks to OpenVR: the overlay is drawn into the backbuffer before
// the eye capture reads it, so the same draw serves flat and headset alike.

namespace asi {

// Prepares the atlas, saves device state and switches to the overlay pipeline.
// Returns false when the overlay cannot draw this frame; do not call any other
// draw function and do not call MenuDrawEnd() in that case.
bool MenuDrawBegin(IDirect3DDevice9* dev);

// Flushes the batched quads and restores the device state saved by Begin().
void MenuDrawEnd();

// Backbuffer size the current frame was laid out against.
int MenuDrawWidth();
int MenuDrawHeight();

// Glyph cell height in pixels; scales with the backbuffer so the menu stays
// readable both on a 1080p monitor and through a headset lens.
int MenuFontHeight();

// Width of `s` in pixels with the current atlas. Null-safe.
int MenuTextWidth(const char* s);

void MenuDrawRect(float x, float y, float w, float h, D3DCOLOR col);
void MenuDrawText(float x, float y, D3DCOLOR col, const char* s);
void MenuDrawTextf(float x, float y, D3DCOLOR col, const char* fmt, ...);

// Drops the atlas. Safe to call at any time; the next Begin() rebuilds it.
void MenuDrawShutdown();

}  // namespace asi
