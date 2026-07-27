#pragma once

namespace asi {

void LogInit();
void Log(const char* fmt, ...);

// Before truncating a session log: if path exists and is non-empty, rename to
// "{path}.m{mode}.{YYYYMMDD-HHMMSS}". Mode peeked from gtaiv_dxvk_vr.stereo (−1 → m0).
// Keeps the newest 20 archives matching "{path}.m*".
void ArchiveSessionLog(const char* fullPath);

}  // namespace asi
