# One-shot: submodule + MinHook + OpenVR (all thirdparty needed to build ASI)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

Write-Host "=== 1/3 DXVK submodule ==="
& "$PSScriptRoot\init-submodules.ps1"
if ($LASTEXITCODE -ne 0) { throw "init-submodules failed" }

Write-Host ""
Write-Host "=== 2/3 MinHook ==="
& "$PSScriptRoot\fetch-minhook.ps1"
if ($LASTEXITCODE -ne 0) { throw "fetch-minhook failed" }

Write-Host ""
Write-Host "=== 3/3 OpenVR v2.12.14 ==="
& "$PSScriptRoot\fetch-openvr.ps1"
if ($LASTEXITCODE -ne 0) { throw "fetch-openvr failed" }

Write-Host ""
Write-Host "=== Verify ==="
$checks = @(
  @{ Path = "dxvk\src\d3d9\d3d9_vr.h"; Name = "dxvk d3d9_vr.h" },
  @{ Path = "thirdparty\minhook\include\MinHook.h"; Name = "MinHook.h" },
  @{ Path = "thirdparty\openvr\headers\openvr.h"; Name = "openvr.h" },
  @{ Path = "thirdparty\openvr\bin\win32\openvr_api.dll"; Name = "openvr_api.dll (win32)" },
  @{ Path = "thirdparty\openvr\lib\win32\openvr_api.lib"; Name = "openvr_api.lib (win32)" },
  @{ Path = "thirdparty\dxvk\d3d9_vk_interop.h"; Name = "d3d9_vk_interop.h" }
)
$bad = $false
foreach ($c in $checks) {
  $ok = Test-Path (Join-Path $Root $c.Path)
  if ($ok) { Write-Host "  OK  $($c.Name)" }
  else { Write-Host "  MISS $($c.Name) ($($c.Path))"; $bad = $true }
}
if ($bad) { throw "Some dependencies missing — see MISS lines above" }

Write-Host ""
Write-Host "All deps ready. Next: .\scripts\build-asi.ps1"
