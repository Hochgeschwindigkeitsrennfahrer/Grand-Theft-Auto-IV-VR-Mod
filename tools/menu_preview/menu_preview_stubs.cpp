// Stand-ins for everything vr_menu.cpp / menu_bridge.cpp reach for outside the
// menu code, so the offline preview links against the real menu sources without
// dragging in stereo_render.cpp and the whole VR pipeline.
//
// These only have to be plausible: the preview verifies the atlas, the layout
// and the device state save/restore. The real setters are exercised in-game.

#include "../../src/asi/log.h"
#include "../../src/asi/stereo_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace asi {
namespace {

int g_mode = 243;
int g_sepCm = 6;
int g_stereoScale = 115;
int g_worldScale = 100;
int g_wsPreset = 5;
int g_fovAdd = 12;
uint32_t g_vres = 1536;
int g_eyeFwdCm = 42;
int g_fpFov = 90;

const char* kWsPresets[] = {"CropMax", "Crop",  "TallFill", "Mild18", "Soft16",
                            "Open12",  "Room8", "MatchH6",  "Air4",   "Window0"};
constexpr int kWsPresetN = static_cast<int>(sizeof(kWsPresets) / sizeof(kWsPresets[0]));

}  // namespace

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

void LogInit() {}
void ArchiveSessionLog(const char*) {}

StereoMode GetStereoMode() {
  return static_cast<StereoMode>(g_mode);
}
void ReloadStereoMode() {}
void WriteStereoModeFile(int mode) {
  g_mode = mode;
}

void MenuSetSepCm(int cm) {
  g_sepCm = cm;
}
int MenuGetSepCm() {
  return g_sepCm;
}
void MenuSetStereoScalePercent(int pct) {
  g_stereoScale = pct;
}
int MenuGetStereoScalePercent() {
  return g_stereoScale;
}
void MenuSetWorldScalePercent(int pct) {
  g_worldScale = pct;
}
int MenuGetWorldScalePercent() {
  return g_worldScale;
}
void MenuSetWorldScalePreset(int idx) {
  g_wsPreset = idx;
}
int MenuGetWorldScalePreset() {
  return g_wsPreset;
}
int MenuWorldScalePresetCount() {
  return kWsPresetN;
}
const char* MenuWorldScalePresetName(int idx) {
  return (idx >= 0 && idx < kWsPresetN) ? kWsPresets[idx] : "?";
}
void MenuSetFovAddDegrees(int deg) {
  g_fovAdd = deg;
}
int MenuGetFovAddDegrees() {
  return g_fovAdd;
}
int MenuGetEyeForwardCm() {
  return g_eyeFwdCm;
}

uint32_t GetCanvasMaxDim() {
  return g_vres;
}
void SetCanvasMaxDim(uint32_t dim, bool) {
  g_vres = dim;
}
void SetEyeForwardCm(int cm) {
  g_eyeFwdCm = cm;
}
float GetFpForwardFovDegrees() {
  return static_cast<float>(g_fpFov);
}
float GetFpRearFovDegrees() {
  return 90.f;
}
float GetFpFootFovDegrees() {
  return 90.f;
}
void ForceFpFovDegrees(int forward, int, int) {
  g_fpFov = forward;
}

}  // namespace asi
