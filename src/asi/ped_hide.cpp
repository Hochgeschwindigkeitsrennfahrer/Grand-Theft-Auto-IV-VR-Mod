#include "ped_hide.h"

namespace asi {

// Intentionally empty: calling script natives from EndScene crashes CE.
// Head interior is mitigated via cam_matrix eye-forward offset instead.
void UpdatePedHeadHide() {}

}  // namespace asi
