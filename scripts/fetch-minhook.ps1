# Fetch MinHook (x86 ASI hooks)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = Join-Path $Root "thirdparty\minhook"

if (Test-Path (Join-Path $dest "include\MinHook.h")) {
  Write-Host "OK: MinHook already at $dest"
  exit 0
}

Write-Host "Cloning MinHook into $dest ..."
git clone --depth 1 https://github.com/TsudaKageyu/minhook.git $dest
Write-Host "OK: MinHook ready"
