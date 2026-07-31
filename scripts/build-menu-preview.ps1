# Build tools/menu_preview — the offline render test for the settings overlay.
#
# Renders the real overlay on a plain D3D9 device and writes a BMP, so the panel
# can be checked without launching GTA IV (and without a headset).
#
#   .\scripts\build-menu-preview.ps1
#   .\out-preview\menu_preview.exe 1920 1080 out-preview\menu_preview.bmp

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

$vsInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = Join-Path $vsInstaller "vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VS with C++ x86 tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"

$outDir = Join-Path $Root "out-preview"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$src = @(
  "tools\menu_preview\menu_preview.cpp",
  "tools\menu_preview\menu_preview_stubs.cpp",
  "src\asi\menu_draw.cpp",
  "src\asi\menu_bridge.cpp",
  "src\asi\vr_menu.cpp"
)

$includes = @(
  "/I$Root\src\asi",
  "/I$Root\thirdparty\openvr\headers"
)

$cmd = @"
set "PATH=$vsInstaller;%PATH%"
call "$vcvars" x86
cd /d "$Root"
cl /nologo /EHsc /O2 /MD /W3 /DWIN32 /D_WINDOWS $($includes -join ' ') $($src -join ' ') /Fo"$outDir\\" /Fe"$outDir\menu_preview.exe" /link /LIBPATH:"$Root\thirdparty\openvr\lib\win32" openvr_api.lib d3d9.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
copy /Y "$Root\thirdparty\openvr\bin\win32\openvr_api.dll" "$outDir\openvr_api.dll"
echo Built: $outDir\menu_preview.exe
"@

$bat = Join-Path $env:TEMP "build-menu-preview.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "menu_preview build failed" }

Write-Host "OK: $outDir\menu_preview.exe"
