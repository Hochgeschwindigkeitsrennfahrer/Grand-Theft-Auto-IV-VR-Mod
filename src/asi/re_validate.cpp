#include "re_validate.h"
#include "aob.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>

namespace asi {
namespace {

struct ExpectedSite {
  const char* name;
  const char* pattern;
  uint32_t expectedRva;  // 0 = any hit OK
  bool required;
};

std::atomic<bool> g_logged{false};
std::atomic<bool> g_gateOpen{false};

uint32_t FileOffsetToRva(uintptr_t hit) {
  HMODULE exe = GetModuleHandleA(nullptr);
  if (!exe || !hit)
    return 0;
  return static_cast<uint32_t>(hit - reinterpret_cast<uintptr_t>(exe));
}

bool VerifyBytesAtRva(uint32_t rva, const uint8_t* expect, size_t n) {
  HMODULE exe = GetModuleHandleA(nullptr);
  if (!exe || !rva)
    return false;
  const auto* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(exe) + rva);
  __try {
    for (size_t i = 0; i < n; ++i) {
      if (p[i] != expect[i])
        return false;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

void LogRePatternValidationOnce() {
  if (g_logged.exchange(true))
    return;

  Log("ReValidate: CE GTAIV.exe offline AOB pass (read-only; no game hooks)");

  static const ExpectedSite kSites[] = {
      {"CopyMat onfoot_front", "E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6", 0x61FFC8, true},
      {"FindPlayerPed", "8B 44 24 04 85 C0 75 18 A1", 0x4D14E0, true},
      {"FovSite CE CALL", "E8 ? ? ? ? F6 87 ? ? ? ? ? 5B", 0x70637C, true},
      {"BuildRenderList DrawScene mid", "83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C", 0x6DC60D,
       true},
      {"BuildRenderList PhaseA mid", "83 BF 38 09 00 00 FF 0F 84 76 03 00 00 80 3D", 0x527EDE, true},
      {"BuildRenderList PhaseC mid", "83 BF 38 09 00 00 FF 0F 84 77 02 00 00 8D 8F B0 00 00 00",
       0x975D77, true},
  };

  int ok = 0;
  int required = 0;
  for (const ExpectedSite& site : kSites) {
    if (site.required)
      ++required;
    const uintptr_t hit = FindPattern(nullptr, site.pattern);
    const uint32_t rva = FileOffsetToRva(hit);
    if (!hit) {
      Log("ReValidate: MISS %s", site.name);
      continue;
    }
    if (site.expectedRva != 0 && rva != site.expectedRva) {
      Log("ReValidate: DRIFT %s hit=0x%X expected=0x%X", site.name, rva, site.expectedRva);
      continue;
    }
    ++ok;
    Log("ReValidate: OK %s @ exeRva=0x%X", site.name, rva);
  }

  // CopyMat CALL target (all four sites call the same fn on CE).
  const uintptr_t copySite = FindPattern(nullptr, kSites[0].pattern);
  if (copySite && reinterpret_cast<const uint8_t*>(copySite)[0] == 0xE8) {
    const uint32_t tgt = FileOffsetToRva(GetCallTarget(copySite));
    Log("ReValidate: CopyMat callee exeRva=0x%X (expect 0x83DB90)", tgt);
    if (tgt == 0x83DB90)
      ++ok;
  }

  const uintptr_t fovSite = FindPattern(nullptr, kSites[2].pattern);
  if (fovSite && reinterpret_cast<const uint8_t*>(fovSite)[0] == 0xE8) {
    const uint32_t tgt = FileOffsetToRva(GetCallTarget(fovSite));
    Log("ReValidate: FovSite callee exeRva=0x%X (expect 0x706A00)", tgt);
    if (tgt == 0x706A00)
      ++ok;
  }

  // Anchor bytes at known RVAs (forbidden + observation sites).
  static const uint8_t kVsRet[] = {0x85, 0xC0, 0x75, 0x14};
  static const uint8_t kVsWrap[] = {0x89, 0x51, 0x0A};
  static const uint8_t kBuildRootA[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
  Log("ReValidate: VsRet 0x2C73E bytes=%s",
      VerifyBytesAtRva(0x2C73E, kVsRet, sizeof(kVsRet)) ? "match" : "DRIFT");
  Log("ReValidate: VS wrap 0x2C6AC bytes=%s (FORBIDDEN hook)",
      VerifyBytesAtRva(0x2C6AC, kVsWrap, sizeof(kVsWrap)) ? "match" : "DRIFT");
  Log("ReValidate: BuildRootA 0x8F7F00 prologue=%s",
      VerifyBytesAtRva(0x8F7F00, kBuildRootA, sizeof(kBuildRootA)) ? "match" : "DRIFT");
  Log("ReValidate: VsRet+wrap share fn start ~0x2C180 (upload wrapper; indirect callers only)");

  // Same-frame seam gate: patterns OK is necessary but NOT sufficient — no proven walker.
  const bool patternsOk = (ok >= required);
  g_gateOpen.store(false);  // honest: RE has no safe replay owner yet
  Log("ReValidate: patterns %d/%d required OK; SameFrameSeamGate=CLOSED "
      "(need replay owner ABI proof — see docs/RE_OFFSETS.md)",
      ok, required);
  if (!patternsOk)
    Log("ReValidate: WARNING exe may differ from CE 1.2.0.59 — update RE_OFFSETS expected RVAs");
  (void)patternsOk;
}

bool IsSameFrameSeamGateOpen() {
  if (!g_logged.load())
    LogRePatternValidationOnce();
  return g_gateOpen.load();
}

}  // namespace asi
