#include "aob.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace asi {
namespace {

struct PatByte {
  uint8_t value = 0;
  bool any = false;
};

bool ParsePattern(const char* pattern, std::vector<PatByte>* out) {
  out->clear();
  const char* p = pattern;
  while (*p) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    PatByte b{};
    if (*p == '?') {
      b.any = true;
      ++p;
      if (*p == '?')
        ++p;
    } else {
      unsigned int v = 0;
      if (sscanf_s(p, "%2x", &v) != 1)
        return false;
      b.value = static_cast<uint8_t>(v);
      p += 2;
    }
    out->push_back(b);
  }
  return !out->empty();
}

}  // namespace

uintptr_t FindPatternN(const char* moduleName, const char* pattern, int occurrence) {
  if (occurrence < 0)
    return 0;

  HMODULE mod = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
  if (!mod)
    return 0;

  MODULEINFO mi{};
  if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    return 0;

  std::vector<PatByte> pat;
  if (!ParsePattern(pattern, &pat))
    return 0;

  auto* base = static_cast<const uint8_t*>(mi.lpBaseOfDll);
  const size_t size = mi.SizeOfImage;
  const size_t n = pat.size();
  if (n == 0 || size < n)
    return 0;

  int found = 0;
  for (size_t i = 0; i + n <= size; ++i) {
    bool ok = true;
    for (size_t j = 0; j < n; ++j) {
      if (!pat[j].any && base[i + j] != pat[j].value) {
        ok = false;
        break;
      }
    }
    if (ok) {
      if (found == occurrence)
        return reinterpret_cast<uintptr_t>(base + i);
      ++found;
    }
  }
  return 0;
}

uintptr_t FindPattern(const char* moduleName, const char* pattern) {
  return FindPatternN(moduleName, pattern, 0);
}

uintptr_t GetCallTarget(uintptr_t callInstr) {
  auto* p = reinterpret_cast<const uint8_t*>(callInstr);
  if (p[0] != 0xE8)
    return 0;
  const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
  return callInstr + 5 + rel;
}

uintptr_t HookCallSite(uintptr_t callInstr, void* detour) {
  auto* p = reinterpret_cast<uint8_t*>(callInstr);
  if (p[0] != 0xE8)
    return 0;

  const uintptr_t original = GetCallTarget(callInstr);
  DWORD oldProt = 0;
  if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    return 0;

  const intptr_t rel = reinterpret_cast<intptr_t>(detour) - static_cast<intptr_t>(callInstr + 5);
  p[0] = 0xE8;
  *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(rel);
  VirtualProtect(p, 5, oldProt, &oldProt);
  FlushInstructionCache(GetCurrentProcess(), p, 5);
  return original;
}

}  // namespace asi
