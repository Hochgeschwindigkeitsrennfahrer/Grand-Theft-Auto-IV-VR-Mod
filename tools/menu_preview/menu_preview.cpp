// Offline render test for the in-game settings overlay.
//
// Builds a plain D3D9 device on a hidden window, draws the overlay exactly the
// way the EndScene hook does, reads the backbuffer back and writes a 24-bit BMP.
// The point is to prove the atlas, the layout and the state save/restore work
// without launching GTA IV — useful in general, and the only way to see the
// panel at all when no headset is around.
//
// It links the real menu_draw.cpp / vr_menu.cpp / menu_bridge.cpp. Everything
// those pull in from stereo_config lives in menu_preview_stubs.cpp, so the
// preview exercises the menu code and nothing else.
//
//   scripts\build-menu-preview.ps1
//   out-preview\menu_preview.exe [width] [height] [out.bmp]

#include "../../src/asi/vr_menu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool WriteBmp24(const char* path, const uint8_t* bgra, int w, int h, int pitch) {
  const int rowBytes = w * 3;
  const int padded = (rowBytes + 3) & ~3;
  const int imageBytes = padded * h;

  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + imageBytes;
  ih.biSize = sizeof(ih);
  ih.biWidth = w;
  ih.biHeight = h;  // positive = bottom-up, so rows are written in reverse
  ih.biPlanes = 1;
  ih.biBitCount = 24;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = imageBytes;

  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return false;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&ih, sizeof(ih), 1, f);

  std::vector<uint8_t> row(padded, 0);
  for (int y = h - 1; y >= 0; --y) {
    const uint8_t* src = bgra + static_cast<size_t>(y) * pitch;
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = src[x * 4 + 0];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 2];
    }
    fwrite(row.data(), 1, padded, f);
  }
  fclose(f);
  return true;
}

// A recognisable backdrop: if the overlay ever fails to restore device state,
// or blends wrong, the gradient makes it obvious.
void DrawBackdrop(IDirect3DDevice9* dev, int w, int top, int bottom) {
  struct V {
    float x, y, z, rhw;
    D3DCOLOR c;
  };
  const float t = static_cast<float>(top), b = static_cast<float>(bottom);
  const V quad[4] = {
      {0.f, t, 0.f, 1.f, D3DCOLOR_XRGB(24, 60, 96)},
      {static_cast<float>(w), t, 0.f, 1.f, D3DCOLOR_XRGB(96, 40, 24)},
      {0.f, b, 0.f, 1.f, D3DCOLOR_XRGB(20, 96, 60)},
      {static_cast<float>(w), b, 0.f, 1.f, D3DCOLOR_XRGB(140, 130, 40)},
  };
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  dev->SetTexture(0, nullptr);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
}

}  // namespace

int main(int argc, char** argv) {
  const int w = (argc > 1) ? atoi(argv[1]) : 1920;
  const int h = (argc > 2) ? atoi(argv[2]) : 1080;
  const char* out = (argc > 3) ? argv[3] : "menu_preview.bmp";

  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "GtaIvVrMenuPreview";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA(wc.lpszClassName, "menu preview", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    printf("FAIL: CreateWindow\n");
    return 1;
  }

  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("FAIL: Direct3DCreate9\n");
    return 1;
  }

  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = static_cast<UINT>(w);
  pp.BackBufferHeight = static_cast<UINT>(h);
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.hDeviceWindow = hwnd;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    printf("FAIL: CreateDevice hr=0x%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }

  asi::VrMenuSetOpen(true);

  dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);
  dev->BeginScene();
  DrawBackdrop(dev, w, 0, h);

  // Paint the bottom strip magenta BEFORE the overlay, then draw over it with
  // the caller's own pipeline AFTER the overlay. Any magenta left in the output
  // means the overlay leaked its texture / FVF / blend state and the caller's
  // draw did not come out as a plain untextured gradient.
  const int stripH = 40;
  D3DRECT strip{0, h - stripH, w, h};
  dev->Clear(1, &strip, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.f, 0);

  // Same call site as hooks.cpp HookEndScene.
  asi::VrMenuOnEndScene(dev);

  DrawBackdrop(dev, w, h - stripH, h);
  dev->EndScene();

  IDirect3DSurface9* bb = nullptr;
  IDirect3DSurface9* sys = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) ||
      FAILED(dev->CreateOffscreenPlainSurface(static_cast<UINT>(w), static_cast<UINT>(h),
                                              pp.BackBufferFormat, D3DPOOL_SYSTEMMEM, &sys,
                                              nullptr)) ||
      FAILED(dev->GetRenderTargetData(bb, sys))) {
    printf("FAIL: readback\n");
    return 1;
  }

  D3DLOCKED_RECT lr{};
  if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    printf("FAIL: LockRect\n");
    return 1;
  }
  const bool ok = WriteBmp24(out, static_cast<const uint8_t*>(lr.pBits), w, h, lr.Pitch);
  sys->UnlockRect();
  printf(ok ? "OK: wrote %s (%dx%d)\n" : "FAIL: write %s (%dx%d)\n", out, w, h);

  sys->Release();
  bb->Release();
  dev->Release();
  d3d->Release();
  DestroyWindow(hwnd);
  return ok ? 0 : 1;
}
