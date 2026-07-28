#pragma once

namespace asi {

// Resolve CTimer pointers once (FusionFix CE AOBs). Soft-fail → no-ops.
bool ResolveGameTimer();

// Freeze sim clock for same-tick dual (Mode 189):
//   fTimeStep=0, pin m_snTimeInMilliseconds, m_CodePause=1.
// Restore on End. Idempotent. Always safe to call End (device reset / SEH).
void BeginDualTimeFreeze();
void EndDualTimeFreeze();
// Re-write pinned ms immediately before Right eye (if anything nudged the clock).
void RepinDualTimeMs();

bool IsDualTimeFreezeActive();

}  // namespace asi
