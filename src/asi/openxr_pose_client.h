#pragma once

namespace asi
{
// Reads the x64 host's fixed ABI and logs validated samples. This deliberately
// does not move the GTA camera or inject input in the first hardware gate.
void PollOpenXrPoseBridge();
void ShutdownOpenXrPoseBridge();
}
