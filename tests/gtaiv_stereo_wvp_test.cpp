#include "../src/asi/shader_ctab.h"
#include "../src/asi/stereo_draw_patch.h"
#include "../src/asi/stereo_wvp_math.h"
#include "../src/bridge/gtaiv_xr_fov_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using asi::stereo_wvp::Matrix4;

bool Near(double left, double right, double tolerance = 1.0e-5) {
  return std::fabs(left - right) <= tolerance;
}

void StoreFloat(const Matrix4& matrix, float output[16]) {
  for (size_t index = 0; index < 16; ++index)
    output[index] = static_cast<float>(matrix.value[index]);
}

void TransformPoint(const double point[4],
                    const Matrix4& matrix,
                    double output[4]) {
  for (size_t column = 0; column < 4; ++column) {
    output[column] =
        point[0] * matrix.value[column] +
        point[1] * matrix.value[4 + column] +
        point[2] * matrix.value[8 + column] +
        point[3] * matrix.value[12 + column];
  }
}

bool SamePoint(const double left[4], const double right[4]) {
  for (size_t index = 0; index < 4; ++index) {
    const double scale =
        (std::max)(1.0, (std::max)(std::fabs(left[index]),
                                  std::fabs(right[index])));
    if (!Near(left[index], right[index], 5.0e-6 * scale))
      return false;
  }
  return true;
}

Matrix4 TestCamera() {
  Matrix4 camera = asi::stereo_wvp::Identity();
  camera.value[0] = 1.31;
  camera.value[1] = 0.07;
  camera.value[5] = 1.57;
  camera.value[6] = -0.11;
  camera.value[8] = 0.13;
  camera.value[10] = 0.91;
  camera.value[11] = 0.2;
  camera.value[12] = -4.0;
  camera.value[13] = 2.5;
  camera.value[14] = 7.0;
  camera.value[15] = 1.1;
  return camera;
}

bool TestWorldPatch(const Matrix4& world, const float delta[3]) {
  const Matrix4 camera = TestCamera();
  const Matrix4 original = asi::stereo_wvp::Multiply(world, camera);
  float worldValues[16] = {};
  float originalValues[16] = {};
  float patchedValues[16] = {};
  StoreFloat(world, worldValues);
  StoreFloat(original, originalValues);
  if (!asi::stereo_wvp::PatchWithWorld(
          worldValues, originalValues, delta, patchedValues))
    return false;
  Matrix4 patched{};
  if (!asi::stereo_wvp::FromFloat(patchedValues, &patched))
    return false;
  const Matrix4 expected = asi::stereo_wvp::Multiply(
      asi::stereo_wvp::Multiply(
          world,
          asi::stereo_wvp::Translation(-delta[0], -delta[1], -delta[2])),
      camera);
  if (!asi::stereo_wvp::NearlyEqual(patched, expected, 2.0e-5))
    return false;
  const double objectPoint[4] = {2.0, -3.0, 0.75, 1.0};
  double actualPoint[4] = {};
  double expectedPoint[4] = {};
  TransformPoint(objectPoint, patched, actualPoint);
  TransformPoint(objectPoint, expected, expectedPoint);
  return SamePoint(actualPoint, expectedPoint);
}

bool TestIdentityWorld() {
  const float delta[3] = {0.064f, 0.0f, 0.0f};
  return TestWorldPatch(asi::stereo_wvp::Identity(), delta);
}

bool TestRotatedTranslatedWorld() {
  Matrix4 world = asi::stereo_wvp::Identity();
  const double cosine = std::cos(0.71);
  const double sine = std::sin(0.71);
  world.value[0] = cosine;
  world.value[1] = sine;
  world.value[4] = -sine;
  world.value[5] = cosine;
  world.value[12] = 125.0;
  world.value[13] = -44.0;
  world.value[14] = 9.0;
  const float delta[3] = {0.041f, -0.032f, 0.012f};
  return TestWorldPatch(world, delta);
}

bool TestFactorOnlyPatch() {
  Matrix4 world = asi::stereo_wvp::Identity();
  world.value[0] = 0.75;
  world.value[5] = 1.25;
  world.value[10] = 1.1;
  world.value[12] = 12.0;
  world.value[13] = -8.0;
  world.value[14] = 2.0;
  const Matrix4 camera = TestCamera();
  const Matrix4 original = asi::stereo_wvp::Multiply(world, camera);
  float originalValues[16] = {};
  float patchedValues[16] = {};
  StoreFloat(original, originalValues);
  const float delta[3] = {-0.052f, 0.011f, 0.003f};
  if (!asi::stereo_wvp::PatchWithCameraFactor(
          originalValues, camera, delta, patchedValues))
    return false;
  Matrix4 patched{};
  if (!asi::stereo_wvp::FromFloat(patchedValues, &patched))
    return false;
  const Matrix4 expected = asi::stereo_wvp::Multiply(
      asi::stereo_wvp::Multiply(
          world,
          asi::stereo_wvp::Translation(-delta[0], -delta[1], -delta[2])),
      camera);
  return asi::stereo_wvp::NearlyEqual(patched, expected, 2.0e-5);
}

bool TestSingularAndNanReject() {
  Matrix4 singular{};
  Matrix4 inverse{};
  if (asi::stereo_wvp::Invert(singular, &inverse))
    return false;
  float world[16] = {};
  float wvp[16] = {};
  StoreFloat(asi::stereo_wvp::Identity(), world);
  StoreFloat(TestCamera(), wvp);
  world[5] = std::numeric_limits<float>::quiet_NaN();
  float patched[16] = {};
  const float delta[3] = {0.064f, 0.0f, 0.0f};
  return !asi::stereo_wvp::PatchWithWorld(
      world, wvp, delta, patched);
}

bool TestLargeTranslationResidual() {
  Matrix4 world = asi::stereo_wvp::Identity();
  world.value[12] = 250000.0;
  world.value[13] = -175000.0;
  world.value[14] = 90000.0;
  const float delta[3] = {0.063f, -0.004f, 0.002f};
  return TestWorldPatch(world, delta);
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool TestSyntheticCtab() {
  constexpr size_t TableBytes = 132u;
  std::vector<uint8_t> table(TableBytes, 0u);
  WriteU32(table, 0u, 28u);
  WriteU32(table, 8u, 0xfffe0300u);
  WriteU32(table, 12u, 2u);
  WriteU32(table, 16u, 28u);

  constexpr uint32_t TypeWorld = 68u;
  constexpr uint32_t TypeWvp = 84u;
  constexpr uint32_t NameWorld = 100u;
  constexpr uint32_t NameWvp = 108u;
  WriteU32(table, 28u, NameWorld);
  WriteU16(table, 32u, 2u);
  WriteU16(table, 34u, 0u);
  WriteU16(table, 36u, 4u);
  WriteU32(table, 40u, TypeWorld);
  WriteU32(table, 48u, NameWvp);
  WriteU16(table, 52u, 2u);
  WriteU16(table, 54u, 8u);
  WriteU16(table, 56u, 4u);
  WriteU32(table, 60u, TypeWvp);
  for (uint32_t typeOffset : {TypeWorld, TypeWvp}) {
    WriteU16(table, typeOffset, 2u);
    WriteU16(table, typeOffset + 2u, 3u);
    WriteU16(table, typeOffset + 4u, 4u);
    WriteU16(table, typeOffset + 6u, 4u);
    WriteU16(table, typeOffset + 8u, 1u);
  }
  std::memcpy(table.data() + NameWorld, "gWorld", 7u);
  std::memcpy(table.data() + NameWvp, "gWorldViewProj", 15u);

  const uint32_t paddedTableBytes =
      static_cast<uint32_t>((TableBytes + 3u) & ~3u);
  const uint32_t commentWords = 1u + paddedTableBytes / 4u;
  std::vector<uint8_t> shader(
      12u + paddedTableBytes + 4u, 0u);
  WriteU32(shader, 0u, 0xfffe0300u);
  WriteU32(shader, 4u, (commentWords << 16u) | 0xfffeu);
  WriteU32(shader, 8u, 0x42415443u);
  std::memcpy(shader.data() + 12u, table.data(), table.size());
  WriteU32(shader, 12u + paddedTableBytes, 0x0000ffffu);

  asi::ShaderMatrixContract contract{};
  if (asi::ParseShaderMatrixContract(
          shader.data(), shader.size(), &contract) !=
          asi::ShaderCtabParseResult::Valid ||
      !contract.hasWorld || !contract.hasWorldViewProjection ||
      contract.worldRegister != 0u ||
      contract.worldViewProjectionRegister != 8u)
    return false;

  shader[12u + TypeWvp + 4u] = 3u;
  return asi::ParseShaderMatrixContract(
             shader.data(), shader.size(), &contract) ==
         asi::ShaderCtabParseResult::Malformed;
}

bool TestPairAuditRejectsLooseStereo() {
  asi::StereoDrawPatchAudit audit{};
  audit.leftDraws = 90u;
  audit.rightDraws = 90u;
  audit.eligibleDraws = 64u;
  audit.patchedDraws = 64u;
  audit.trustedCameraDraws = 50u;
  audit.leftSequenceHash = 0x1234u;
  audit.rightSequenceHash = 0x1234u;
  if (!asi::IsVerifiedStereoDrawPatchAudit(audit))
    return false;

  asi::StereoDrawPatchAudit rejected = audit;
  rejected.rightDraws = 89u;
  if (asi::IsVerifiedStereoDrawPatchAudit(rejected))
    return false;
  rejected = audit;
  rejected.rightSequenceHash ^= 1u;
  if (asi::IsVerifiedStereoDrawPatchAudit(rejected))
    return false;
  rejected = audit;
  --rejected.patchedDraws;
  if (asi::IsVerifiedStereoDrawPatchAudit(rejected))
    return false;
  rejected = audit;
  rejected.trustedCameraDraws = 0u;
  if (asi::IsVerifiedStereoDrawPatchAudit(rejected))
    return false;
  rejected = audit;
  rejected.failures = 1u;
  return !asi::IsVerifiedStereoDrawPatchAudit(rejected);
}

bool TestOpenXrCoverFovMath() {
  gtaiv_xr_bridge::EyeFov fovs[2] = {
      {-0.9425f, 0.6981f, 0.7679f, -0.9599f},
      {-0.6981f, 0.9425f, 0.7679f, -0.9599f},
  };
  float horizontal = 0.0f;
  float vertical = 0.0f;
  if (!gtaiv_xr_bridge::ComputeOpenXrCoverTangents(
          fovs, &horizontal, &vertical) ||
      !Near(horizontal, std::tan(0.9425), 1.0e-5) ||
      !Near(vertical, -std::tan(-0.9599), 1.0e-5)) {
    return false;
  }

  fovs[0].angleLeft = std::numeric_limits<float>::quiet_NaN();
  return !gtaiv_xr_bridge::ComputeOpenXrCoverTangents(
      fovs, &horizontal, &vertical);
}

}  // namespace

int main() {
  struct Test {
    const char* name;
    bool (*run)();
  };
  const Test tests[] = {
      {"identity-world", &TestIdentityWorld},
      {"rotated-translated-world", &TestRotatedTranslatedWorld},
      {"factor-only", &TestFactorOnlyPatch},
      {"singular-nan-reject", &TestSingularAndNanReject},
      {"large-translation", &TestLargeTranslationResidual},
      {"synthetic-ctab", &TestSyntheticCtab},
      {"strict-pair-audit", &TestPairAuditRejectsLooseStereo},
      {"openxr-cover-fov", &TestOpenXrCoverFovMath},
  };
  for (const Test& test : tests) {
    if (!test.run()) {
      std::fprintf(stderr, "StereoWvpTest: FAIL %s\n", test.name);
      return 1;
    }
  }
  std::printf(
      "StereoWvpTest: PASS math=%u ctab=%u pairAudit=%u "
      "openxrFov=%u runtimeUntouched=1\n",
      5u,
      1u,
      1u,
      1u);
  return 0;
}
