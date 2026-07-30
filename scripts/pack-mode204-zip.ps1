# Pack the Mode 204 preset using only explicit project/build inputs.
param(
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$OutAsi = Join-Path $Root "out-asi"
$Pre = Join-Path $Root "prebuilt\mode204"
$Stage = Join-Path $Root "dist\gtaiv-dxvk-vr-mode204-drop"
$Drop = Join-Path $Stage "drop-into-GTAIV"
$ZipOut = Join-Path $Root "dist\gtaiv-dxvk-vr-mode204-install.zip"
$BuildId = "20260728-mode204-fusion"
$DxvkSha256 = "44A2E749694128710CCE3A545C954BD9D986DD6A8EA61BB08D931C2EF0190488"

function Assert-Win32PeFile {
  param(
    [Parameter(Mandatory = $true)]
    [string] $LiteralPath,
    [Parameter(Mandatory = $true)]
    [string] $Label
  )

  $bytes = [System.IO.File]::ReadAllBytes($LiteralPath)
  if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
    throw "$Label is not a valid PE file: $LiteralPath"
  }

  $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
  if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
    throw "$Label has an invalid PE header offset: $LiteralPath"
  }
  if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw "$Label has no PE signature: $LiteralPath"
  }

  $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
  if ($machine -ne 0x014c) {
    throw ("$Label must be Win32/x86 (PE machine 0x014c); found 0x{0:x4}: {1}" -f
      $machine, $LiteralPath)
  }
}

function Resolve-BuildArtifact {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Label,
    [Parameter(Mandatory = $true)]
    [string[]] $Candidates
  )

  foreach ($candidate in $Candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  throw "$Label is missing. Expected one of: $($Candidates -join ', ')"
}

function Resolve-VerifiedDxvkD3D9 {
  param(
    [Parameter(Mandatory = $true)]
    [string[]] $Candidates
  )

  $candidate = Resolve-BuildArtifact "Stock DXVK 3.0.2 d3d9.dll" $Candidates
  Assert-Win32PeFile $candidate "Stock DXVK 3.0.2 d3d9.dll"
  $actualHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
  if ($actualHash -ne $DxvkSha256) {
    throw (
      "Refusing unverified d3d9.dll. Expected DXVK 3.0.2 x86 SHA256 " +
      "$DxvkSha256, found $actualHash at $candidate"
    )
  }
  return $candidate
}

if (-not $SkipBuild) {
  & (Join-Path $Root "scripts\build-asi.ps1")
}

$asi = Resolve-BuildArtifact "gtaiv_dxvk_vr.asi" @(
  (Join-Path $OutAsi "gtaiv_dxvk_vr.asi"),
  (Join-Path $Pre "gtaiv_dxvk_vr.asi")
)
$ovr = Resolve-BuildArtifact "openvr_api.dll" @(
  (Join-Path $OutAsi "openvr_api.dll"),
  (Join-Path $Pre "openvr_api.dll")
)
$d3d9 = Resolve-VerifiedDxvkD3D9 @(
  (Join-Path $Root "vendor\dxvk-3.0.2\x32\d3d9.dll"),
  (Join-Path $Root "prebuilt\mode170\drop-into-GTAIV\d3d9.dll")
)
$projectLicense = Join-Path $Root "LICENSE"
$openVrLicense = Join-Path $Root "thirdparty\openvr\LICENSE"
$minHookLicense = Join-Path $Root "thirdparty\minhook\LICENSE.txt"
$credits = Join-Path $Root "CREDITS.md"

foreach ($requiredFile in @(
    $projectLicense,
    $openVrLicense,
    $minHookLicense,
    $credits
  )) {
  if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
    throw "Missing required package notice: $requiredFile"
  }
}
Assert-Win32PeFile $asi "gtaiv_dxvk_vr.asi"
Assert-Win32PeFile $ovr "openvr_api.dll"

if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Drop | Out-Null

# Only these validated binaries enter the drop.
Copy-Item $asi (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll") -Force

# Mode 204 preset.
$configs = @{
  "gtaiv_dxvk_vr.backend"     = "openvr"
  "gtaiv_dxvk_vr.stereo"      = "204"
  "gtaiv_dxvk_vr.vres"        = "2048"
  "gtaiv_dxvk_vr.eyefwd"      = "0"
  "gtaiv_dxvk_vr.fovadd"      = "0"
  "gtaiv_dxvk_vr.fpfov"       = "110 110 110"
  "gtaiv_dxvk_vr.camoff"      = "0 0 0"
  "gtaiv_dxvk_vr.vehcamoff"   = "0 -12 0"
  "gtaiv_dxvk_vr.mousesens"   = "50"
  "gtaiv_dxvk_vr.joysens"     = "20 30"
  "gtaiv_dxvk_vr.dualn"       = "2"
  "gtaiv_dxvk_vr.ipd"         = "2"
  "gtaiv_dxvk_vr.pedhide"     = "0"
  "gtaiv_dxvk_vr.stereoscale" = "130"
  "gtaiv_dxvk_vr.scale"       = "100"
  "gtaiv_dxvk_vr.hudoff"      = "center 1.0 -0.10 0.10"
  "gtaiv_dxvk_vr.buildid"     = $BuildId
}
foreach ($kv in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $kv.Key) -Value $kv.Value -Encoding ASCII
}

$installPath = Join-Path $Stage "INSTALL.txt"
$configGuidePath = Join-Path $Stage "CONFIG-GUIDE.txt"
$thirdPartyPath = Join-Path $Stage "THIRDPARTY.txt"
$dxvkLicensePath = Join-Path $Stage "LICENSE-DXVK.txt"

@"
gtaiv-dxvk-vr - Mode 204 install
================================

This archive contains only gtaiv-dxvk-vr, its Mode 204 configuration,
OpenVR, and a hash-verified Win32 DXVK 3.0.2 d3d9.dll.

Before copying these files:
1. Own and install GTA IV Complete Edition.
2. Install an ASI loader from its official release.
3. If you use FusionFix, install it separately from its official release:
   https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix
4. Configure FusionFix to use DirectX 9, not its Vulkan path.
5. Install and test SteamVR.

Copy the contents of drop-into-GTAIV into the folder containing GTAIV.exe.
Develop and test only in singleplayer/offline mode. The runtime log is
gtaiv_dxvk_vr.log next to GTAIV.exe.

Not included: Rockstar files, an ASI loader, FusionFix files or update assets,
Complete Edition hooks, generated shader/state caches, or saved game data.
"@ | Set-Content -LiteralPath $installPath -Encoding UTF8

@"
Mode 204 configuration
======================

The gtaiv_dxvk_vr.* files belong next to GTAIV.exe.
stereo=204 selects the fused-stereo milestone.
vres=2048 is the conservative starting eye-canvas resolution.
ipd, stereoscale, scale, fpfov, and camera offsets can be tuned with the
documented hotkeys. Keep FirstPerson.asi disabled while testing this preset.
"@ | Set-Content -LiteralPath $configGuidePath -Encoding UTF8

@"
Bundled components
==================
- gtaiv_dxvk_vr.asi: this project
- DXVK 3.0.2 Win32 d3d9.dll: https://github.com/doitsujin/dxvk
- OpenVR Win32 client library: https://github.com/ValveSoftware/openvr
- MinHook, linked into the ASI: https://github.com/TsudaKageyu/minhook

Install separately from official upstream releases
==================================================
- An ASI loader
- FusionFix: https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix

No Rockstar game assets, FusionFix files, closed VR-mod binaries, or
game-derived caches are included.
"@ | Set-Content -LiteralPath $thirdPartyPath -Encoding UTF8

@"
Copyright (c) 2017 Philip Rebohle
                  Copyright (c) 2019 Joshua Ashton
                  Copyright (c) 2019 Robin Kertels
                  Copyright (c) 2023 Jeffrey Ellison

                          zlib/libpng license

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

- The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.
- Altered source versions must be plainly marked as such, and must not
  be misrepresented as being the original software.
- This notice may not be removed or altered from any source distribution.
"@ | Set-Content -LiteralPath $dxvkLicensePath -Encoding UTF8

Copy-Item $projectLicense (Join-Path $Stage "LICENSE-gtaiv-dxvk-vr.txt") -Force
Copy-Item $openVrLicense (Join-Path $Stage "LICENSE-OpenVR.txt") -Force
Copy-Item $minHookLicense (Join-Path $Stage "LICENSE-MinHook.txt") -Force
Copy-Item $credits (Join-Path $Stage "CREDITS.md") -Force
Copy-Item $installPath (Join-Path $Drop "gtaiv_dxvk_vr-INSTALL.txt") -Force
Copy-Item $configGuidePath (Join-Path $Drop "gtaiv_dxvk_vr-CONFIG-GUIDE.txt") -Force

New-Item -ItemType Directory -Force -Path (Join-Path $Root "dist") | Out-Null
if (Test-Path $ZipOut) { Remove-Item -LiteralPath $ZipOut -Force }

$packageEntries = @(
  $Drop,
  $installPath,
  $configGuidePath,
  $thirdPartyPath,
  $dxvkLicensePath,
  (Join-Path $Stage "LICENSE-gtaiv-dxvk-vr.txt"),
  (Join-Path $Stage "LICENSE-OpenVR.txt"),
  (Join-Path $Stage "LICENSE-MinHook.txt"),
  (Join-Path $Stage "CREDITS.md")
)
Write-Host "Compressing Mode 204 install pack..."
Compress-Archive -LiteralPath $packageEntries -DestinationPath $ZipOut -CompressionLevel Optimal -Force

$zipLen = (Get-Item $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Includes only: Mode 204 ASI/config, verified stock DXVK 3.0.2, OpenVR, and notices."
Write-Host "Install the ASI loader and FusionFix separately from their official releases."
