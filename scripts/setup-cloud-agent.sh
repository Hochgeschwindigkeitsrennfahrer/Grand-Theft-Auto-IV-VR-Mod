#!/usr/bin/env bash
# Cloud / Linux agent bootstrap for gtaiv-dxvk-vr.
# Fetches thirdparty deps (gitignored) + optional apt packages for reading/building DXVK.
# Does NOT build the Win32 ASI (needs MSVC on Windows).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== gtaiv-dxvk-vr cloud agent setup ==="
echo "Repo: $ROOT"

install_apt() {
  if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get not available — skip package install"
    return 0
  fi
  echo "--- apt packages (dev helpers) ---"
  sudo apt-get update -y
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    git \
    python3 \
    python3-pip \
    meson \
    ninja-build \
    pkg-config \
    glslang-tools \
    mingw-w64 \
    g++-mingw-w64-i686 \
    gcc-mingw-w64-i686 \
    cmake \
    curl \
    ca-certificates \
    ripgrep \
    || echo "WARNING: some apt packages failed — continuing with deps fetch"
}

fetch_submodule() {
  echo "--- DXVK submodule ---"
  git submodule sync --recursive
  git submodule update --init --recursive
  if [[ ! -d dxvk/src ]]; then
    echo "ERROR: dxvk/src missing after submodule update"
    exit 1
  fi
  if [[ ! -f dxvk/src/d3d9/d3d9_vr.h ]]; then
    echo "ERROR: d3d9_vr.h missing — wrong DXVK branch (need tiw-rel)"
    exit 1
  fi
  echo "OK: dxvk submodule"
}

fetch_minhook() {
  echo "--- MinHook ---"
  if [[ -f thirdparty/minhook/include/MinHook.h ]]; then
    echo "OK: MinHook already present"
    return 0
  fi
  git clone --depth 1 https://github.com/TsudaKageyu/minhook.git thirdparty/minhook
  echo "OK: MinHook"
}

fetch_openvr() {
  local tag="v2.12.14"
  echo "--- OpenVR $tag ---"
  if [[ -f thirdparty/openvr/headers/openvr.h ]] && \
     [[ -f thirdparty/openvr/bin/win32/openvr_api.dll ]]; then
    local cur
    cur="$(git -C thirdparty/openvr describe --tags --exact-match HEAD 2>/dev/null || true)"
    if [[ "$cur" == "$tag" ]]; then
      echo "OK: OpenVR $tag already present"
      return 0
    fi
    echo "OpenVR present but not $tag — re-cloning"
    rm -rf thirdparty/openvr
  fi
  git clone --depth 1 --branch "$tag" https://github.com/ValveSoftware/openvr.git thirdparty/openvr
  echo "OK: OpenVR $tag"
}

verify() {
  echo "--- Verify ---"
  local fail=0
  for p in \
    dxvk/src/d3d9/d3d9_vr.h \
    thirdparty/minhook/include/MinHook.h \
    thirdparty/openvr/headers/openvr.h \
    thirdparty/openvr/bin/win32/openvr_api.dll \
    thirdparty/openvr/lib/win32/openvr_api.lib \
    thirdparty/dxvk/d3d9_vk_interop.h
  do
    if [[ -f "$p" ]]; then
      echo "  OK  $p"
    else
      echo "  MISS $p"
      fail=1
    fi
  done
  if [[ "$fail" -ne 0 ]]; then
    echo "ERROR: missing dependencies"
    exit 1
  fi
  echo "All cloud deps ready."
  echo "Note: ASI binary build still requires Windows + VS2022 (scripts/build-asi.ps1)."
}

# Allow: SKIP_APT=1 ./scripts/setup-cloud-agent.sh
if [[ "${SKIP_APT:-0}" != "1" ]]; then
  install_apt
else
  echo "SKIP_APT=1 — not installing packages"
fi

fetch_submodule
fetch_minhook
fetch_openvr
verify
