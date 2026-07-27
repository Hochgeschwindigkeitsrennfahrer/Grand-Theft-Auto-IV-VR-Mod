# Build gtaiv_dxvk_vr.asi (Win32 / x86) + OpenVR
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

& "$PSScriptRoot\fetch-minhook.ps1"
& "$PSScriptRoot\fetch-vulkan-headers.ps1"

if (-not (Test-Path "$Root\thirdparty\openvr\headers\openvr.h")) {
  throw "OpenVR missing - run: git clone --depth 1 https://github.com/ValveSoftware/openvr.git thirdparty/openvr"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VS with C++ x86 tools not found" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
$outDir = Join-Path $Root "out-asi"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$src = @(
  "src\asi\dllmain.cpp",
  "src\asi\log.cpp",
  "src\asi\hooks.cpp",
  "src\asi\openxr_bridge.cpp",
  "src\asi\openxr_controller.cpp",
  "src\asi\openxr_pose_client.cpp",
  "src\asi\openvr_mono.cpp",
  "src\asi\hmd_look.cpp",
  "src\asi\hmd_pose.cpp",
  "src\asi\aob.cpp",
  "src\asi\cam_matrix.cpp",
  "src\asi\ped_hide.cpp",
  "src\asi\vr_move.cpp",
  "src\asi\vr_display.cpp",
  "src\asi\stereo_eye.cpp",
  "src\asi\stereo_config.cpp",
  "src\asi\stereo_draw_patch.cpp",
  "src\asi\stereo_render.cpp",
  "src\asi\stereo_proj.cpp",
  "src\asi\ui_state.cpp",
  "thirdparty\minhook\src\buffer.c",
  "thirdparty\minhook\src\hook.c",
  "thirdparty\minhook\src\trampoline.c",
  "thirdparty\minhook\src\hde\hde32.c"
)

$includes = @(
  "/I$Root\src\asi",
  "/I$Root\thirdparty\dxvk",
  "/I$Root\thirdparty\vulkan-headers\include",
  "/I$Root\thirdparty\minhook\include",
  "/I$Root\thirdparty\minhook\src",
  "/I$Root\thirdparty\openvr\headers"
)

$libpath = "/LIBPATH:$Root\thirdparty\openvr\lib\win32"

$cmd = @"
call "$vcvars" x86
cd /d "$Root"
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++17 tests\gtaiv_stereo_wvp_test.cpp /Fo:"$outDir\gtaiv_stereo_wvp_test.obj" /Fe:"$outDir\gtaiv_stereo_wvp_test.exe"
if errorlevel 1 exit /b 1
"$outDir\gtaiv_stereo_wvp_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /MD /W3 /std:c++17 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE $($includes -join ' ') $($src -join ' ') /link /DLL /OUT:"$outDir\gtaiv_dxvk_vr.dll" $libpath /DELAYLOAD:openvr_api.dll openvr_api.lib delayimp.lib d3d9.lib d3d11.lib dxgi.lib user32.lib shell32.lib psapi.lib xinput.lib
if errorlevel 1 exit /b 1
copy /Y "$outDir\gtaiv_dxvk_vr.dll" "$outDir\gtaiv_dxvk_vr.asi"
copy /Y "$Root\thirdparty\openvr\bin\win32\openvr_api.dll" "$outDir\openvr_api.dll"
echo Built: $outDir\gtaiv_dxvk_vr.asi
"@

$bat = Join-Path $env:TEMP "build-gtaiv-asi.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "ASI build failed" }

$AsiPath = Join-Path $outDir "gtaiv_dxvk_vr.asi"
$bytes = [IO.File]::ReadAllBytes($AsiPath)
if ($bytes.Length -lt 256) {
  throw "Built ASI is not a valid PE file: $AsiPath"
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x014c) {
  throw ("Built ASI machine is 0x{0:X4}; expected x86 0x014C." -f $machine)
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $AsiPath).Hash

Write-Host "OK: $AsiPath + openvr_api.dll"
Write-Host ("PE:  0x{0:X4} (x86)" -f $machine)
Write-Host "SHA256: $hash"
