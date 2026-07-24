#include "ped_hide.h"
#include "log.h"

#include <atomic>

namespace asi {

// Historic (2026-07-23): SET_DRAW_PLAYER_COMPONENT / SET_CHAR_COMPONENT_VARIATION
// from EndScene DID run (log showed PedHide) then CRASHED — natives need the
// script thread, not the D3D/render path. CopyMat is also not a script thread.
// Safe partial: cam_matrix eye-forward (~42 cm) places the camera past skull/hair.
// Real mesh hide needs ScriptHook / a script-thread pump later — do not re-enable
// natives here without that.
void UpdatePedHeadHide() {
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true))
    Log("PedHide: OFF (natives from EndScene/CopyMat crash CE) — use eyeForward past hair; "
        "script-thread hide later");
}

}  // namespace asi
