param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$SdkRoot = Join-Path $Root "thirdparty\openxr-sdk"
$BuildDir = Join-Path $Root "build\openxr-host-x64"
$OutputDir = Join-Path $Root "out-openxr"
$HostExe = Join-Path $OutputDir "gtaiv_xr_host.exe"

& "$PSScriptRoot\fetch-openxr-sdk.ps1" -Destination $SdkRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools with Desktop development with C++."
}

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
  throw "Visual Studio C++ x86/x64 tools were not found."
}

$buildId = (& git -C $Root rev-parse --short=12 HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $buildId) {
  $buildId = "unknown"
}
if (& git -C $Root status --porcelain --untracked-files=no) {
  $buildId = "$buildId-dirty"
}

New-Item -ItemType Directory -Force -Path $BuildDir, $OutputDir | Out-Null

& cmake `
  -S (Join-Path $Root "src\openxr_host") `
  -B $BuildDir `
  -G "Visual Studio 17 2022" `
  -A x64 `
  "-DOPENXR_SDK_ROOT=$SdkRoot" `
  "-DGTAIV_XR_BUILD_ID=$buildId"
if ($LASTEXITCODE -ne 0) {
  throw "OpenXR host CMake configure failed."
}

$pathRows = @(& cmd.exe /d /c "set path")
$hasUpperPath = @($pathRows | Where-Object { $_ -cmatch "^PATH=" }).Count -gt 0
$hasMixedPath = @($pathRows | Where-Object { $_ -cmatch "^Path=" }).Count -gt 0
if ($hasUpperPath -and $hasMixedPath) {
  # Some automation hosts inject both PATH and Path. MSBuild's .NET process
  # launcher treats those as duplicate dictionary keys and refuses to start
  # CL.exe. Remove only the duplicate uppercase entry in this child cmd.
  $buildCommand = 'set "PATH=" && cmake --build "{0}" --config "{1}" --target gtaiv_xr_host' -f `
    $BuildDir, $Configuration
  & cmd.exe /d /s /c $buildCommand
} else {
  & cmake --build $BuildDir --config $Configuration --target gtaiv_xr_host
}
if ($LASTEXITCODE -ne 0) {
  throw "OpenXR host build failed."
}

if (-not (Test-Path $HostExe)) {
  throw "Build completed without expected host: $HostExe"
}

$bytes = [IO.File]::ReadAllBytes($HostExe)
if ($bytes.Length -lt 256) {
  throw "Host executable is too small to be a valid PE file."
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length) {
  throw "Host executable has an invalid PE header offset."
}
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x8664) {
  throw ("Host has wrong PE machine 0x{0:X4}; expected x64/AMD64 0x8664." -f $machine)
}

& $HostExe --self-test
if ($LASTEXITCODE -ne 0) {
  throw "OpenXR host self-test failed. See $OutputDir\gtaiv_xr_host.log"
}

$hash = (Get-FileHash -Algorithm SHA256 $HostExe).Hash
Write-Host "OK: x64 OpenXR host built and self-tested"
Write-Host "EXE: $HostExe"
Write-Host "PE:  0x8664 (x64)"
Write-Host "SHA256: $hash"
