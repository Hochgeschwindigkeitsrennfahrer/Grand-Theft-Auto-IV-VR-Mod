#pragma once

namespace asi {

// Hide player head/hair/teeth/face via SET_CHAR_COMPONENT_VARIATION (-1).
// Call each VR frame while FP cam is active (game may restore components).
void UpdatePedHeadHide();

}  // namespace asi
