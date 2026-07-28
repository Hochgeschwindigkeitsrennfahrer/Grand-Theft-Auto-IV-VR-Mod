[CmdletBinding()]
param(
    [string]$Destination = "",
    [string]$RepositoryUrl = "https://github.com/elliotttate/OpenXR-Simulator.git",
    [switch]$PrepareOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$PinnedCommit = "48a70f440ac7d9bda385994937e3da8e15a4d9bb"
$ExpectedPatchSha256 = "A26147E2FD9C033895F238DA575D080911D78CEA972AB236964F552177916324"
$ExpectedCliSha256 = "5F5136D78C4E3E6D75F4F3CD18D5D3F6190C02E3FE9242A919A925F76CEECFB6"

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$PatchPath = Join-Path $RepoRoot "patches\openxr-simulator-headless-cli-48a70f4.patch"
$CliAssetPath = Join-Path $RepoRoot "patches\openxr-simulator-headless-cli-input.py"
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepoRoot "out-openxr\external\OpenXR-Simulator"
}
$Destination = [IO.Path]::GetFullPath($Destination)
$GitSafeDirectoryArgument = "safe.directory=$Destination"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Get-GitText {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git failed with exit code $LASTEXITCODE"
    }
    return (($output | Out-String).Trim())
}

function Test-PatchDirection {
    param([switch]$Reverse)

    $arguments = @(
        "-c",
        $GitSafeDirectoryArgument,
        "-C",
        $Destination,
        "apply",
        "--check",
        "--quiet",
        "--whitespace=fix"
    )
    if ($Reverse) {
        $arguments += "--reverse"
    }
    $arguments += $PatchPath
    & git @arguments 2>$null
    return $LASTEXITCODE -eq 0
}

function Assert-Sha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required package file is missing: $Path"
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    if ($actual -ne $Expected) {
        throw "SHA-256 mismatch for '$Path': expected $Expected, got $actual"
    }
}

function Assert-PeX64 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64) {
            throw "DLL is too small to be a PE image: $Path"
        }
        $stream.Position = 0
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "DLL has no MZ signature: $Path"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or ($peOffset + 6) -gt $stream.Length) {
            throw "DLL has an invalid PE offset: $Path"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "DLL has no PE signature: $Path"
        }
        $machine = $reader.ReadUInt16()
        if ($machine -ne 0x8664) {
            throw ("DLL is not Win64/x64 (PE machine 0x{0:X4}): {1}" -f $machine, $Path)
        }
    }
    finally {
        $stream.Dispose()
    }
}

foreach ($tool in @("git", "cmake")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool is not on PATH: $tool"
    }
}

Assert-Sha256 -Path $PatchPath -Expected $ExpectedPatchSha256
Assert-Sha256 -Path $CliAssetPath -Expected $ExpectedCliSha256

if (-not (Test-Path -LiteralPath $Destination)) {
    $parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Invoke-Native -FilePath "git" -Arguments @(
        "clone",
        "--filter=blob:none",
        "--no-checkout",
        $RepositoryUrl,
        $Destination
    )
    Invoke-Native -FilePath "git" -Arguments @(
        "-c",
        $GitSafeDirectoryArgument,
        "-C",
        $Destination,
        "checkout",
        "--detach",
        $PinnedCommit
    )
}
elseif (-not (Test-Path -LiteralPath (Join-Path $Destination ".git"))) {
    throw "Destination exists but is not a Git working tree: $Destination"
}

$head = Get-GitText -Arguments @(
    "-c",
    $GitSafeDirectoryArgument,
    "-C",
    $Destination,
    "rev-parse",
    "HEAD"
)
if ($head -ne $PinnedCommit) {
    throw "Refusing non-pinned checkout at '$Destination': expected $PinnedCommit, got $head"
}

$forwardApplies = Test-PatchDirection
$reverseApplies = Test-PatchDirection -Reverse
if ($forwardApplies -and -not $reverseApplies) {
    Invoke-Native -FilePath "git" -Arguments @(
        "-c",
        $GitSafeDirectoryArgument,
        "-C",
        $Destination,
        "apply",
        "--whitespace=fix",
        $PatchPath
    )
}
elseif (-not $forwardApplies -and $reverseApplies) {
    Write-Host "Pinned simulator source patch is already applied."
}
else {
    throw "Simulator tree is neither pristine pinned source nor the exact packaged patch."
}

if (-not (Test-PatchDirection -Reverse)) {
    throw "Packaged simulator patch did not verify after application."
}

$unexpected = @(
    & git -c $GitSafeDirectoryArgument -C $Destination status --porcelain |
        Where-Object {
            $_ -notmatch "^\s*M src/(mcp_integration\.h|runtime\.cpp)$" -and
            $_ -notmatch "^\?\? scripts/openxr_simulator_input\.py$"
        }
)
if ($LASTEXITCODE -ne 0) {
    throw "git status failed with exit code $LASTEXITCODE"
}
if ($unexpected.Count -gt 0) {
    throw "Unexpected changes in simulator checkout:`n$($unexpected -join [Environment]::NewLine)"
}

$CliDestination = Join-Path $Destination "scripts\openxr_simulator_input.py"
Copy-Item -LiteralPath $CliAssetPath -Destination $CliDestination -Force
Assert-Sha256 -Path $CliDestination -Expected $ExpectedCliSha256

$runtimeSource = Get-Content -LiteralPath (Join-Path $Destination "src\runtime.cpp") -Raw
foreach ($marker in @(
    'OPENXR_SIMULATOR_HEADLESS',
    'OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE',
    'static bool IsInputFocusForeground()',
    'if (IsHeadlessMode()) return false;'
)) {
    if ($runtimeSource -notmatch [regex]::Escape($marker)) {
        throw "Patched runtime marker is missing: $marker"
    }
}
$mcpSource = Get-Content -LiteralPath (Join-Path $Destination "src\mcp_integration.h") -Raw
if ($mcpSource -notmatch [regex]::Escape("WriteControllerCommandAck")) {
    throw "Patched MCP controller acknowledgment marker is missing."
}

if ($PrepareOnly) {
    Write-Host "Prepared pinned Elliott OpenXR Simulator CLI/headless source."
    Write-Host "Source:   $Destination"
    Write-Host "Commit:   $PinnedCommit"
    Write-Host "CLI tool: $CliDestination"
    exit 0
}

$BuildDirectory = Join-Path $Destination "build-cli-x64"
$pathRows = @(& cmd.exe /d /c "set path")
$hasUpperPath =
    @($pathRows | Where-Object { $_ -cmatch "^PATH=" }).Count -gt 0
$hasMixedPath =
    @($pathRows | Where-Object { $_ -cmatch "^Path=" }).Count -gt 0
if ($hasUpperPath -and $hasMixedPath) {
    # MSBuild's .NET launcher rejects a child environment containing both
    # case variants. Keep the mixed-case Path and remove only duplicate PATH.
    $cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
    $configureCommand =
        'set "PATH=" && "{0}" -S "{1}" -B "{2}" -G "Visual Studio 17 2022" -A x64' -f
        $cmakeExe,
        $Destination,
        $BuildDirectory
    & cmd.exe /d /s /c $configureCommand
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE"
    }
    $buildCommand =
        'set "PATH=" && "{0}" --build "{1}" --config Release' -f
        $cmakeExe,
        $BuildDirectory
    & cmd.exe /d /s /c $buildCommand
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }
}
else {
    Invoke-Native -FilePath "cmake" -Arguments @(
        "-S",
        $Destination,
        "-B",
        $BuildDirectory,
        "-G",
        "Visual Studio 17 2022",
        "-A",
        "x64"
    )
    Invoke-Native -FilePath "cmake" -Arguments @(
        "--build",
        $BuildDirectory,
        "--config",
        "Release"
    )
}

$DllPath = [IO.Path]::GetFullPath((Join-Path $Destination "bin\openxr_simulator.dll"))
$ManifestPath = [IO.Path]::GetFullPath((Join-Path $Destination "bin\openxr_simulator.json"))
if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) {
    throw "Win64 Release build did not produce: $DllPath"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "CMake did not generate the runtime manifest: $ManifestPath"
}
Assert-PeX64 -Path $DllPath

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if (-not $manifest.runtime -or [string]::IsNullOrWhiteSpace($manifest.runtime.library_path)) {
    throw "Runtime manifest has no runtime.library_path: $ManifestPath"
}
$declaredDll = [string]$manifest.runtime.library_path
if (-not [IO.Path]::IsPathRooted($declaredDll)) {
    $declaredDll = Join-Path (Split-Path -Parent $ManifestPath) $declaredDll
}
$declaredDll = [IO.Path]::GetFullPath($declaredDll)
if (-not [string]::Equals($declaredDll, $DllPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Runtime manifest points to '$declaredDll', expected '$DllPath'."
}

$dllHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $DllPath).Hash
Write-Host "Elliott OpenXR Simulator CLI/headless package: PASS"
Write-Host "Source commit: $PinnedCommit"
Write-Host "DLL:           $DllPath"
Write-Host "DLL SHA-256:   $dllHash"
Write-Host "Manifest:      $ManifestPath"
Write-Host "CLI tool:      $CliDestination"
Write-Host "No runtime was registered or launched."
