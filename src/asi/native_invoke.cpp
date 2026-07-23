#include "native_invoke.h"
#include "aob.h"
#include "log.h"

namespace asi {
namespace {

struct Vec3 {
  float x, y, z;
};
struct Vec4 {
  float x, y, z, w;
};

class NativeContext {
  enum { MaxNativeParams = 16, ArgSize = 4 };

  void* m_pReturn = nullptr;
  uint32_t m_nArgCount = 0;
  void* m_pArgs = nullptr;
  uint32_t m_nDataCount = 0;
  Vec3* m_pOriginalData[4]{};
  Vec4 m_TemporaryData[4]{};
  uint8_t m_TempStack[MaxNativeParams * ArgSize]{};

 public:
  NativeContext() {
    m_pArgs = m_TempStack;
    m_pReturn = m_TempStack;
  }

  void PushU32(uint32_t value) {
    *reinterpret_cast<uint32_t*>(m_TempStack + ArgSize * m_nArgCount) = value;
    ++m_nArgCount;
  }

  uint32_t GetResultU32() const { return *reinterpret_cast<const uint32_t*>(m_TempStack); }
};

using NativeFn = void (*)(NativeContext*);
using GetNativeAddress_t = void*(__stdcall*)(uint32_t);

uint32_t* g_nativeTableSize = nullptr;
uint32_t** g_pNatives = nullptr;
GetNativeAddress_t g_getNativeAddress = nullptr;
bool g_ready = false;
bool g_loggedProbe = false;

uint32_t* ReadImmPtr(uintptr_t match) {
  if (!match)
    return nullptr;
  return *reinterpret_cast<uint32_t**>(match + 2);
}

bool TableUsable() {
  if (!g_pNatives || !g_nativeTableSize)
    return false;
  if (!*g_pNatives)
    return false;
  return *g_nativeTableSize != 0;
}

bool TryBindTable(int occurrence) {
  const uintptr_t sizePat = FindPatternN(
      nullptr, "8B 35 ? ? ? ? 85 F6 75 06 33 C0 5E C2 04 00 53 57 8B 7C 24 10", occurrence);
  const uintptr_t natPat =
      FindPatternN(nullptr, "8B 1D ? ? ? ? 8B CF 8B 04 D3 3B C7 74 19 8D 64 24 00 85 C0", occurrence);
  if (!sizePat || !natPat)
    return false;

  auto* sizePtr = reinterpret_cast<uint32_t*>(ReadImmPtr(sizePat));
  auto* natPtr = reinterpret_cast<uint32_t**>(ReadImmPtr(natPat));
  if (!sizePtr || !natPtr)
    return false;

  g_nativeTableSize = sizePtr;
  g_pNatives = natPtr;
  Log("NativeInvoke: table occ=%d size@%p natives@%p tableSize=%u pTable=%p", occurrence,
      reinterpret_cast<void*>(g_nativeTableSize), reinterpret_cast<void*>(g_pNatives),
      *g_nativeTableSize, reinterpret_cast<void*>(*g_pNatives));
  return TableUsable();
}

NativeFn GetNativeHandler(uint32_t hash) {
  if (g_getNativeAddress) {
    if (void* p = g_getNativeAddress(hash))
      return reinterpret_cast<NativeFn>(p);
  }

  if (!TableUsable() || hash == 0)
    return nullptr;

  uint32_t* natives = *g_pNatives;
  const uint32_t tableSize = *g_nativeTableSize;

  uint32_t nativeIndex = hash % tableSize;
  uint32_t tmpHash = hash;
  uint32_t handlerHash = natives[2 * nativeIndex];

  if (handlerHash != hash) {
    while (handlerHash) {
      tmpHash = (tmpHash >> 1) + 1;
      nativeIndex = (tmpHash + nativeIndex) % tableSize;
      handlerHash = natives[2 * nativeIndex];
      if (handlerHash == hash)
        break;
    }
  }

  if (!handlerHash)
    return nullptr;

  return reinterpret_cast<NativeFn>(natives[2 * nativeIndex + 1]);
}

}  // namespace

bool InitNativeInvoke() {
  if (g_ready)
    return true;

  // FusionFix: 2nd match of this pattern is getNativeAddress (stdcall)
  if (!g_getNativeAddress) {
    const uintptr_t fn = FindPatternN(nullptr, "56 8B 35 ? ? ? ? 85 F6 75 06", 1);
    if (fn) {
      g_getNativeAddress = reinterpret_cast<GetNativeAddress_t>(fn);
      Log("NativeInvoke: getNativeAddress @ %p", reinterpret_cast<void*>(fn));
    }
  }

  // Prefer a live hash table (size != 0). Try both CE copies.
  if (!TableUsable()) {
    if (!TryBindTable(0))
      TryBindTable(1);
    else if (!TableUsable())
      TryBindTable(1);
  }

  // Usable if we can resolve a known native via either path
  constexpr uint32_t kProbe = 0x3EFE3DC8;  // SET_DRAW_PLAYER_COMPONENT
  if (GetNativeHandler(kProbe)) {
    g_ready = true;
    Log("NativeInvoke: READY (probe SET_DRAW_PLAYER_COMPONENT ok)");
    return true;
  }

  if (!g_loggedProbe) {
    Log("NativeInvoke: waiting for native registration (getNative=%d tableSize=%u)",
        g_getNativeAddress ? 1 : 0, g_nativeTableSize ? *g_nativeTableSize : 0u);
    g_loggedProbe = true;
  }
  return false;
}

bool IsNativeInvokeReady() {
  return g_ready;
}

bool InvokeNativeRaw(uint32_t hash, const uint32_t* args, uint32_t argCount, uint32_t* outResult) {
  if (!g_ready && !InitNativeInvoke())
    return false;

  NativeFn fn = GetNativeHandler(hash);
  if (!fn)
    return false;

  NativeContext cxt;
  for (uint32_t i = 0; i < argCount && i < 16; ++i)
    cxt.PushU32(args[i]);

  fn(&cxt);
  if (outResult)
    *outResult = cxt.GetResultU32();
  return true;
}

}  // namespace asi
