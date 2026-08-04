#include "menu_draw.h"

#include "log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace asi {
namespace {

constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr int kGlyphCount = kLastChar - kFirstChar + 1;  // 95 printable ASCII
constexpr int kCellsPerRow = 16;
constexpr int kGlyphRows = (kGlyphCount + kCellsPerRow - 1) / kCellsPerRow;  // 6

constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR color;
  float u, v;
};

struct Glyph {
  float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
  int advance = 0;
};

// --- atlas -----------------------------------------------------------------
IDirect3DDevice9* g_atlasDev = nullptr;  // device the atlas belongs to
IDirect3DTexture9* g_atlas = nullptr;
Glyph g_glyphs[kGlyphCount];
float g_whiteU = 0.f, g_whiteV = 0.f;  // opaque texel, used for solid quads
int g_cellW = 0, g_cellH = 0;
int g_fontPx = 0;
int g_atlasFails = 0;  // give up after a few tries instead of thrashing GDI

// --- per-frame -------------------------------------------------------------
IDirect3DDevice9* g_dev = nullptr;
int g_bbW = 0, g_bbH = 0;
bool g_inFrame = false;
std::vector<Vertex> g_verts;

// --- saved device state ----------------------------------------------------
// D3DSBT_ALL state blocks are the usual overlay trick, but CreateStateBlock is
// documented as illegal between BeginScene/EndScene and this renderer runs
// exactly there. Saving the states we touch by hand is both legal and cheaper.
const D3DRENDERSTATETYPE kSavedRs[] = {
    D3DRS_ZENABLE,        D3DRS_ZWRITEENABLE,
    D3DRS_FILLMODE,       D3DRS_SHADEMODE,
    D3DRS_CULLMODE,       D3DRS_LIGHTING,
    D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,      D3DRS_BLENDOP,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
    D3DRS_FOGENABLE,      D3DRS_STENCILENABLE,
    D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPING,
    D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
};
const D3DTEXTURESTAGESTATETYPE kSavedTss[] = {
    D3DTSS_COLOROP,   D3DTSS_COLORARG1,          D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,   D3DTSS_ALPHAARG1,          D3DTSS_ALPHAARG2,
    D3DTSS_RESULTARG, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_TEXCOORDINDEX,
};
const D3DSAMPLERSTATETYPE kSavedSs[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
    D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_SRGBTEXTURE,
};
constexpr int kRsN = static_cast<int>(sizeof(kSavedRs) / sizeof(kSavedRs[0]));
constexpr int kTssN = static_cast<int>(sizeof(kSavedTss) / sizeof(kSavedTss[0]));
constexpr int kSsN = static_cast<int>(sizeof(kSavedSs) / sizeof(kSavedSs[0]));

DWORD g_oldRs[kRsN]{};
DWORD g_oldTss0[kTssN]{};
DWORD g_oldTss1Color = 0, g_oldTss1Alpha = 0;
DWORD g_oldSs[kSsN]{};
DWORD g_oldFvf = 0;
IDirect3DVertexDeclaration9* g_oldDecl = nullptr;
IDirect3DVertexShader9* g_oldVs = nullptr;
IDirect3DPixelShader9* g_oldPs = nullptr;
IDirect3DBaseTexture9* g_oldTex0 = nullptr;
IDirect3DVertexBuffer9* g_oldStream0 = nullptr;
UINT g_oldStream0Offset = 0, g_oldStream0Stride = 0;
IDirect3DIndexBuffer9* g_oldIndices = nullptr;
IDirect3DSurface9* g_oldRt = nullptr;
IDirect3DSurface9* g_oldDs = nullptr;
IDirect3DSurface9* g_frameBb = nullptr;
D3DVIEWPORT9 g_oldVp{};
bool g_rtSwapped = false;

int NextPow2(int v) {
  int p = 1;
  while (p < v)
    p <<= 1;
  return p;
}

void ReleaseAtlas() {
  if (g_atlas) {
    g_atlas->Release();
    g_atlas = nullptr;
  }
  g_atlasDev = nullptr;
  g_cellW = g_cellH = g_fontPx = 0;
}

// Bakes printable ASCII into one A8R8G8B8 texture with GDI. White text on black
// becomes white texels with the greyscale as alpha, so MODULATE with the vertex
// colour gives coloured, antialiased glyphs.
bool BuildAtlas(IDirect3DDevice9* dev, int fontPx) {
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  if (screen)
    ReleaseDC(nullptr, screen);
  if (!dc)
    return false;

  HFONT font = nullptr;
  static const char* kFaces[] = {"Consolas", "Lucida Console", "Courier New"};
  for (int i = 0; i < 3 && !font; ++i)
    font = CreateFontA(fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, kFaces[i]);
  if (!font)
    font = CreateFontA(fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, nullptr);
  if (!font) {
    DeleteDC(dc);
    return false;
  }
  HGDIOBJ oldFont = SelectObject(dc, font);

  TEXTMETRICA tm{};
  if (!GetTextMetricsA(dc, &tm)) {
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return false;
  }
  const int cellW = tm.tmMaxCharWidth + 2;
  const int cellH = tm.tmHeight + 2;
  // One spare row below the glyphs holds the opaque texel for solid quads.
  const int texW = NextPow2(cellW * kCellsPerRow);
  const int texH = NextPow2(cellH * (kGlyphRows + 1));

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = texW;
  bmi.bmiHeader.biHeight = -texH;  // negative = top-down, matches D3D row order
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    if (bmp)
      DeleteObject(bmp);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return false;
  }
  HGDIOBJ oldBmp = SelectObject(dc, bmp);

  RECT all{0, 0, texW, texH};
  FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));

  Glyph glyphs[kGlyphCount];
  for (int i = 0; i < kGlyphCount; ++i) {
    const int ch = kFirstChar + i;
    const int cx = (i % kCellsPerRow) * cellW + 1;
    const int cy = (i / kCellsPerRow) * cellH + 1;
    const char s[2] = {static_cast<char>(ch), 0};
    TextOutA(dc, cx, cy, s, 1);

    int adv = 0;
    if (!GetCharWidth32A(dc, ch, ch, &adv) || adv <= 0)
      adv = tm.tmAveCharWidth;
    glyphs[i].advance = adv;
    glyphs[i].u0 = static_cast<float>(cx - 1) / texW;
    glyphs[i].v0 = static_cast<float>(cy - 1) / texH;
    glyphs[i].u1 = static_cast<float>(cx - 1 + cellW) / texW;
    glyphs[i].v1 = static_cast<float>(cy - 1 + cellH) / texH;
  }

  // Opaque 4x4 block in the spare row.
  const int wx = 2, wy = kGlyphRows * cellH + 2;
  RECT white{wx, wy, wx + 4, wy + 4};
  FillRect(dc, &white, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  GdiFlush();

  IDirect3DTexture9* tex = nullptr;
  HRESULT hr = dev->CreateTexture(static_cast<UINT>(texW), static_cast<UINT>(texH), 1, 0,
                                  D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
  if (SUCCEEDED(hr) && tex) {
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
      const auto* src = static_cast<const uint32_t*>(bits);
      for (int y = 0; y < texH; ++y) {
        auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
        const uint32_t* row = src + static_cast<size_t>(y) * texW;
        for (int x = 0; x < texW; ++x) {
          const uint32_t p = row[x];
          const uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
          uint32_t a = r > g ? r : g;
          if (b > a)
            a = b;
          dst[x] = (a << 24) | 0x00FFFFFFu;
        }
      }
      tex->UnlockRect(0);
    } else {
      tex->Release();
      tex = nullptr;
    }
  } else {
    tex = nullptr;
  }

  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  SelectObject(dc, oldFont);
  DeleteObject(font);
  DeleteDC(dc);

  if (!tex)
    return false;

  ReleaseAtlas();
  g_atlas = tex;
  g_atlasDev = dev;
  g_cellW = cellW;
  g_cellH = cellH;
  g_fontPx = fontPx;
  g_whiteU = (wx + 2.f) / texW;
  g_whiteV = (wy + 2.f) / texH;
  memcpy(g_glyphs, glyphs, sizeof(g_glyphs));
  Log("MenuDraw: font atlas %dx%d cell=%dx%d fontPx=%d", texW, texH, cellW, cellH, fontPx);
  return true;
}

void PushQuad(float x, float y, float w, float h, D3DCOLOR col, float u0, float v0, float u1,
              float v1) {
  // -0.5 texel/pixel offset: D3D9 rasterises pre-transformed vertices half a
  // pixel off without it, which smears the glyph edges.
  const float x0 = x - 0.5f, y0 = y - 0.5f;
  const float x1 = x + w - 0.5f, y1 = y + h - 0.5f;
  const Vertex quad[6] = {
      {x0, y0, 0.f, 1.f, col, u0, v0}, {x1, y0, 0.f, 1.f, col, u1, v0},
      {x1, y1, 0.f, 1.f, col, u1, v1}, {x0, y0, 0.f, 1.f, col, u0, v0},
      {x1, y1, 0.f, 1.f, col, u1, v1}, {x0, y1, 0.f, 1.f, col, u0, v1},
  };
  g_verts.insert(g_verts.end(), quad, quad + 6);
}

void SaveState(IDirect3DDevice9* dev) {
  for (int i = 0; i < kRsN; ++i)
    dev->GetRenderState(kSavedRs[i], &g_oldRs[i]);
  for (int i = 0; i < kTssN; ++i)
    dev->GetTextureStageState(0, kSavedTss[i], &g_oldTss0[i]);
  dev->GetTextureStageState(1, D3DTSS_COLOROP, &g_oldTss1Color);
  dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &g_oldTss1Alpha);
  for (int i = 0; i < kSsN; ++i)
    dev->GetSamplerState(0, kSavedSs[i], &g_oldSs[i]);
  dev->GetFVF(&g_oldFvf);
  dev->GetVertexDeclaration(&g_oldDecl);
  dev->GetVertexShader(&g_oldVs);
  dev->GetPixelShader(&g_oldPs);
  dev->GetTexture(0, &g_oldTex0);
  dev->GetStreamSource(0, &g_oldStream0, &g_oldStream0Offset, &g_oldStream0Stride);
  dev->GetIndices(&g_oldIndices);
}

void RestoreState(IDirect3DDevice9* dev) {
  // DrawPrimitiveUP clears stream 0 and the index buffer, so both are restored
  // unconditionally — including back to null.
  dev->SetStreamSource(0, g_oldStream0, g_oldStream0Offset, g_oldStream0Stride);
  dev->SetIndices(g_oldIndices);
  dev->SetTexture(0, g_oldTex0);
  dev->SetPixelShader(g_oldPs);
  dev->SetVertexShader(g_oldVs);
  // SetFVF and SetVertexDeclaration overwrite each other; restoring the
  // declaration covers both cases (an FVF-set device reports an implicit one).
  if (g_oldDecl)
    dev->SetVertexDeclaration(g_oldDecl);
  else
    dev->SetFVF(g_oldFvf);
  for (int i = 0; i < kSsN; ++i)
    dev->SetSamplerState(0, kSavedSs[i], g_oldSs[i]);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, g_oldTss1Alpha);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, g_oldTss1Color);
  for (int i = 0; i < kTssN; ++i)
    dev->SetTextureStageState(0, kSavedTss[i], g_oldTss0[i]);
  for (int i = 0; i < kRsN; ++i)
    dev->SetRenderState(kSavedRs[i], g_oldRs[i]);

  if (g_oldStream0) {
    g_oldStream0->Release();
    g_oldStream0 = nullptr;
  }
  if (g_oldIndices) {
    g_oldIndices->Release();
    g_oldIndices = nullptr;
  }
  if (g_oldTex0) {
    g_oldTex0->Release();
    g_oldTex0 = nullptr;
  }
  if (g_oldPs) {
    g_oldPs->Release();
    g_oldPs = nullptr;
  }
  if (g_oldVs) {
    g_oldVs->Release();
    g_oldVs = nullptr;
  }
  if (g_oldDecl) {
    g_oldDecl->Release();
    g_oldDecl = nullptr;
  }
}

void ApplyOverlayState(IDirect3DDevice9* dev) {
  dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
  dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_CLIPPING, TRUE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                      D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                          D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
  dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(kFvf);
  dev->SetTexture(0, g_atlas);
}

}  // namespace

bool MenuDrawBegin(IDirect3DDevice9* dev) {
  if (!dev || g_inFrame)
    return false;

  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
    return false;
  D3DSURFACE_DESC desc{};
  if (FAILED(bb->GetDesc(&desc))) {
    bb->Release();
    return false;
  }
  g_bbW = static_cast<int>(desc.Width);
  g_bbH = static_cast<int>(desc.Height);

  int fontPx = g_bbH / 40;
  if (fontPx < 14)
    fontPx = 14;
  if (fontPx > 40)
    fontPx = 40;

  if (g_atlas && (g_atlasDev != dev || g_fontPx != fontPx))
    ReleaseAtlas();
  if (!g_atlas) {
    if (g_atlasFails >= 3) {
      bb->Release();
      return false;
    }
    if (!BuildAtlas(dev, fontPx)) {
      ++g_atlasFails;
      Log("MenuDraw: font atlas build FAILED (%d/3) — overlay off", g_atlasFails);
      bb->Release();
      return false;
    }
    g_atlasFails = 0;
  }

  g_dev = dev;
  g_frameBb = bb;  // reference handed to MenuDrawEnd
  g_verts.clear();

  dev->GetRenderTarget(0, &g_oldRt);
  if (FAILED(dev->GetDepthStencilSurface(&g_oldDs)))
    g_oldDs = nullptr;
  dev->GetViewport(&g_oldVp);

  // Draw into the backbuffer even when the mod has an eye canvas bound, so the
  // overlay lands in the image the eye capture copies from — and on the monitor.
  g_rtSwapped = (g_oldRt != bb);
  if (g_rtSwapped && FAILED(dev->SetRenderTarget(0, bb))) {
    if (g_oldRt) {
      g_oldRt->Release();
      g_oldRt = nullptr;
    }
    if (g_oldDs) {
      g_oldDs->Release();
      g_oldDs = nullptr;
    }
    bb->Release();
    g_frameBb = nullptr;
    g_rtSwapped = false;
    return false;
  }
  dev->SetDepthStencilSurface(nullptr);

  D3DVIEWPORT9 vp{};
  vp.X = 0;
  vp.Y = 0;
  vp.Width = static_cast<DWORD>(g_bbW);
  vp.Height = static_cast<DWORD>(g_bbH);
  vp.MinZ = 0.f;
  vp.MaxZ = 1.f;
  dev->SetViewport(&vp);

  SaveState(dev);
  ApplyOverlayState(dev);
  g_inFrame = true;
  return true;
}

void MenuDrawEnd() {
  if (!g_inFrame || !g_dev)
    return;
  IDirect3DDevice9* dev = g_dev;

  if (!g_verts.empty()) {
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(g_verts.size() / 3),
                         g_verts.data(), sizeof(Vertex));
    g_verts.clear();
  }

  RestoreState(dev);

  dev->SetViewport(&g_oldVp);
  dev->SetDepthStencilSurface(g_oldDs);
  if (g_rtSwapped)
    dev->SetRenderTarget(0, g_oldRt);
  if (g_oldRt) {
    g_oldRt->Release();
    g_oldRt = nullptr;
  }
  if (g_oldDs) {
    g_oldDs->Release();
    g_oldDs = nullptr;
  }
  if (g_frameBb) {
    g_frameBb->Release();
    g_frameBb = nullptr;
  }
  g_rtSwapped = false;
  g_inFrame = false;
  g_dev = nullptr;
}

int MenuDrawWidth() {
  return g_bbW;
}

int MenuDrawHeight() {
  return g_bbH;
}

int MenuFontHeight() {
  return g_cellH;
}

int MenuTextWidth(const char* s) {
  if (!s || !g_cellW)
    return 0;
  int w = 0;
  for (; *s; ++s) {
    const int c = static_cast<unsigned char>(*s);
    if (c < kFirstChar || c > kLastChar)
      continue;
    w += g_glyphs[c - kFirstChar].advance;
  }
  return w;
}

void MenuDrawRect(float x, float y, float w, float h, D3DCOLOR col) {
  if (!g_inFrame || w <= 0.f || h <= 0.f)
    return;
  PushQuad(x, y, w, h, col, g_whiteU, g_whiteV, g_whiteU, g_whiteV);
}

void MenuDrawText(float x, float y, D3DCOLOR col, const char* s) {
  if (!g_inFrame || !s)
    return;
  float pen = x;
  for (; *s; ++s) {
    const int c = static_cast<unsigned char>(*s);
    if (c < kFirstChar || c > kLastChar) {
      if (c == '\t')
        pen += 4.f * g_glyphs['n' - kFirstChar].advance;
      continue;
    }
    const Glyph& gl = g_glyphs[c - kFirstChar];
    if (c != ' ')
      PushQuad(pen, y, static_cast<float>(g_cellW), static_cast<float>(g_cellH), col, gl.u0, gl.v0,
               gl.u1, gl.v1);
    pen += static_cast<float>(gl.advance);
  }
}

void MenuDrawTextf(float x, float y, D3DCOLOR col, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  MenuDrawText(x, y, col, buf);
}

void MenuDrawShutdown() {
  ReleaseAtlas();
  g_verts.clear();
  g_verts.shrink_to_fit();
  g_atlasFails = 0;
}

}  // namespace asi
