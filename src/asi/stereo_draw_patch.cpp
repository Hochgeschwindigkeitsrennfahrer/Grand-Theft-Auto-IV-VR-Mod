#include "stereo_draw_patch.h"

#include "log.h"
#include "shader_ctab.h"
#include "stereo_wvp_math.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#include <d3d9.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace asi {
namespace {

using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUp_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUp_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
    D3DFORMAT, const void*, UINT);
using DrawRectPatch_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, const float*, const D3DRECTPATCH_INFO*);
using DrawTriPatch_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, const float*, const D3DTRIPATCH_INFO*);

DrawPrimitive_t g_originalDrawPrimitive = nullptr;
DrawIndexedPrimitive_t g_originalDrawIndexedPrimitive = nullptr;
DrawPrimitiveUp_t g_originalDrawPrimitiveUp = nullptr;
DrawIndexedPrimitiveUp_t g_originalDrawIndexedPrimitiveUp = nullptr;
DrawRectPatch_t g_originalDrawRectPatch = nullptr;
DrawTriPatch_t g_originalDrawTriPatch = nullptr;

std::atomic<bool> g_hooksReady{false};
std::atomic<bool> g_installAttempted{false};
void* g_hookTargets[6] = {};

constexpr uint64_t FnvOffset = 14695981039346656037ull;
constexpr uint64_t FnvPrime = 1099511628211ull;
constexpr uint32_t MaxShaderContracts = 1024u;
constexpr uint32_t MaxPairDraws = 32768u;
constexpr uint32_t MaxShaderBytes = 1024u * 1024u;

enum class PairPass : int {
  Inactive = 0,
  Left = 1,
  Right = 2,
};

enum class DrawContractKind : uint8_t {
  Passthrough = 0,
  WorldAndWvp = 1,
  WvpWithFactor = 2,
  Invalid = 3,
  AuxiliaryWvp = 4,
};

struct ShaderContractCacheEntry {
  IDirect3DVertexShader9* shader = nullptr;
  uint64_t bytecodeHash = 0;
  ShaderMatrixContract matrices{};
  ShaderCtabParseResult parseResult = ShaderCtabParseResult::NoCtab;
};

struct DrawRecord {
  uint64_t signature = 0;
  uint64_t shaderHash = 0;
  DrawContractKind kind = DrawContractKind::Passthrough;
  uint16_t worldRegister = 0;
  uint16_t wvpRegister = 0;
  float originalWvp[16] = {};
  stereo_wvp::Matrix4 cameraFactor{};
};

struct PairState {
  IDirect3DDevice9* device = nullptr;
  DWORD renderThreadId = 0;
  float delta[3] = {};
  uint32_t leftDraws = 0;
  uint32_t rightDraws = 0;
  uint32_t eligibleDraws = 0;
  uint32_t patchedDraws = 0;
  uint32_t factorOnlyDraws = 0;
  uint32_t auxiliaryWvpDraws = 0;
  uint32_t passthroughDraws = 0;
  uint32_t failures = 0;
  uint32_t trustedCameraDraws = 0;
  uint64_t leftHash = FnvOffset;
  uint64_t rightHash = FnvOffset;
};

ShaderContractCacheEntry g_shaderContracts[MaxShaderContracts] = {};
uint32_t g_shaderContractCount = 0;
DrawRecord g_drawRecords[MaxPairDraws] = {};
PairState g_pair{};
std::atomic<int> g_pairPass{static_cast<int>(PairPass::Inactive)};
std::atomic<uint32_t> g_foreignDrawFailures{0u};

uint64_t HashBytes(const void* memory, size_t bytes) {
  const uint8_t* values = static_cast<const uint8_t*>(memory);
  uint64_t hash = FnvOffset;
  for (size_t index = 0; index < bytes; ++index) {
    hash ^= values[index];
    hash *= FnvPrime;
  }
  return hash;
}

uint64_t HashValue(uint64_t hash, uint64_t value) {
  for (size_t byte = 0; byte < sizeof(value); ++byte) {
    hash ^= static_cast<uint8_t>(value >> (byte * 8u));
    hash *= FnvPrime;
  }
  return hash;
}

bool SameFloatMatrix(const float left[16], const float right[16]) {
  stereo_wvp::Matrix4 leftMatrix{};
  stereo_wvp::Matrix4 rightMatrix{};
  return stereo_wvp::FromFloat(left, &leftMatrix) &&
         stereo_wvp::FromFloat(right, &rightMatrix) &&
         stereo_wvp::NearlyEqual(leftMatrix, rightMatrix, 3.0e-4);
}

bool FinalizeLeftContracts() {
  struct CameraCluster {
    stereo_wvp::Matrix4 representative{};
    uint32_t draws = 0u;
  };
  constexpr uint32_t MaxCameraClusters = 16u;
  CameraCluster clusters[MaxCameraClusters] = {};
  uint32_t clusterCount = 0u;

  for (uint32_t draw = 0u; draw < g_pair.leftDraws; ++draw) {
    const DrawRecord& record = g_drawRecords[draw];
    if (record.kind != DrawContractKind::WorldAndWvp)
      continue;
    uint32_t cluster = 0u;
    for (; cluster < clusterCount; ++cluster) {
      if (stereo_wvp::NearlyEqual(
              record.cameraFactor,
              clusters[cluster].representative,
              3.0e-4))
        break;
    }
    if (cluster == clusterCount) {
      if (clusterCount >= MaxCameraClusters) {
        ++g_pair.failures;
        break;
      }
      clusters[cluster].representative = record.cameraFactor;
      ++clusterCount;
    }
    ++clusters[cluster].draws;
  }

  uint32_t dominantCluster = MaxCameraClusters;
  uint32_t dominantDraws = 0u;
  for (uint32_t cluster = 0u; cluster < clusterCount; ++cluster) {
    if (clusters[cluster].draws > dominantDraws) {
      dominantCluster = cluster;
      dominantDraws = clusters[cluster].draws;
    }
  }
  const bool dominantValid =
      g_pair.failures == 0u &&
      dominantCluster < clusterCount &&
      dominantDraws >= 4u &&
      dominantDraws * 2u > g_pair.trustedCameraDraws;
  if (!dominantValid) {
    ++g_pair.failures;
    for (uint32_t draw = 0u; draw < g_pair.leftDraws; ++draw) {
      DrawRecord& record = g_drawRecords[draw];
      if (record.kind == DrawContractKind::WorldAndWvp ||
          record.kind == DrawContractKind::WvpWithFactor)
        record.kind = DrawContractKind::Invalid;
    }
    return false;
  }

  const stereo_wvp::Matrix4& dominantCamera =
      clusters[dominantCluster].representative;
  uint32_t dominantTrustedDraws = 0u;
  for (uint32_t draw = 0u; draw < g_pair.leftDraws; ++draw) {
    DrawRecord& record = g_drawRecords[draw];
    if (record.kind == DrawContractKind::WorldAndWvp) {
      if (stereo_wvp::NearlyEqual(
              record.cameraFactor, dominantCamera, 3.0e-4)) {
        ++g_pair.eligibleDraws;
        ++dominantTrustedDraws;
      } else {
        record.kind = DrawContractKind::AuxiliaryWvp;
        ++g_pair.auxiliaryWvpDraws;
      }
      continue;
    }
    if (record.kind != DrawContractKind::WvpWithFactor)
      continue;
    float proofPatched[16] = {};
    if (!stereo_wvp::PatchWithCameraFactor(
            record.originalWvp,
            dominantCamera,
            g_pair.delta,
            proofPatched)) {
      record.kind = DrawContractKind::Invalid;
      ++g_pair.failures;
      continue;
    }
    record.cameraFactor = dominantCamera;
    ++g_pair.eligibleDraws;
  }
  g_pair.trustedCameraDraws = dominantTrustedDraws;
  return g_pair.failures == 0u;
}

ShaderContractCacheEntry* FindOrParseShader(
    IDirect3DVertexShader9* shader) {
  if (!shader)
    return nullptr;
  for (uint32_t index = 0; index < g_shaderContractCount; ++index) {
    if (g_shaderContracts[index].shader == shader)
      return &g_shaderContracts[index];
  }
  if (g_shaderContractCount >= MaxShaderContracts)
    return nullptr;

  UINT byteCount = 0;
  if (FAILED(shader->GetFunction(nullptr, &byteCount)) ||
      byteCount < 8u || byteCount > MaxShaderBytes)
    return nullptr;
  uint8_t* bytecode = new (std::nothrow) uint8_t[byteCount];
  if (!bytecode)
    return nullptr;
  UINT actualBytes = byteCount;
  const HRESULT result = shader->GetFunction(bytecode, &actualBytes);
  if (FAILED(result) || actualBytes == 0u || actualBytes > byteCount) {
    delete[] bytecode;
    return nullptr;
  }

  ShaderContractCacheEntry& entry =
      g_shaderContracts[g_shaderContractCount];
  entry.bytecodeHash = HashBytes(bytecode, actualBytes);
  entry.parseResult = ParseShaderMatrixContract(
      bytecode, actualBytes, &entry.matrices);
  delete[] bytecode;
  entry.shader = shader;
  shader->AddRef();  // Prevent COM pointer reuse from corrupting the cache key.
  ++g_shaderContractCount;
  return &entry;
}

uint64_t BuildDrawSignature(
    uint32_t apiKind,
    const uint64_t* arguments,
    size_t argumentCount,
    uint64_t shaderHash) {
  uint64_t hash = FnvOffset;
  hash = HashValue(hash, apiKind);
  hash = HashValue(hash, shaderHash);
  for (size_t index = 0; index < argumentCount; ++index)
    hash = HashValue(hash, arguments[index]);
  return hash;
}

struct PendingPatch {
  bool active = false;
  uint16_t registerIndex = 0;
  float original[16] = {};
};

bool PairThreadActive(IDirect3DDevice9* device, PairPass* pass) {
  const PairPass current =
      static_cast<PairPass>(g_pairPass.load(std::memory_order_acquire));
  if (current == PairPass::Inactive)
    return false;
  if (GetCurrentThreadId() != g_pair.renderThreadId ||
      device != g_pair.device) {
    g_foreignDrawFailures.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  if (pass)
    *pass = current;
  return true;
}

PendingPatch BeforeDraw(
    IDirect3DDevice9* device,
    uint32_t apiKind,
    const uint64_t* arguments,
    size_t argumentCount) {
  PendingPatch pending{};
  PairPass pass = PairPass::Inactive;
  if (!PairThreadActive(device, &pass))
    return pending;

  IDirect3DVertexShader9* shader = nullptr;
  if (FAILED(device->GetVertexShader(&shader))) {
    ++g_pair.failures;
    return pending;
  }
  ShaderContractCacheEntry* contract =
      shader ? FindOrParseShader(shader) : nullptr;
  if (shader)
    shader->Release();

  const uint64_t shaderHash = contract ? contract->bytecodeHash : 0u;
  const uint64_t signature =
      BuildDrawSignature(apiKind, arguments, argumentCount, shaderHash);
  uint64_t& sequenceHash =
      pass == PairPass::Left ? g_pair.leftHash : g_pair.rightHash;
  sequenceHash = HashValue(sequenceHash, signature);

  if (pass == PairPass::Left) {
    const uint32_t drawIndex = g_pair.leftDraws++;
    if (drawIndex >= MaxPairDraws) {
      ++g_pair.failures;
      return pending;
    }
    DrawRecord& record = g_drawRecords[drawIndex];
    record = {};
    record.signature = signature;
    record.shaderHash = shaderHash;
    if (!shader) {
      ++g_pair.passthroughDraws;
      return pending;
    }
    if (!contract ||
        contract->parseResult != ShaderCtabParseResult::Valid) {
      record.kind = DrawContractKind::Invalid;
      ++g_pair.failures;
      return pending;
    }
    if (!contract->matrices.hasWorldViewProjection) {
      if (contract->matrices.hasWorld) {
        record.kind = DrawContractKind::Invalid;
        ++g_pair.failures;
        return pending;
      }
      ++g_pair.passthroughDraws;
      return pending;
    }

    record.wvpRegister =
        contract->matrices.worldViewProjectionRegister;
    record.worldRegister = contract->matrices.worldRegister;
    if (FAILED(device->GetVertexShaderConstantF(
            record.wvpRegister, record.originalWvp, 4u))) {
      record.kind = DrawContractKind::Invalid;
      ++g_pair.failures;
      return pending;
    }

    if (contract->matrices.hasWorld) {
      float world[16] = {};
      stereo_wvp::Matrix4 camera{};
      if (FAILED(device->GetVertexShaderConstantF(
              record.worldRegister, world, 4u)) ||
          !stereo_wvp::DeriveCameraFactor(
              world, record.originalWvp, &camera)) {
        record.kind = DrawContractKind::Invalid;
        ++g_pair.failures;
        return pending;
      }
      record.kind = DrawContractKind::WorldAndWvp;
      record.cameraFactor = camera;
      ++g_pair.trustedCameraDraws;
    } else {
      record.kind = DrawContractKind::WvpWithFactor;
      ++g_pair.factorOnlyDraws;
    }
    return pending;
  }

  const uint32_t drawIndex = g_pair.rightDraws++;
  if (drawIndex >= g_pair.leftDraws || drawIndex >= MaxPairDraws) {
    ++g_pair.failures;
    return pending;
  }
  const DrawRecord& record = g_drawRecords[drawIndex];
  if (record.signature != signature ||
      record.shaderHash != shaderHash) {
    ++g_pair.failures;
    return pending;
  }
  if (record.kind == DrawContractKind::Passthrough ||
      record.kind == DrawContractKind::AuxiliaryWvp)
    return pending;
  if (record.kind == DrawContractKind::Invalid || !contract) {
    ++g_pair.failures;
    return pending;
  }

  float currentWvp[16] = {};
  float patchedWvp[16] = {};
  if (FAILED(device->GetVertexShaderConstantF(
          record.wvpRegister, currentWvp, 4u)) ||
      !SameFloatMatrix(currentWvp, record.originalWvp)) {
    ++g_pair.failures;
    return pending;
  }

  bool patched = false;
  if (record.kind == DrawContractKind::WorldAndWvp) {
    float currentWorld[16] = {};
    stereo_wvp::Matrix4 currentCamera{};
    patched =
        SUCCEEDED(device->GetVertexShaderConstantF(
            record.worldRegister, currentWorld, 4u)) &&
        stereo_wvp::DeriveCameraFactor(
            currentWorld, currentWvp, &currentCamera) &&
        stereo_wvp::NearlyEqual(
            currentCamera, record.cameraFactor, 3.0e-4) &&
        stereo_wvp::PatchWithWorld(
            currentWorld, currentWvp, g_pair.delta, patchedWvp);
  } else if (record.kind == DrawContractKind::WvpWithFactor) {
    patched = stereo_wvp::PatchWithCameraFactor(
        currentWvp, record.cameraFactor, g_pair.delta, patchedWvp);
  }
  if (!patched ||
      FAILED(device->SetVertexShaderConstantF(
          record.wvpRegister, patchedWvp, 4u))) {
    ++g_pair.failures;
    return pending;
  }

  pending.active = true;
  pending.registerIndex = record.wvpRegister;
  std::memcpy(pending.original, currentWvp, sizeof(currentWvp));
  ++g_pair.patchedDraws;
  return pending;
}

void AfterDraw(
    IDirect3DDevice9* device,
    const PendingPatch& pending,
    HRESULT drawResult) {
  if (g_pairPass.load(std::memory_order_relaxed) ==
          static_cast<int>(PairPass::Inactive))
    return;
  if (FAILED(drawResult))
    ++g_pair.failures;
  if (pending.active &&
      FAILED(device->SetVertexShaderConstantF(
          pending.registerIndex, pending.original, 4u)))
    ++g_pair.failures;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE primitiveType,
    UINT startVertex,
    UINT primitiveCount) {
  const uint64_t args[] = {
      static_cast<uint64_t>(primitiveType),
      startVertex,
      primitiveCount,
  };
  const PendingPatch patch = BeforeDraw(device, 1u, args, 3u);
  const HRESULT result = g_originalDrawPrimitive(
      device, primitiveType, startVertex, primitiveCount);
  AfterDraw(device, patch, result);
  return result;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex,
    UINT minimumVertexIndex,
    UINT vertexCount,
    UINT startIndex,
    UINT primitiveCount) {
  const uint64_t args[] = {
      static_cast<uint64_t>(primitiveType),
      static_cast<uint64_t>(static_cast<int64_t>(baseVertexIndex)),
      minimumVertexIndex,
      vertexCount,
      startIndex,
      primitiveCount,
  };
  const PendingPatch patch = BeforeDraw(device, 2u, args, 6u);
  const HRESULT result = g_originalDrawIndexedPrimitive(
      device, primitiveType, baseVertexIndex, minimumVertexIndex,
      vertexCount, startIndex, primitiveCount);
  AfterDraw(device, patch, result);
  return result;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUp(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexData,
    UINT vertexStride) {
  const uint64_t args[] = {
      static_cast<uint64_t>(primitiveType),
      primitiveCount,
      vertexStride,
  };
  const PendingPatch patch = BeforeDraw(device, 3u, args, 3u);
  const HRESULT result = g_originalDrawPrimitiveUp(
      device, primitiveType, primitiveCount, vertexData, vertexStride);
  AfterDraw(device, patch, result);
  return result;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUp(
    IDirect3DDevice9* device,
    D3DPRIMITIVETYPE primitiveType,
    UINT minimumVertexIndex,
    UINT vertexCount,
    UINT primitiveCount,
    const void* indexData,
    D3DFORMAT indexFormat,
    const void* vertexData,
    UINT vertexStride) {
  const uint64_t args[] = {
      static_cast<uint64_t>(primitiveType),
      minimumVertexIndex,
      vertexCount,
      primitiveCount,
      static_cast<uint64_t>(indexFormat),
      vertexStride,
  };
  const PendingPatch patch = BeforeDraw(device, 4u, args, 6u);
  const HRESULT result = g_originalDrawIndexedPrimitiveUp(
      device, primitiveType, minimumVertexIndex, vertexCount,
      primitiveCount, indexData, indexFormat, vertexData, vertexStride);
  AfterDraw(device, patch, result);
  return result;
}

HRESULT STDMETHODCALLTYPE HookDrawRectPatch(
    IDirect3DDevice9* device,
    UINT handle,
    const float* segments,
    const D3DRECTPATCH_INFO* patchInfo) {
  const uint64_t args[] = {handle};
  const PendingPatch patch = BeforeDraw(device, 5u, args, 1u);
  const HRESULT result =
      g_originalDrawRectPatch(device, handle, segments, patchInfo);
  AfterDraw(device, patch, result);
  return result;
}

HRESULT STDMETHODCALLTYPE HookDrawTriPatch(
    IDirect3DDevice9* device,
    UINT handle,
    const float* segments,
    const D3DTRIPATCH_INFO* patchInfo) {
  const uint64_t args[] = {handle};
  const PendingPatch patch = BeforeDraw(device, 6u, args, 1u);
  const HRESULT result =
      g_originalDrawTriPatch(device, handle, segments, patchInfo);
  AfterDraw(device, patch, result);
  return result;
}

bool HookDrawMethod(
    void* target,
    void* detour,
    void** original,
    const char* name) {
  if (!target || !detour || !original)
    return false;
  if (MH_CreateHook(target, detour, original) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    Log("StereoWvp: draw hook FAILED %s target=%p", name, target);
    return false;
  }
  Log("StereoWvp: draw hook ready %s target=%p", name, target);
  return true;
}

}  // namespace

bool InstallStereoDrawPatchHooks(IDirect3DDevice9* device) {
  if (g_hooksReady.load(std::memory_order_acquire)) {
    if (!device)
      return false;
    void** vtable = *reinterpret_cast<void***>(device);
    constexpr uint32_t indices[6] = {81u, 82u, 83u, 84u, 115u, 116u};
    for (size_t index = 0; index < 6u; ++index) {
      if (g_hookTargets[index] != vtable[indices[index]])
        return false;
    }
    return true;
  }
  if (!device || g_installAttempted.exchange(true))
    return false;

  void** vtable = *reinterpret_cast<void***>(device);
  constexpr uint32_t indices[6] = {81u, 82u, 83u, 84u, 115u, 116u};
  for (size_t index = 0; index < 6u; ++index)
    g_hookTargets[index] = vtable[indices[index]];

  bool ready = true;
  ready &= HookDrawMethod(
      g_hookTargets[0], reinterpret_cast<void*>(&HookDrawPrimitive),
      reinterpret_cast<void**>(&g_originalDrawPrimitive), "DrawPrimitive");
  ready &= HookDrawMethod(
      g_hookTargets[1], reinterpret_cast<void*>(&HookDrawIndexedPrimitive),
      reinterpret_cast<void**>(&g_originalDrawIndexedPrimitive),
      "DrawIndexedPrimitive");
  ready &= HookDrawMethod(
      g_hookTargets[2], reinterpret_cast<void*>(&HookDrawPrimitiveUp),
      reinterpret_cast<void**>(&g_originalDrawPrimitiveUp), "DrawPrimitiveUP");
  ready &= HookDrawMethod(
      g_hookTargets[3], reinterpret_cast<void*>(&HookDrawIndexedPrimitiveUp),
      reinterpret_cast<void**>(&g_originalDrawIndexedPrimitiveUp),
      "DrawIndexedPrimitiveUP");
  ready &= HookDrawMethod(
      g_hookTargets[4], reinterpret_cast<void*>(&HookDrawRectPatch),
      reinterpret_cast<void**>(&g_originalDrawRectPatch), "DrawRectPatch");
  ready &= HookDrawMethod(
      g_hookTargets[5], reinterpret_cast<void*>(&HookDrawTriPatch),
      reinterpret_cast<void**>(&g_originalDrawTriPatch), "DrawTriPatch");
  g_hooksReady.store(ready, std::memory_order_release);
  Log(
      "StereoWvp: six-method draw interception %s",
      ready ? "READY (inactive until Mode54 pair)" : "FAILED; Mode54 stays blank");
  return ready;
}

bool StereoDrawPatchHooksReady() {
  return g_hooksReady.load(std::memory_order_acquire);
}

bool BeginStereoDrawPatchPair(
    IDirect3DDevice9* device,
    const float rightEyeDeltaWorld[3]) {
  if (!StereoDrawPatchHooksReady() || !device || !rightEyeDeltaWorld ||
      !std::isfinite(rightEyeDeltaWorld[0]) ||
      !std::isfinite(rightEyeDeltaWorld[1]) ||
      !std::isfinite(rightEyeDeltaWorld[2]) ||
      g_pairPass.load(std::memory_order_acquire) !=
          static_cast<int>(PairPass::Inactive))
    return false;
  const double deltaLengthSquared =
      static_cast<double>(rightEyeDeltaWorld[0]) * rightEyeDeltaWorld[0] +
      static_cast<double>(rightEyeDeltaWorld[1]) * rightEyeDeltaWorld[1] +
      static_cast<double>(rightEyeDeltaWorld[2]) * rightEyeDeltaWorld[2];
  if (deltaLengthSquared < 1.0e-12)
    return false;

  g_pair = {};
  g_pair.device = device;
  g_pair.renderThreadId = GetCurrentThreadId();
  std::memcpy(g_pair.delta, rightEyeDeltaWorld, sizeof(g_pair.delta));
  g_pair.leftHash = FnvOffset;
  g_pair.rightHash = FnvOffset;
  g_foreignDrawFailures.store(0u, std::memory_order_relaxed);
  g_pairPass.store(
      static_cast<int>(PairPass::Left), std::memory_order_release);
  return true;
}

bool BeginStereoDrawPatchRightPass() {
  if (g_pairPass.load(std::memory_order_acquire) !=
          static_cast<int>(PairPass::Left) ||
      GetCurrentThreadId() != g_pair.renderThreadId ||
      g_pair.leftDraws == 0u)
    return false;
  FinalizeLeftContracts();
  g_pairPass.store(
      static_cast<int>(PairPass::Right), std::memory_order_release);
  return true;
}

StereoDrawPatchAudit EndStereoDrawPatchPair() {
  StereoDrawPatchAudit result{};
  const bool correctThread =
      GetCurrentThreadId() == g_pair.renderThreadId;
  const bool wasRight =
      g_pairPass.exchange(
          static_cast<int>(PairPass::Inactive),
          std::memory_order_acq_rel) ==
      static_cast<int>(PairPass::Right);
  result.leftDraws = g_pair.leftDraws;
  result.rightDraws = g_pair.rightDraws;
  result.eligibleDraws = g_pair.eligibleDraws;
  result.patchedDraws = g_pair.patchedDraws;
  result.factorOnlyDraws = g_pair.factorOnlyDraws;
  result.auxiliaryWvpDraws = g_pair.auxiliaryWvpDraws;
  result.passthroughDraws = g_pair.passthroughDraws;
  result.trustedCameraDraws = g_pair.trustedCameraDraws;
  result.failures =
      g_pair.failures +
      g_foreignDrawFailures.load(std::memory_order_relaxed);
  result.leftSequenceHash = g_pair.leftHash;
  result.rightSequenceHash = g_pair.rightHash;
  result.verified =
      correctThread && wasRight &&
      IsVerifiedStereoDrawPatchAudit(result);
  return result;
}

void CancelStereoDrawPatchPair() {
  g_pairPass.store(
      static_cast<int>(PairPass::Inactive), std::memory_order_release);
}

}  // namespace asi
