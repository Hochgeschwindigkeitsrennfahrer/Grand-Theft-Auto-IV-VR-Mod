#include "re_validate.h"
#include "aob.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace asi {
namespace {

struct ExpectedSite {
  const char* name;
  const char* pattern;
  uint32_t expectedRva;  // LOADED-module RVA (file_offset + 0xC00 for CE .text)
  bool required;
};

std::atomic<bool> g_logged{false};
std::atomic<bool> g_gateOpen{false};

uint32_t AddrToRva(uintptr_t hit) {
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

bool HasCcPad(uintptr_t start) {
  if (start < 2)
    return false;
  const auto* b = reinterpret_cast<const uint8_t*>(start);
  __try {
    return b[-1] == 0xCC && b[-2] == 0xCC;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

uint32_t ResolveReSite(const ReSiteSpec& spec) {
  if (!spec.name || !spec.prologue || spec.prologueLen == 0)
    return 0;

  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  if (!base)
    return 0;

  uint32_t hitRva = 0;
  if (spec.pattern && spec.pattern[0]) {
    const uintptr_t hit = FindPattern(nullptr, spec.pattern);
    if (hit)
      hitRva = AddrToRva(hit);
  }

  auto tryRva = [&](uint32_t rva, const char* how) -> uint32_t {
    if (!rva)
      return 0;
    if (!VerifyBytesAtRva(rva, spec.prologue, spec.prologueLen)) {
      Log("ReSite: %s %s exeRva=0x%X prologue mismatch", spec.name, how, rva);
      return 0;
    }
    if (spec.requireCcPad && !HasCcPad(base + rva)) {
      Log("ReSite: %s %s exeRva=0x%X missing CC-pad", spec.name, how, rva);
      return 0;
    }
    if (spec.expectedRva && rva != spec.expectedRva)
      Log("ReSite: DRIFT %s hit=0x%X expected=0x%X (using hit; how=%s)", spec.name, rva,
          spec.expectedRva, how);
    else
      Log("ReSite: OK %s exeRva=0x%X how=%s", spec.name, rva, how);
    return rva;
  };

  // Prefer expected mapped RVA when prologue verifies. Short AOBs can hit lookalikes
  // first (PublishSync short prologue has 21 .text hits); Mode 64/66/67 patterns are
  // unique today, but expected-first is the safe CE policy after +0xC00 skew.
  if (spec.expectedRva) {
    if (VerifyBytesAtRva(spec.expectedRva, spec.prologue, spec.prologueLen)) {
      if (hitRva && hitRva != spec.expectedRva)
        Log("ReSite: %s AOB alt hit=0x%X ignored; using expected=0x%X", spec.name, hitRva,
            spec.expectedRva);
      if (const uint32_t ok = tryRva(spec.expectedRva, hitRva == spec.expectedRva ? "AOB" : "EXPECTED"))
        return ok;
    }
  }

  if (hitRva && hitRva != spec.expectedRva) {
    if (const uint32_t ok = tryRva(hitRva, "AOB"))
      return ok;
  }

  Log("ReSite: MISS %s (AOB+expected failed; expected=0x%X)", spec.name, spec.expectedRva);
  return 0;
}

void LogRePatternValidationOnce() {
  if (g_logged.exchange(true))
    return;

  Log("ReValidate: CE GTAIV.exe AOB pass (read-only; Mode 62 safe — no game hooks)");
  Log("ReValidate: NOTE mapped RVA = file_offset + 0xC00 for .text/.rdata (PE section skew)");

  // expectedRva values are LOADED-module RVAs (corrected +0xC00 from old file-offset docs).
  static const ExpectedSite kSites[] = {
      {"CopyMat onfoot_front", "E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6", 0x620BC8, true},
      {"CopyMat onfoot_behind", "E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74", 0x620B9B, true},
      {"CopyMat vehicle_front", "E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24", 0x6166E6,
       true},
      {"CopyMat vehicle_behind", "E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00", 0x618CB3, true},
      {"FindPlayerPed", "8B 44 24 04 85 C0 75 18 A1", 0x4D20E0, true},
      {"FovSite CE CALL", "E8 ? ? ? ? F6 87 ? ? ? ? ? 5B", 0x706F7C, true},
      {"VS wrapper prologue", "55 8B EC 83 E4 F8 81 EC A0 03 00 00", 0x2CD80, false},
      {"BuildRootA prologue", "55 8B EC 83 E4 F0 83 EC 18 56 57 8B 7D 08", 0x8F8B00, false},
      {"BuildRenderList DrawScene mid", "83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C", 0x6DD20D,
       true},
      {"BuildRenderList PhaseA mid", "83 BF 38 09 00 00 FF 0F 84 76 03 00 00 80 3D", 0x528ADE, true},
      {"BuildRenderList PhaseC mid", "83 BF 38 09 00 00 FF 0F 84 77 02 00 00 8D 8F B0 00 00 00",
       0x976977, true},
      {"PedHide SetDraw", "E8 ? ? ? ? 83 C4 04 85 C0 74 ? 8B 0D", 0x7B538C, false},
      {"PublishSync",
       "55 8B EC 83 E4 F8 51 56 8B F1 8D 86 80 00 00 00 50 8D 86 80 01 00 00 50 8D 8E C0 00 00 00 "
       "E8",
       0x30F00, false},
      {"ViewMatWriter",
       "55 8B EC 83 E4 F8 83 EC 10 56 57 8B F9 0F 57 C9 F3 0F 10 87 BC 02 00 00", 0x314C0, false},
      {"ViewConst",
       "55 8B EC 83 E4 F0 81 EC A8 00 00 00 56 57 8B F9 8D 44 24 70 F3 0F 10 87", 0x32470, false},
  };

  int ok = 0;
  int required = 0;
  int requiredOk = 0;
  for (const ExpectedSite& site : kSites) {
    if (site.required)
      ++required;
    const uintptr_t hit = FindPattern(nullptr, site.pattern);
    const uint32_t rva = AddrToRva(hit);
    if (!hit) {
      Log("ReValidate: MISS %s%s", site.name, site.required ? " (required)" : "");
      continue;
    }
    if (site.expectedRva != 0 && rva != site.expectedRva) {
      Log("ReValidate: DRIFT %s hit=0x%X expected=0x%X%s", site.name, rva, site.expectedRva,
          site.required ? " (required)" : "");
      // Still count as OK if AOB hit — pattern matched live code (RVA table may lag).
      ++ok;
      if (site.required)
        ++requiredOk;
      continue;
    }
    ++ok;
    if (site.required)
      ++requiredOk;
    Log("ReValidate: OK %s @ exeRva=0x%X%s", site.name, rva,
        site.required ? "" : " (optional)");
  }

  static const uint8_t kVsRet[] = {0x85, 0xC0, 0x75, 0x14};
  static const uint8_t kVsWrap[] = {0x89, 0x51, 0x0A};
  static const uint8_t kBuildRootA[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
  static const uint8_t kVsWrapFn[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
  static const uint8_t kIndirectCall[] = {0xFF, 0x90, 0x78, 0x01, 0x00, 0x00};
  static const uint8_t kViewConstFn[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
                                         0xA8, 0x00, 0x00, 0x00};
  static const uint8_t kPublishSync[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0x56, 0x8B, 0xF1};
  static const uint8_t kViewMatWriter[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x10,
                                           0x56, 0x57, 0x8B, 0xF9};
  static const uint8_t kReplayDisp[] = {0x83, 0x3D, 0x18, 0xD9, 0x7E, 0x01, 0x00, 0x56, 0x8B, 0xF1};
  static const uint8_t kMatMul[] = {0x55, 0x8B, 0xEC};
  static const uint8_t kPublishProj[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
                                         0x88, 0x00, 0x00, 0x00};
  static const uint8_t kUploadFn[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
                                      0xD8, 0x00, 0x00, 0x00};
  static const uint8_t kCall178Ecx[] = {0xFF, 0x91, 0x78, 0x01, 0x00, 0x00};
  int anchors = 0;
  if (VerifyBytesAtRva(0x2D33E, kVsRet, sizeof(kVsRet))) {
    ++anchors;
    Log("ReValidate: anchor VsRet 0x2D33E match (mapped; old file-off doc was 0x2C73E)");
  } else {
    Log("ReValidate: anchor DRIFT VsRet 0x2D33E");
  }
  if (VerifyBytesAtRva(0x2D2AC, kVsWrap, sizeof(kVsWrap))) {
    ++anchors;
    Log("ReValidate: anchor VS wrap 0x2D2AC match (FORBIDDEN — mapped from old 0x2C6AC)");
  } else {
    Log("ReValidate: anchor DRIFT VS wrap 0x2D2AC");
  }
  if (VerifyBytesAtRva(0x2CD80, kVsWrapFn, sizeof(kVsWrapFn))) {
    ++anchors;
    Log("ReValidate: anchor VS upload wrapper 0x2CD80 prologue match");
  }
  if (VerifyBytesAtRva(0x8F8B00, kBuildRootA, sizeof(kBuildRootA))) {
    ++anchors;
    Log("ReValidate: anchor BuildRootA 0x8F8B00 prologue match");
  }
  if (VerifyBytesAtRva(0x30D0D, kIndirectCall, sizeof(kIndirectCall))) {
    ++anchors;
    Log("ReValidate: anchor indirect replay call 0x30D0D match (call [eax+0x178])");
  } else {
    Log("ReValidate: anchor DRIFT indirect 0x30D0D");
  }
  if (VerifyBytesAtRva(0x32470, kViewConstFn, sizeof(kViewConstFn))) {
    ++anchors;
    Log("ReValidate: anchor view-const 0x32470 TRUE start match (Mode 64; mid 0x3247C unsafe)");
  } else {
    Log("ReValidate: anchor DRIFT view-const 0x32470");
  }
  if (VerifyBytesAtRva(0x30F00, kPublishSync, sizeof(kPublishSync))) {
    ++anchors;
    Log("ReValidate: anchor PublishSync 0x30F00 prologue match (Mode 66)");
  } else {
    Log("ReValidate: anchor DRIFT PublishSync 0x30F00");
  }
  if (VerifyBytesAtRva(0x314C0, kViewMatWriter, sizeof(kViewMatWriter))) {
    ++anchors;
    Log("ReValidate: anchor ViewMatWriter 0x314C0 prologue match (Mode 67)");
  } else {
    Log("ReValidate: anchor DRIFT ViewMatWriter 0x314C0");
  }
  if (VerifyBytesAtRva(0x30CD0, kReplayDisp, sizeof(kReplayDisp))) {
    ++anchors;
    Log("ReValidate: anchor ReplayDispatch 0x30CD0 match (owner of call [eax+0x178])");
  } else {
    Log("ReValidate: anchor DRIFT ReplayDispatch 0x30CD0");
  }
  if (VerifyBytesAtRva(0x307F0, kMatMul, sizeof(kMatMul))) {
    ++anchors;
    Log("ReValidate: anchor MatMul 0x307F0 match (PublishSync callee; not 0x307BF0)");
  } else {
    Log("ReValidate: anchor DRIFT MatMul 0x307F0");
  }
  if (VerifyBytesAtRva(0x31BA0, kPublishProj, sizeof(kPublishProj))) {
    ++anchors;
    Log("ReValidate: anchor PublishProj 0x31BA0 match (11 E8; after PublishSync)");
  } else {
    Log("ReValidate: anchor DRIFT PublishProj 0x31BA0");
  }
  if (VerifyBytesAtRva(0x2A1E10, kUploadFn, sizeof(kUploadFn))) {
    ++anchors;
    Log("ReValidate: anchor UploadFn 0x2A1E10 match (vtable MatMulx3 +178 paths)");
  } else {
    Log("ReValidate: anchor DRIFT UploadFn 0x2A1E10");
  }
  if (VerifyBytesAtRva(0x2A217D, kCall178Ecx, sizeof(kCall178Ecx))) {
    ++anchors;
    Log("ReValidate: anchor UploadA +178 @0x2A217D (OWNER-EDGE ret 0x2A2183)");
  } else {
    Log("ReValidate: anchor DRIFT UploadA 0x2A217D");
  }
  if (VerifyBytesAtRva(0x2A25F9, kCall178Ecx, sizeof(kCall178Ecx))) {
    ++anchors;
    Log("ReValidate: anchor UploadB +178 @0x2A25F9 (OWNER-EDGE ret 0x2A25FF)");
  } else {
    Log("ReValidate: anchor DRIFT UploadB 0x2A25F9");
  }
  Log("ReValidate: replay path — fixed-RVA docs were file offsets; use mapped RVAs / AOB");

  const bool patternsOk = (requiredOk >= required);
  g_gateOpen.store(false);
  Log("ReValidate: summary required=%d/%d optionalAnchors=%d anchors=%d SameFrameSeamGate=CLOSED",
      requiredOk, required, ok - requiredOk, anchors);
  if (!patternsOk)
    Log("ReValidate: WARNING required AOB miss — exe may differ from CE 1.2.0.59");
  (void)patternsOk;
}

bool IsSameFrameSeamGateOpen() {
  if (!g_logged.load())
    LogRePatternValidationOnce();
  return g_gateOpen.load();
}

}  // namespace asi
