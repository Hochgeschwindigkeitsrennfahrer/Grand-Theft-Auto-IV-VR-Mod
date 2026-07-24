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

  Log("ReValidate: CE GTAIV.exe AOB pass (read-only; Mode 62 safe — no game hooks)");

  static const ExpectedSite kSites[] = {
      {"CopyMat onfoot_front", "E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6", 0x61FFC8, true},
      {"CopyMat onfoot_behind", "E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74", 0x61FF9B, true},
      {"CopyMat vehicle_front", "E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24", 0x615AE6,
       true},
      {"CopyMat vehicle_behind", "E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00", 0x6180B3, true},
      {"FindPlayerPed", "8B 44 24 04 85 C0 75 18 A1", 0x4D14E0, true},
      {"FovSite CE CALL", "E8 ? ? ? ? F6 87 ? ? ? ? ? 5B", 0x70637C, true},
      {"VS wrapper prologue", "55 8B EC 83 E4 F8 81 EC A0 03 00 00", 0x2C180, false},
      {"BuildRootA prologue", "55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08", 0x8F7F00, false},
      {"BuildRenderList DrawScene mid", "83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C", 0x6DC60D,
       true},
      {"BuildRenderList PhaseA mid", "83 BF 38 09 00 00 FF 0F 84 76 03 00 00 80 3D", 0x527EDE, true},
      {"BuildRenderList PhaseC mid", "83 BF 38 09 00 00 FF 0F 84 77 02 00 00 8D 8F B0 00 00 00",
       0x975D77, true},
      {"PedHide SetDraw", "E8 ? ? ? ? 83 C4 04 85 C0 74 ? 8B 0D", 0x7B478C, false},
  };

  int ok = 0;
  int required = 0;
  int requiredOk = 0;
  for (const ExpectedSite& site : kSites) {
    if (site.required)
      ++required;
    const uintptr_t hit = FindPattern(nullptr, site.pattern);
    const uint32_t rva = FileOffsetToRva(hit);
    if (!hit) {
      Log("ReValidate: MISS %s%s", site.name, site.required ? " (required)" : "");
      continue;
    }
    if (site.expectedRva != 0 && rva != site.expectedRva) {
      Log("ReValidate: DRIFT %s hit=0x%X expected=0x%X%s", site.name, rva, site.expectedRva,
          site.required ? " (required)" : "");
      continue;
    }
    ++ok;
    if (site.required)
      ++requiredOk;
    Log("ReValidate: OK %s @ exeRva=0x%X%s", site.name, rva,
        site.required ? "" : " (optional)");
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
  static const uint8_t kCopyMat[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
  static const uint8_t kFovCallee[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
  static const uint8_t kVsWrapFn[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
  static const uint8_t kIndirectCall[] = {0xFF, 0x90, 0x78, 0x01, 0x00, 0x00};  // call [eax+178]
  static const uint8_t kViewConstFn[] = {0x56, 0x57, 0x8B, 0xF9, 0x8D, 0x44, 0x24, 0x70};
  int anchors = 0;
  if (VerifyBytesAtRva(0x2C73E, kVsRet, sizeof(kVsRet))) {
    ++anchors;
    Log("ReValidate: anchor VsRet 0x2C73E match (test eax,eax; jnz — replay return)");
  } else {
    Log("ReValidate: anchor DRIFT VsRet 0x2C73E");
  }
  if (VerifyBytesAtRva(0x2C6AC, kVsWrap, sizeof(kVsWrap))) {
    ++anchors;
    Log("ReValidate: anchor VS wrap 0x2C6AC match (FORBIDDEN — mov [ecx+0x0A],edx)");
  } else {
    Log("ReValidate: anchor DRIFT VS wrap 0x2C6AC");
  }
  if (VerifyBytesAtRva(0x2C180, kVsWrapFn, sizeof(kVsWrapFn))) {
    ++anchors;
    Log("ReValidate: anchor VS upload wrapper 0x2C180 prologue match");
  }
  if (VerifyBytesAtRva(0x8F7F00, kBuildRootA, sizeof(kBuildRootA))) {
    ++anchors;
    Log("ReValidate: anchor BuildRootA 0x8F7F00 prologue match");
  }
  if (VerifyBytesAtRva(0x83DB90, kCopyMat, sizeof(kCopyMat))) {
    ++anchors;
    Log("ReValidate: anchor CopyMat 0x83DB90 prologue match");
  }
  if (VerifyBytesAtRva(0x706A00, kFovCallee, sizeof(kFovCallee))) {
    ++anchors;
    Log("ReValidate: anchor FovRecompute 0x706A00 prologue match");
  }
  if (VerifyBytesAtRva(0x3010D, kIndirectCall, sizeof(kIndirectCall))) {
    ++anchors;
    Log("ReValidate: anchor indirect replay call 0x3010D match (call [eax+0x178])");
  } else {
    Log("ReValidate: anchor DRIFT indirect 0x3010D (Mode 42 stack ret was mid-SSE)");
  }
  if (VerifyBytesAtRva(0x3187C, kViewConstFn, sizeof(kViewConstFn))) {
    ++anchors;
    Log("ReValidate: anchor view-const 0x3187C prologue match (Mode 64 COUNT target)");
  } else {
    Log("ReValidate: anchor DRIFT view-const 0x3187C");
  }
  static const uint8_t kPublishSync[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x56, 0x8B, 0xF1};
  if (VerifyBytesAtRva(0x30300, kPublishSync, sizeof(kPublishSync))) {
    ++anchors;
    Log("ReValidate: anchor PublishSync 0x30300 prologue match (Mode 66 COUNT target)");
  } else {
    Log("ReValidate: anchor DRIFT PublishSync 0x30300");
  }
  Log("ReValidate: replay path — 0 static E8->VsRet; 9 E8 into 0x2C180-0x2C7FE; "
      "SameFrameSeamGate needs ~1x/frame owner (see 0x3187C UNTESTED)");

  // Same-frame seam gate: patterns OK is necessary but NOT sufficient — no proven walker.
  const bool patternsOk = (requiredOk >= required);
  g_gateOpen.store(false);  // honest: RE has no safe replay owner yet
  Log("ReValidate: summary required=%d/%d optional=%d anchors=%d SameFrameSeamGate=CLOSED",
      requiredOk, required, ok - requiredOk, anchors);
  if (!patternsOk)
    Log("ReValidate: WARNING exe may differ from CE 1.2.0.59 — run scripts/offline-re-scan.py");
  (void)patternsOk;
}

bool IsSameFrameSeamGateOpen() {
  if (!g_logged.load())
    LogRePatternValidationOnce();
  return g_gateOpen.load();
}

}  // namespace asi
