#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <limits>

namespace asi {
namespace stereo_wvp {

// GTA's compiled vertex shaders use row vectors:
//   clip = objectPosition * gWorld * viewProjection
// Matrices below are therefore row-major and multiply in that order.
struct Matrix4 {
  double value[16] = {};
};

inline Matrix4 Identity() {
  Matrix4 result{};
  result.value[0] = 1.0;
  result.value[5] = 1.0;
  result.value[10] = 1.0;
  result.value[15] = 1.0;
  return result;
}

inline bool IsFinite(const Matrix4& matrix) {
  for (double value : matrix.value) {
    if (!std::isfinite(value))
      return false;
  }
  return true;
}

inline bool FromFloat(const float source[16], Matrix4* output) {
  if (!source || !output)
    return false;
  for (size_t index = 0; index < 16; ++index) {
    if (!std::isfinite(source[index]))
      return false;
    output->value[index] = static_cast<double>(source[index]);
  }
  return true;
}

inline bool ToFloat(const Matrix4& source, float output[16]) {
  if (!output || !IsFinite(source))
    return false;
  for (size_t index = 0; index < 16; ++index) {
    const double value = source.value[index];
    if (std::fabs(value) > static_cast<double>(FLT_MAX))
      return false;
    output[index] = static_cast<float>(value);
  }
  return true;
}

inline Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
  Matrix4 result{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      double sum = 0.0;
      for (size_t inner = 0; inner < 4; ++inner) {
        sum += left.value[row * 4 + inner] *
               right.value[inner * 4 + column];
      }
      result.value[row * 4 + column] = sum;
    }
  }
  return result;
}

inline double MaxAbs(const Matrix4& matrix) {
  double result = 0.0;
  for (double value : matrix.value)
    result = (std::max)(result, std::fabs(value));
  return result;
}

inline double MaxAbsDifference(const Matrix4& left, const Matrix4& right) {
  double result = 0.0;
  for (size_t index = 0; index < 16; ++index) {
    result =
        (std::max)(result, std::fabs(left.value[index] - right.value[index]));
  }
  return result;
}

inline bool NearlyEqual(const Matrix4& left,
                        const Matrix4& right,
                        double relativeTolerance = 2.0e-4) {
  if (!IsFinite(left) || !IsFinite(right))
    return false;
  const double scale = (std::max)(1.0, (std::max)(MaxAbs(left), MaxAbs(right)));
  return MaxAbsDifference(left, right) <= relativeTolerance * scale;
}

inline double InverseResidual(const Matrix4& matrix, const Matrix4& inverse) {
  const Matrix4 identity = Identity();
  return (std::max)(
      MaxAbsDifference(Multiply(matrix, inverse), identity),
      MaxAbsDifference(Multiply(inverse, matrix), identity));
}

inline bool Invert(const Matrix4& matrix,
                   Matrix4* output,
                   double* residual = nullptr) {
  if (!output || !IsFinite(matrix))
    return false;

  double augmented[4][8] = {};
  double inputScale = 0.0;
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      augmented[row][column] = matrix.value[row * 4 + column];
      inputScale =
          (std::max)(inputScale, std::fabs(augmented[row][column]));
    }
    augmented[row][row + 4] = 1.0;
  }
  if (inputScale == 0.0)
    return false;

  const double pivotFloor =
      (std::max)(1.0, inputScale) *
      std::numeric_limits<double>::epsilon() * 256.0;
  for (size_t column = 0; column < 4; ++column) {
    size_t pivotRow = column;
    double pivotMagnitude = std::fabs(augmented[column][column]);
    for (size_t row = column + 1; row < 4; ++row) {
      const double candidate = std::fabs(augmented[row][column]);
      if (candidate > pivotMagnitude) {
        pivotMagnitude = candidate;
        pivotRow = row;
      }
    }
    if (!std::isfinite(pivotMagnitude) || pivotMagnitude <= pivotFloor)
      return false;
    if (pivotRow != column) {
      for (size_t entry = 0; entry < 8; ++entry)
        (std::swap)(augmented[column][entry], augmented[pivotRow][entry]);
    }

    const double pivot = augmented[column][column];
    for (size_t entry = 0; entry < 8; ++entry)
      augmented[column][entry] /= pivot;
    for (size_t row = 0; row < 4; ++row) {
      if (row == column)
        continue;
      const double factor = augmented[row][column];
      for (size_t entry = 0; entry < 8; ++entry)
        augmented[row][entry] -= factor * augmented[column][entry];
    }
  }

  Matrix4 inverse{};
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column)
      inverse.value[row * 4 + column] = augmented[row][column + 4];
  }
  if (!IsFinite(inverse))
    return false;

  const double measuredResidual = InverseResidual(matrix, inverse);
  const double conditionScale =
      (std::max)(1.0, MaxAbs(matrix) * MaxAbs(inverse));
  if (!std::isfinite(measuredResidual) ||
      measuredResidual > 1.0e-9 * conditionScale)
    return false;
  if (residual)
    *residual = measuredResidual;
  *output = inverse;
  return true;
}

inline Matrix4 Translation(double x, double y, double z) {
  Matrix4 result = Identity();
  result.value[12] = x;
  result.value[13] = y;
  result.value[14] = z;
  return result;
}

inline bool IsAffineRowVector(const Matrix4& matrix) {
  if (!IsFinite(matrix))
    return false;
  const double tolerance = 5.0e-4 * (std::max)(1.0, MaxAbs(matrix));
  return std::fabs(matrix.value[3]) <= tolerance &&
         std::fabs(matrix.value[7]) <= tolerance &&
         std::fabs(matrix.value[11]) <= tolerance &&
         std::fabs(matrix.value[15] - 1.0) <= tolerance;
}

inline bool DeriveCameraFactor(const float worldValues[16],
                               const float worldViewProjectionValues[16],
                               Matrix4* cameraFactor) {
  if (!cameraFactor)
    return false;
  Matrix4 world{};
  Matrix4 worldViewProjection{};
  Matrix4 inverseWorld{};
  if (!FromFloat(worldValues, &world) ||
      !FromFloat(worldViewProjectionValues, &worldViewProjection) ||
      !IsAffineRowVector(world) || !Invert(world, &inverseWorld))
    return false;
  const Matrix4 camera = Multiply(inverseWorld, worldViewProjection);
  if (!IsFinite(camera) ||
      !NearlyEqual(Multiply(world, camera), worldViewProjection, 5.0e-5))
    return false;
  *cameraFactor = camera;
  return true;
}

inline bool PatchWithWorld(const float worldValues[16],
                           const float worldViewProjectionValues[16],
                           const float rightEyeDeltaWorld[3],
                           float output[16],
                           Matrix4* derivedCameraFactor = nullptr) {
  if (!rightEyeDeltaWorld || !output ||
      !std::isfinite(rightEyeDeltaWorld[0]) ||
      !std::isfinite(rightEyeDeltaWorld[1]) ||
      !std::isfinite(rightEyeDeltaWorld[2]))
    return false;
  const double deltaLengthSquared =
      static_cast<double>(rightEyeDeltaWorld[0]) * rightEyeDeltaWorld[0] +
      static_cast<double>(rightEyeDeltaWorld[1]) * rightEyeDeltaWorld[1] +
      static_cast<double>(rightEyeDeltaWorld[2]) * rightEyeDeltaWorld[2];
  if (deltaLengthSquared < 1.0e-12)
    return false;

  Matrix4 world{};
  Matrix4 original{};
  Matrix4 camera{};
  if (!FromFloat(worldValues, &world) ||
      !FromFloat(worldViewProjectionValues, &original) ||
      !DeriveCameraFactor(worldValues, worldViewProjectionValues, &camera))
    return false;

  // Moving the camera by +delta means translating world coordinates by -delta
  // before the existing view/projection factor.
  const Matrix4 eyeTranslation =
      Translation(-rightEyeDeltaWorld[0],
                  -rightEyeDeltaWorld[1],
                  -rightEyeDeltaWorld[2]);
  const Matrix4 patched =
      Multiply(Multiply(world, eyeTranslation), camera);
  if (!IsFinite(patched) ||
      MaxAbsDifference(patched, original) <= 1.0e-9)
    return false;
  if (derivedCameraFactor)
    *derivedCameraFactor = camera;
  return ToFloat(patched, output);
}

inline bool PatchWithCameraFactor(
    const float worldViewProjectionValues[16],
    const Matrix4& cameraFactor,
    const float rightEyeDeltaWorld[3],
    float output[16]) {
  if (!rightEyeDeltaWorld || !output || !IsFinite(cameraFactor) ||
      !std::isfinite(rightEyeDeltaWorld[0]) ||
      !std::isfinite(rightEyeDeltaWorld[1]) ||
      !std::isfinite(rightEyeDeltaWorld[2]))
    return false;
  const double deltaLengthSquared =
      static_cast<double>(rightEyeDeltaWorld[0]) * rightEyeDeltaWorld[0] +
      static_cast<double>(rightEyeDeltaWorld[1]) * rightEyeDeltaWorld[1] +
      static_cast<double>(rightEyeDeltaWorld[2]) * rightEyeDeltaWorld[2];
  if (deltaLengthSquared < 1.0e-12)
    return false;

  Matrix4 original{};
  Matrix4 inverseCamera{};
  if (!FromFloat(worldViewProjectionValues, &original) ||
      !Invert(cameraFactor, &inverseCamera))
    return false;
  const Matrix4 inferredWorld = Multiply(original, inverseCamera);
  if (!IsAffineRowVector(inferredWorld) ||
      !NearlyEqual(Multiply(inferredWorld, cameraFactor), original, 5.0e-5))
    return false;

  const Matrix4 eyeTranslation =
      Translation(-rightEyeDeltaWorld[0],
                  -rightEyeDeltaWorld[1],
                  -rightEyeDeltaWorld[2]);
  const Matrix4 patched =
      Multiply(Multiply(inferredWorld, eyeTranslation), cameraFactor);
  if (!IsFinite(patched) ||
      MaxAbsDifference(patched, original) <= 1.0e-9)
    return false;
  return ToFloat(patched, output);
}

}  // namespace stereo_wvp
}  // namespace asi
