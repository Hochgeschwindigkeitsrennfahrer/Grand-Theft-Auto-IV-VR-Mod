#pragma once

#include <cstdint>

namespace asi {

bool InitNativeInvoke();
bool IsNativeInvokeReady();

bool InvokeNativeRaw(uint32_t hash, const uint32_t* args, uint32_t argCount, uint32_t* outResult);

// Call a resolved native handler (same NativeContext layout as InvokeNativeRaw).
bool InvokeNativeFn(void* handler, const uint32_t* args, uint32_t argCount, uint32_t* outResult);

// Like InvokeNativeFn, but also copies the first outFloatCount result floats
// (IV Vector3 returns live in the NativeContext stack slots 0..2).
bool InvokeNativeFnResultFloats(void* handler, const uint32_t* args, uint32_t argCount,
                                float* outFloats, uint32_t outFloatCount);

// Resolve handler by hash (FusionFix table / getNativeAddress).
void* GetNativeHandlerPtr(uint32_t hash);

// Optional: ScriptHook.dll Game::GetNativeAddress("NAME") — works on CE with
// aCompleteEditionHook. Returns nullptr if ScriptHook is absent.
void* GetNativeHandlerByName(const char* name);

inline void InvokeNativeVoid_I32_I32(uint32_t hash, int32_t a0, int32_t a1) {
  const uint32_t args[] = {static_cast<uint32_t>(a0), static_cast<uint32_t>(a1)};
  InvokeNativeRaw(hash, args, 2, nullptr);
}

inline uint32_t FloatBits(float f) {
  union {
    float f;
    uint32_t u;
  } v;
  v.f = f;
  return v.u;
}

}  // namespace asi
