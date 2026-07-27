#pragma once

namespace asi
{
// Installs a virtual XInput controller backed by fresh OpenXR action samples.
// Missing/stale OpenXR input always falls through to the real XInput device.
bool InstallOpenXrControllerHooks();

// Refreshes the x86 -> x64 vibration heartbeat once per GTA frame.
void PumpOpenXrControllerBridge();

// Marks haptics stopped and releases the named mapping during process teardown.
void ShutdownOpenXrControllerBridge();
}
