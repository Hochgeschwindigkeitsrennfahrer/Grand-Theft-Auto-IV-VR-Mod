#pragma once

#include <cstdint>

namespace asi {

// Log once: scan main-module AOBs, compare to CE 1.2.0.59 expected RVAs, report
// same-frame seam GATE status. Safe at ASI load — no game hooks.
void LogRePatternValidationOnce();

// True when all required patterns resolve and forbidden sites are not armed.
bool IsSameFrameSeamGateOpen();

// Resolve a same-frame / COUNT site via AOB (preferred) with corrected mapped-RVA
// fallback. Returns 0 on failure. Logs DRIFT/FALLBACK/MISS.
// IMPORTANT: expectedRva must be the LOADED-module RVA (PE VirtualAddress), NOT the
// raw file offset. For CE .text, file_offset + 0xC00 == mapped RVA.
struct ReSiteSpec {
  const char* name;
  const char* pattern;  // IDA-style; may be null to skip AOB
  uint32_t expectedRva;
  const uint8_t* prologue;
  size_t prologueLen;
  bool requireCcPad;
};

uint32_t ResolveReSite(const ReSiteSpec& spec);

}  // namespace asi
