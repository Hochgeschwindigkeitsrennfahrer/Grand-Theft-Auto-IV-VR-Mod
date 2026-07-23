#include "stereo_eye.h"

#include <atomic>

namespace asi {
namespace {
std::atomic<int> g_eye{static_cast<int>(StereoEye::Left)};
}

void SetStereoEye(StereoEye eye) {
  g_eye.store(static_cast<int>(eye));
}

StereoEye GetStereoEye() {
  return static_cast<StereoEye>(g_eye.load());
}

}  // namespace asi
