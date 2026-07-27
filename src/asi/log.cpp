#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include <windows.h>

// std::ifstream used for buildid

namespace asi {
namespace {

std::mutex g_mu;
std::string g_path;

std::string GameDirLogPath() {
  char mod[MAX_PATH]{};
  HMODULE self = nullptr;
  GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&GameDirLogPath),
      &self);
  GetModuleFileNameA(self, mod, MAX_PATH);
  std::string p(mod);
  const auto slash = p.find_last_of("\\/");
  if (slash != std::string::npos)
    p.resize(slash + 1);
  p += "gtaiv_dxvk_vr.log";
  return p;
}

std::string GameDir() {
  std::string p = GameDirLogPath();
  const auto slash = p.find_last_of("\\/");
  if (slash != std::string::npos)
    p.resize(slash + 1);
  return p;
}

// Peek stereo file at init (mode may not be loaded into g_mode yet).
int PeekStereoModeFile() {
  std::string path = GameDir();
  path += "gtaiv_dxvk_vr.stereo";
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
    return -1;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return -1;
  int v = -1;
  if (sscanf_s(buf, "%d", &v) == 1 && v >= 0)
    return v;
  return -1;
}

void CapArchives(const char* fullPath, int keepLast) {
  // Match "{fullPath}.m*" archives (e.g. gtaiv_dxvk_vr.log.m126.20260727-143000).
  char dir[MAX_PATH]{};
  strncpy_s(dir, fullPath, _TRUNCATE);
  char* slash = strrchr(dir, '\\');
  if (!slash)
    slash = strrchr(dir, '/');
  if (!slash)
    return;
  slash[1] = 0;
  const char* baseName = fullPath + (slash - dir) + 1;

  char pattern[MAX_PATH]{};
  snprintf(pattern, sizeof(pattern), "%s%s.m*", dir, baseName);

  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  struct Entry {
    std::string path;
    FILETIME write;
  };
  std::vector<Entry> entries;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    Entry e;
    e.path = std::string(dir) + fd.cFileName;
    e.write = fd.ftLastWriteTime;
    entries.push_back(std::move(e));
  } while (FindNextFileA(h, &fd));
  FindClose(h);

  if (static_cast<int>(entries.size()) <= keepLast)
    return;

  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) {
              return CompareFileTime(&a.write, &b.write) > 0;  // newest first
            });
  for (size_t i = static_cast<size_t>(keepLast); i < entries.size(); ++i)
    DeleteFileA(entries[i].path.c_str());
}

}  // namespace

void ArchiveSessionLog(const char* fullPath) {
  if (!fullPath || !fullPath[0])
    return;

  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExA(fullPath, GetFileExInfoStandard, &fad))
    return;
  if (fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0)
    return;

  const int mode = PeekStereoModeFile();
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char stamp[32]{};
  snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u",
           static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
           static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
           static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));

  char dest[MAX_PATH + 64]{};
  if (mode >= 0)
    snprintf(dest, sizeof(dest), "%s.m%d.%s", fullPath, mode, stamp);
  else
    snprintf(dest, sizeof(dest), "%s.m0.%s", fullPath, stamp);

  // If dest exists (same-second restart), append a letter.
  if (GetFileAttributesA(dest) != INVALID_FILE_ATTRIBUTES) {
    char alt[MAX_PATH + 64]{};
    for (char c = 'a'; c <= 'z'; ++c) {
      snprintf(alt, sizeof(alt), "%s%c", dest, c);
      if (GetFileAttributesA(alt) == INVALID_FILE_ATTRIBUTES) {
        strncpy_s(dest, alt, _TRUNCATE);
        break;
      }
    }
  }

  if (MoveFileA(fullPath, dest))
    CapArchives(fullPath, 20);
}

void LogInit() {
  g_path = GameDirLogPath();
  ArchiveSessionLog(g_path.c_str());
  // Truncate each session so deploy scripts cannot match stale MonoSubmit lines.
  std::ofstream(g_path, std::ios::trunc) << "gtaiv-dxvk-vr ASI interop probe\n";

  // Optional stamp from build-deploy-run.ps1 (gtaiv_dxvk_vr.buildid next to ASI).
  char buildId[128]{};
  {
    std::string idPath = g_path;
    const auto slash = idPath.find_last_of("\\/");
    if (slash != std::string::npos)
      idPath.resize(slash + 1);
    idPath += "gtaiv_dxvk_vr.buildid";
    std::ifstream in(idPath);
    if (in)
      in.getline(buildId, sizeof(buildId));
  }

  const int peekMode = PeekStereoModeFile();
  Log("ASI loaded sizeof(void*)=%zu path=%s", sizeof(void*), g_path.c_str());
  if (buildId[0])
    Log("ASI_BUILD_ID %s", buildId);
  else
    Log("ASI_BUILD_ID (none)");
  if (peekMode >= 0)
    Log("LogArchive: session start stereo_file=%d (prior non-empty logs renamed .mN.stamp)",
        peekMode);
  else
    Log("LogArchive: session start stereo_file=(none)");
}

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  std::lock_guard<std::mutex> lock(g_mu);
  std::ofstream f(g_path, std::ios::app);
  if (f)
    f << buf << '\n';
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
}

}  // namespace asi
