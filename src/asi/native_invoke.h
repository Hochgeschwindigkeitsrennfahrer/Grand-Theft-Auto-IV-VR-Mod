#pragma once

#include <cstdint>

namespace asi {

bool InitNativeInvoke();
bool IsNativeInvokeReady();

bool InvokeNativeRaw(uint32_t hash, const uint32_t* args, uint32_t argCount, uint32_t* outResult);

inline void InvokeNativeVoid_I32_I32(uint32_t hash, int32_t a0, int32_t a1) {
  const uint32_t args[] = {static_cast<uint32_t>(a0), static_cast<uint32_t>(a1)};
  InvokeNativeRaw(hash, args, 2, nullptr);
}

}  // namespace asi
