# New Windows PC — install tools + fetch project deps for gtaiv-dxvk-vr
# Run in PowerShell (Admin recommended for winget installs):
#   cd <this-repo>
#   .\scripts\setup-dev-pc.ps1
#
# ASI build needs: Git + VS2022 C++ (x86). DXVK rebuild needs MSYS2 (optional).
# Does NOT install Steam / SteamVR / GTA IV / FusionFix (manual).

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

function Test-Cmd($Name) {
  return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Ensure-Winget {
  if (-not (Test-Cmd "winget")) {
    throw "winget not found. Install 'App Installer' from Microsoft Store, then re-open PowerShell."
  }
}

function Winget-Install([string]$Id, [string]$Name, [string]$Override = $null) {
  Write-Host ""
  Write-Host "--- Installing $Name ($Id) ---"
  $args = @("install", "--id", $Id, "-e", "--accept-package-agreements", "--accept-source-agreements")
  if ($Override) {
    $args += @("--override", $Override)
  }
  & winget @args
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
    # -1978335189 = already installed (common winget code)
    Write-Host "WARNING: winget exit $LASTEXITCODE for $Name — check manually if needed."
  } else {
    Write-Host "OK: $Name"
  }
}

Write-Host "=== gtaiv-dxvk-vr new PC setup ==="
Write-Host "Repo: $Root"
Write-Host ""

# --- Tools ---
Ensure-Winget

Write-Host "Checking Git..."
if (-not (Test-Cmd "git")) {
  Winget-Install "Git.Git" "Git for Windows"
  $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
              [System.Environment]::GetEnvironmentVariable("Path", "User")
  if (-not (Test-Cmd "git")) {
    throw "Git installed but not on PATH yet. Close PowerShell, open a NEW one, re-run this script."
  }
} else {
  Write-Host "OK: git $(git --version)"
}

Write-Host "Checking Python..."
if (-not (Test-Cmd "python") -and -not (Test-Cmd "python3")) {
  Winget-Install "Python.Python.3.12" "Python 3.12"
} else {
  Write-Host "OK: Python present"
}

Write-Host "Checking Visual Studio 2022 C++ (x86)..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$needVs = $true
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if ($vs) {
    Write-Host "OK: VS C++ tools at $vs"
    $needVs = $false
  }
}
if ($needVs) {
  Write-Host "Installing VS 2022 Community + Desktop C++ (this takes a while)..."
  Winget-Install `
    "Microsoft.VisualStudio.2022.Community" `
    "Visual Studio 2022 Community (C++)" `
    "--passive --wait --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $vs) {
      Write-Host "WARNING: VS installed but x86 C++ tools not detected yet."
      Write-Host "  Open 'Visual Studio Installer' → Modify → enable 'Desktop development with C++' → Modify."
    } else {
      Write-Host "OK: VS C++ tools at $vs"
    }
  }
}

# Optional: MSYS2 for stock/IronWolf DXVK rebuilds (ASI does not need it)
$msys = "C:\msys64\mingw32.exe"
if (-not (Test-Path $msys)) {
  Write-Host ""
  Write-Host "MSYS2 not found (optional — only if you rebuild d3d9.dll from source)."
  $ans = Read-Host "Install MSYS2 now? [y/N]"
  if ($ans -match '^[Yy]') {
    Winget-Install "MSYS2.MSYS2" "MSYS2"
    Write-Host "After MSYS2 install: open 'MSYS2 MINGW32' and run packages from docs/BUILD.md section 2."
  } else {
    Write-Host "Skipped MSYS2. Use stock DXVK 3.0.2 d3d9.dll binary for flat; ASI build still works."
  }
} else {
  Write-Host "OK: MSYS2 present"
}

# Optional Vulkan SDK (glslang for DXVK shader build)
if (-not (Test-Path "C:\VulkanSDK")) {
  Write-Host ""
  $ans = Read-Host "Install Vulkan SDK (optional, for DXVK source builds)? [y/N]"
  if ($ans -match '^[Yy]') {
    Winget-Install "LunarG.VulkanSDK" "Vulkan SDK"
  }
}

# Refresh PATH for this session
$env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
            [System.Environment]::GetEnvironmentVariable("Path", "User")

Write-Host ""
Write-Host "=== Fetch project dependencies ==="
& "$PSScriptRoot\fetch-deps.ps1"

Write-Host ""
Write-Host "=== Tool summary ==="
$summary = @()
$summary += "git:     $(if (Test-Cmd 'git') { (git --version) } else { 'MISSING' })"
$vsOk = $false
if (Test-Path $vswhere) {
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  $vsOk = [bool]$vs
}
$summary += "VS x86:  $(if ($vsOk) { 'OK' } else { 'MISSING — install Desktop development with C++' })"
$summary += "MinHook: $(if (Test-Path (Join-Path $Root 'thirdparty\minhook\include\MinHook.h')) { 'OK' } else { 'MISSING' })"
$summary += "OpenVR:  $(if (Test-Path (Join-Path $Root 'thirdparty\openvr\headers\openvr.h')) { 'OK v2.12.14 expected' } else { 'MISSING' })"
$summary += "dxvk/:   $(if (Test-Path (Join-Path $Root 'dxvk\src')) { 'OK submodule' } else { 'MISSING' })"
$summary | ForEach-Object { Write-Host "  $_" }

Write-Host ""
Write-Host "=== Still install manually (game / VR) ==="
Write-Host "  1. Steam + SteamVR"
Write-Host "  2. GTA IV Complete Edition"
Write-Host "  3. FusionFix — Graphics API = DirectX 9 when our d3d9.dll is active"
Write-Host "  4. Stock DXVK 3.0.2 d3d9.dll (x86) in the GTAIV folder"
Write-Host "  5. Cursor: File → Open Folder → this repo"
Write-Host ""
Write-Host "=== Build ASI now ==="
Write-Host "  .\scripts\build-asi.ps1"
Write-Host "  Expected: out-asi\gtaiv_dxvk_vr.asi + openvr_api.dll"
Write-Host ""
Write-Host "Done."
