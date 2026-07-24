#pragma once

namespace asi {

// Log once: scan main-module AOBs, compare to CE 1.2.0.59 expected RVAs, report
// same-frame seam GATE status. Safe at ASI load — no game hooks.
void LogRePatternValidationOnce();

// True when all required patterns resolve and forbidden sites are not armed.
bool IsSameFrameSeamGateOpen();

}  // namespace asi
