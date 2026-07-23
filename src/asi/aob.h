#pragma once

#include <cstdint>

namespace asi {

// Find IDA-style pattern ("E8 ? ? ? ? 8A 86") in module.
// moduleName null = main exe. occurrence 0 = first match. Returns 0 on failure.
uintptr_t FindPattern(const char* moduleName, const char* pattern);
uintptr_t FindPatternN(const char* moduleName, const char* pattern, int occurrence);

// E8 rel32 → absolute target
uintptr_t GetCallTarget(uintptr_t callInstr);

// Patch E8 at callInstr to jump to detour. Returns previous target (0 on fail).
uintptr_t HookCallSite(uintptr_t callInstr, void* detour);

}  // namespace asi
