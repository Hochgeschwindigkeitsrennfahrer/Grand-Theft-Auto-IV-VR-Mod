# Build gtaiv_dxvk_vr.asi (Win32 / x86) + OpenVR
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

& "$PSScriptRoot\fetch-minhook.ps1"

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
  "src\asi\perf_debug.cpp",
  "src\asi\hooks.cpp",
  "src\asi\openvr_mono.cpp",
  "src\asi\hmd_look.cpp",
  "src\asi\hmd_pose.cpp",
  "src\asi\aob.cpp",
  "src\asi\aim_decouple.cpp",
  "src\asi\cam_matrix.cpp",
  "src\asi\ped_hide.cpp",
  "src\asi\native_invoke.cpp",
  "src\asi\vr_move.cpp",
  "src\asi\look_move.cpp",
  "src\asi\vr_display.cpp",
  "src\asi\stereo_eye.cpp",
  "src\asi\stereo_config.cpp",
  "src\asi\stereo_dual.cpp",
  "src\asi\stereo_render.cpp",
  "src\asi\stereo_proj.cpp",
  "src\asi\stereo_calib.cpp",
  "src\asi\hud_layout.cpp",
  "src\asi\game_timer.cpp",
  "src\asi\menu_draw.cpp",
  "src\asi\menu_bridge.cpp",
  "src\asi\vr_menu.cpp",
  "thirdparty\minhook\src\buffer.c",
  "thirdparty\minhook\src\hook.c",
  "thirdparty\minhook\src\trampoline.c",
  "thirdparty\minhook\src\hde\hde32.c"
)

$includes = @(
  "/I$Root\src\asi",
  "/I$Root\thirdparty\dxvk",
  "/I$Root\thirdparty\minhook\include",
  "/I$Root\thirdparty\minhook\src",
  "/I$Root\thirdparty\openvr\headers"
)

$libpath = "/LIBPATH:$Root\thirdparty\openvr\lib\win32"

$cmd = @"
call "$vcvars" x86
cd /d "$Root"
cl /nologo /EHsc /O2 /MD /W3 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE $($includes -join ' ') $($src -join ' ') /link /DLL /OUT:"$outDir\gtaiv_dxvk_vr.dll" $libpath openvr_api.lib d3d9.lib user32.lib shell32.lib gdi32.lib psapi.lib xinput.lib
if errorlevel 1 exit /b 1
copy /Y "$outDir\gtaiv_dxvk_vr.dll" "$outDir\gtaiv_dxvk_vr.asi"
copy /Y "$Root\thirdparty\openvr\bin\win32\openvr_api.dll" "$outDir\openvr_api.dll"
echo Built: $outDir\gtaiv_dxvk_vr.asi
"@

$bat = Join-Path $env:TEMP "build-gtaiv-asi.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "ASI build failed" }

Write-Host "OK: $outDir\gtaiv_dxvk_vr.asi + openvr_api.dll"
