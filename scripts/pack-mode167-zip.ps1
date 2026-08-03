# Pack the Mode 167 release from reviewed repository/build artifacts only.
param(
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OutAsi = Join-Path $Root "out-asi"
$Dist = Join-Path $Root "dist"
$Stage = Join-Path $Dist "gtaiv-dxvk-vr-mode167-stage"
$Drop = Join-Path $Stage "drop-into-GTAIV"
$Licenses = Join-Path $Stage "licenses"
$ZipOut = Join-Path $Dist "gtaiv-dxvk-vr-mode167-install.zip"
$BuildId = "20260728-mode167-hudtune"
$Dxvk302Sha256 = "44A2E749694128710CCE3A545C954BD9D986DD6A8EA61BB08D931C2EF0190488"

function Assert-X86Pe {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Path,

    [Parameter(Mandatory = $true)]
    [string] $Label
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Missing required $Label artifact: $Path"
  }

  $stream = [System.IO.File]::Open(
    $Path,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::Read
  )
  $reader = New-Object System.IO.BinaryReader($stream)
  try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
      throw "$Label is not a valid PE file: $Path"
    }

    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 0x40 -or ($peOffset + 6) -gt $stream.Length) {
      throw "$Label has an invalid PE header offset: $Path"
    }

    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550) {
      throw "$Label is missing the PE signature: $Path"
    }

    $machine = $reader.ReadUInt16()
    if ($machine -ne 0x014C) {
      throw ("{0} must be Win32/x86 (PE machine 0x014C); found 0x{1:X4}: {2}" -f $Label, $machine, $Path)
    }
  } finally {
    $reader.Dispose()
  }
}

if (-not $SkipBuild) {
  & (Join-Path $Root "scripts\build-asi.ps1")
  if ($LASTEXITCODE -ne 0) {
    throw "The Win32 ASI build failed with exit code $LASTEXITCODE."
  }
}

# These are the only binary inputs. The DXVK file is a repository-controlled,
# hash-pinned copy of the official x86 DXVK 3.0.2 d3d9.dll.
$asi = Join-Path $OutAsi "gtaiv_dxvk_vr.asi"
$openVr = Join-Path $OutAsi "openvr_api.dll"
$d3d9 = Join-Path $Root "prebuilt\mode170\drop-into-GTAIV\d3d9.dll"

# These are the only non-generated file inputs.
$squareCommandLine = Join-Path $Root "config\commandline.vr-square.txt"
$projectLicense = Join-Path $Root "LICENSE"
$credits = Join-Path $Root "CREDITS.md"
$openVrLicense = Join-Path $Root "thirdparty\openvr\LICENSE"
$minHookLicense = Join-Path $Root "thirdparty\minhook\LICENSE.txt"

$requiredInputs = [ordered]@{
  "project ASI" = $asi
  "OpenVR loader" = $openVr
  "DXVK 3.0.2" = $d3d9
  "square command-line preset" = $squareCommandLine
  "project license" = $projectLicense
  "project credits" = $credits
  "OpenVR license" = $openVrLicense
  "MinHook license" = $minHookLicense
}
foreach ($entry in $requiredInputs.GetEnumerator()) {
  if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
    throw "Missing required $($entry.Key) file: $($entry.Value)"
  }
}

Assert-X86Pe -Path $asi -Label "gtaiv_dxvk_vr.asi"
Assert-X86Pe -Path $openVr -Label "openvr_api.dll"
Assert-X86Pe -Path $d3d9 -Label "DXVK d3d9.dll"

$actualDxvkHash = (Get-FileHash -LiteralPath $d3d9 -Algorithm SHA256).Hash.ToUpperInvariant()
if ($actualDxvkHash -ne $Dxvk302Sha256) {
  throw "Refusing to package d3d9.dll: expected official x86 DXVK 3.0.2 SHA-256 $Dxvk302Sha256, found $actualDxvkHash."
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
if (Test-Path -LiteralPath $Stage) {
  Remove-Item -LiteralPath $Stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $Drop | Out-Null
New-Item -ItemType Directory -Force -Path $Licenses | Out-Null

Copy-Item -LiteralPath $asi -Destination (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item -LiteralPath $openVr -Destination (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item -LiteralPath $d3d9 -Destination (Join-Path $Drop "d3d9.dll") -Force
Copy-Item -LiteralPath $squareCommandLine -Destination (Join-Path $Drop "commandline.txt.vr-square") -Force

$configs = [ordered]@{
  "gtaiv_dxvk_vr.stereo"      = "167"
  "gtaiv_dxvk_vr.vres"        = "2048"
  "gtaiv_dxvk_vr.eyefwd"      = "0"
  "gtaiv_dxvk_vr.fovadd"      = "0"
  "gtaiv_dxvk_vr.fpfov"       = "100 100 100"
  "gtaiv_dxvk_vr.camoff"      = "0 0 0"
  "gtaiv_dxvk_vr.vehcamoff"   = "0 -12 0"
  "gtaiv_dxvk_vr.mousesens"   = "50"
  "gtaiv_dxvk_vr.joysens"     = "20 30"
  "gtaiv_dxvk_vr.dualn"       = "2"
  "gtaiv_dxvk_vr.ipd"         = "3"
  "gtaiv_dxvk_vr.pedhide"     = "1"
  "gtaiv_dxvk_vr.stereoscale" = "130"
  "gtaiv_dxvk_vr.hudoff"      = "center 1.0 -0.10 0.10"
  "gtaiv_dxvk_vr.buildid"     = $BuildId
}
foreach ($entry in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $entry.Key) -Value $entry.Value -Encoding ASCII
}

@"
gtaiv-dxvk-vr - Mode 167 install
================================

This package contains only this project's Win32 ASI, its OpenVR loader, the
hash-verified x86 DXVK 3.0.2 d3d9.dll, Mode 167 sidecars, and documentation.
It contains no Rockstar game files, ASI loader, or FusionFix files.

BEFORE INSTALLING
-----------------
1. Own and install GTA IV Complete Edition.
2. Install Ultimate ASI Loader from its upstream release:
   https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
3. Install FusionFix from its upstream release:
   https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/releases
4. Install SteamVR and confirm the headset works in SteamVR.
5. Close GTA IV. Use singleplayer/offline while testing this WIP.

INSTALL
-------
1. Open the folder containing GTAIV.exe.
2. Back up any existing d3d9.dll and gtaiv_dxvk_vr.* files.
3. Copy the contents of drop-into-GTAIV into that folder.
4. In FusionFix graphics settings, select DirectX 9, not FusionFix Vulkan.
5. Start SteamVR, then start GTA IV.
6. Check gtaiv_dxvk_vr.log next to GTAIV.exe.

The optional commandline.txt.vr-square preset is included from this repository.
Review it first. Rename it to commandline.txt only if you want that square
window preset and do not already need another commandline.txt.

This package does not replace or modify GTA's HUD data. Obtain all game and
FusionFix content from their respective owners/upstream projects.

FALLBACK
--------
Set gtaiv_dxvk_vr.stereo to 0 to disable stereo. Restore your d3d9.dll backup
to remove DXVK. See CONFIG-GUIDE.txt for the packaged Mode 167 values.
"@ | Set-Content -LiteralPath (Join-Path $Stage "INSTALL.txt") -Encoding ASCII

@"
gtaiv-dxvk-vr - Mode 167 configuration
======================================

All sidecar files belong next to GTAIV.exe. Edit them with Notepad and restart
the game after changing them.

Packaged preset:
  stereo       = 167
  vres         = 2048
  eyefwd       = 0
  fovadd       = 0
  fpfov        = 100 100 100
  camoff       = 0 0 0
  vehcamoff    = 0 -12 0
  mousesens    = 50
  joysens      = 20 30
  dualn        = 2
  ipd          = 3
  pedhide      = 1
  stereoscale  = 130
  hudoff       = center 1.0 -0.10 0.10
  buildid      = $BuildId

Set gtaiv_dxvk_vr.stereo to 0 for the kill switch. Runtime diagnostics are
written to gtaiv_dxvk_vr.log next to GTAIV.exe.
"@ | Set-Content -LiteralPath (Join-Path $Stage "CONFIG-GUIDE.txt") -Encoding ASCII

Copy-Item -LiteralPath (Join-Path $Stage "INSTALL.txt") -Destination (Join-Path $Drop "gtaiv_dxvk_vr-INSTALL.txt") -Force
Copy-Item -LiteralPath (Join-Path $Stage "CONFIG-GUIDE.txt") -Destination (Join-Path $Drop "gtaiv_dxvk_vr-CONFIG-GUIDE.txt") -Force
Copy-Item -LiteralPath $projectLicense -Destination (Join-Path $Stage "LICENSE.txt") -Force
Copy-Item -LiteralPath $credits -Destination (Join-Path $Stage "CREDITS.md") -Force
Copy-Item -LiteralPath $openVrLicense -Destination (Join-Path $Licenses "OpenVR-LICENSE.txt") -Force
Copy-Item -LiteralPath $minHookLicense -Destination (Join-Path $Licenses "MinHook-LICENSE.txt") -Force

@'
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
'@ | Set-Content -LiteralPath (Join-Path $Licenses "DXVK-LICENSE.txt") -Encoding UTF8

@"
Third-party components included in this Mode 167 package
========================================================

DXVK 3.0.2
  File: drop-into-GTAIV\d3d9.dll
  Upstream: https://github.com/doitsujin/dxvk/releases/tag/v3.0.2
  Required SHA-256: $Dxvk302Sha256
  License: licenses\DXVK-LICENSE.txt

OpenVR
  File: drop-into-GTAIV\openvr_api.dll
  Upstream: https://github.com/ValveSoftware/openvr
  License: licenses\OpenVR-LICENSE.txt

MinHook
  Linked into gtaiv_dxvk_vr.asi
  Upstream: https://github.com/TsudaKageyu/minhook
  License: licenses\MinHook-LICENSE.txt

NOT INCLUDED
------------
- GTA IV or any other Rockstar asset
- Ultimate ASI Loader
- FusionFix binaries, configuration, or update tree
- Complete Edition Hook
- Game-generated runtime files
- Closed , flat-to-VR, or HL2VR binaries

Install Ultimate ASI Loader and FusionFix directly from their upstream releases
listed in INSTALL.txt.
"@ | Set-Content -LiteralPath (Join-Path $Stage "THIRDPARTY.txt") -Encoding ASCII

$hashFiles = [ordered]@{
  "drop-into-GTAIV\d3d9.dll" = Join-Path $Drop "d3d9.dll"
  "drop-into-GTAIV\gtaiv_dxvk_vr.asi" = Join-Path $Drop "gtaiv_dxvk_vr.asi"
  "drop-into-GTAIV\openvr_api.dll" = Join-Path $Drop "openvr_api.dll"
}
$hashLines = foreach ($entry in $hashFiles.GetEnumerator()) {
  $hash = (Get-FileHash -LiteralPath $entry.Value -Algorithm SHA256).Hash.ToUpperInvariant()
  "$hash  $($entry.Key)"
}
$hashLines | Set-Content -LiteralPath (Join-Path $Stage "SHA256SUMS.txt") -Encoding ASCII

$packageEntries = @(
  (Join-Path $Stage "drop-into-GTAIV"),
  (Join-Path $Stage "licenses"),
  (Join-Path $Stage "INSTALL.txt"),
  (Join-Path $Stage "CONFIG-GUIDE.txt"),
  (Join-Path $Stage "LICENSE.txt"),
  (Join-Path $Stage "CREDITS.md"),
  (Join-Path $Stage "THIRDPARTY.txt"),
  (Join-Path $Stage "SHA256SUMS.txt")
)

if (Test-Path -LiteralPath $ZipOut) {
  Remove-Item -LiteralPath $ZipOut -Force
}

Write-Host "Compressing the reviewed Mode 167 allowlist..."
Compress-Archive -LiteralPath $packageEntries -DestinationPath $ZipOut -CompressionLevel Optimal

$zipLen = (Get-Item -LiteralPath $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Mode 167 package ready. Unzip it, read INSTALL.txt, then copy only drop-into-GTAIV contents."
