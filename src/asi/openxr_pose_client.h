#pragma once

#include "../bridge/gtaiv_xr_pose_bridge.h"

namespace asi
{
// Reads and validates the x64 host's fixed ABI.
void PollOpenXrPoseBridge();

// Returns the newest stable, fresh host sample. The caller gets a value copy;
// no pointer into the cross-process mapping escapes this module.
bool GetLatestOpenXrPoseBridge(gtaiv_xr_bridge::PoseBridge* output);

void ShutdownOpenXrPoseBridge();
}
