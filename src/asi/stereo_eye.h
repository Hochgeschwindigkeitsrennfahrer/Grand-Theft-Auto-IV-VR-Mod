#pragma once

namespace asi {

enum class StereoEye {
  Left = 0,
  Right = 1,
};

// Which eye the next CopyMat / world draw should use (IPD offset).
void SetStereoEye(StereoEye eye);
StereoEye GetStereoEye();

}  // namespace asi
