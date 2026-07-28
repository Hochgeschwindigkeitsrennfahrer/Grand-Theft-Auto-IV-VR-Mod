$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$launcherPath = Join-Path `
  $PSScriptRoot "..\scripts\run-openxr-gta-steam-safe.ps1"
$launcher = Get-Content -LiteralPath $launcherPath -Raw

$tokens = $null
$parseErrors = $null
$launcherAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $launcherPath).Path,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Launcher has PowerShell parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

function Get-LauncherFunctionDefinition([string]$Name) {
  $matches = @($launcherAst.FindAll(
    {
      param($node)
      $node -is
        [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq $Name
    },
    $true
  ))
  if ($matches.Count -ne 1) {
    throw "Expected exactly one launcher function named $Name."
  }
  return [scriptblock]::Create($matches[0].Extent.Text)
}

. (Get-LauncherFunctionDefinition "Test-CanonicalPathEqual")
. (Get-LauncherFunctionDefinition "Test-PathEqualOrWithinRoot")

$rockstarRoot = "C:\Program Files\Rockstar Games"
$rootCases = @(
  [pscustomobject]@{
    Name = "equal root"
    Path = "C:\Program Files\Rockstar Games"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "direct child"
    Path = "C:\Program Files\Rockstar Games\Launcher\Launcher.exe"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "case-insensitive child"
    Path = "c:\program files\ROCKSTAR GAMES\Social Club\Helper.exe"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "sibling prefix"
    Path = "C:\Program Files\Rockstar Games Backup\Launcher.exe"
    Expected = $false
  }
  [pscustomobject]@{
    Name = "numeric prefix"
    Path = "C:\Program Files\Rockstar Games2\Launcher.exe"
    Expected = $false
  }
  [pscustomobject]@{
    Name = "normalized sibling"
    Path = "C:\Program Files\Rockstar Games\..\Rockstar Games Backup\Launcher.exe"
    Expected = $false
  }
)
foreach ($case in $rootCases) {
  $actual = Test-PathEqualOrWithinRoot $case.Path $rockstarRoot
  if ($actual -ne $case.Expected) {
    throw (
      "Rockstar root case '$($case.Name)' expected " +
      "$($case.Expected), got $actual."
    )
  }
}

$expectedPlayGta =
  "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV\PlayGTAIV.exe"
if (-not (Test-CanonicalPathEqual `
    $expectedPlayGta `
    "d:\steamlibrary\steamapps\common\Grand Theft Auto IV\GTAIV\PlayGTAIV.exe")) {
  throw "Exact PlayGTAIV path comparison is not case-insensitive."
}
if (Test-CanonicalPathEqual `
    $expectedPlayGta `
    "D:\Other Game\PlayGTAIV.exe") {
  throw "Exact PlayGTAIV path comparison accepted an unrelated executable."
}

$requiredPatterns = [ordered]@{
  PlayGtaPath =
    '\$PlayGtaExe\s*=\s*Join-Path \$GameDir "PlayGTAIV\.exe"'
  RetryExactPath =
    '(?s)Stop-ByExactPath.{0,180}"stalled Rockstar handoff retry"'
  ExactStopPredicate =
    'Test-CanonicalPathEqual \$processPath \$ExpectedPath'
  RockstarBoundaryHelper =
    'Test-PathEqualOrWithinRoot \$processPath \$rockstarRoot'
  CleanupFailureResult =
    '"cleanupFailures=\$\(\$cleanupFailures -join '' \| ''\)"'
  ArchiveFailureResult =
    '"archiveFailures=\$\(\$archiveFailures -join '' \| ''\)"'
  BestEffortRestoreStatus =
    'Write-StatusBestEffort \(\s*"RESTORE WARNING:'
}
foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($launcher -notmatch $entry.Value) {
    throw "Missing launcher safety contract: $($entry.Key)"
  }
}

$forbiddenPatterns = [ordered]@{
  NameOnlyPlayGtaStop =
    '(?s)Stop-ByName\s+@\([^\)]*"PlayGTAIV"'
  RockstarSiblingPrefix =
    '(?s)GetFullPath\(\$processPath\)\.StartsWith\(\s*\$rockstarRoot'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($launcher -match $entry.Value) {
    throw "Forbidden launcher safety contract found: $($entry.Key)"
  }
}

$tryStatements = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.TryStatementAst]
  },
  $true
))
$restorationBarriers = @($tryStatements | Where-Object {
  $_.Finally -and
  $_.Body.Extent.Text -match '\$diagnostics\s*=\s*@\(' -and
  $_.Body.Extent.Text -match 'Copy-Item' -and
  $_.Finally.Extent.Text -match 'foreach \(\$state in \$states\)' -and
  $_.Finally.Extent.Text -match 'Restore-State \$state'
})
if ($restorationBarriers.Count -ne 1) {
  throw (
    "Expected one diagnostic-cleanup try/finally restoration barrier; " +
    "found $($restorationBarriers.Count)."
  )
}
$barrier = $restorationBarriers[0]
if ($barrier.CatchClauses.Count -eq 0) {
  throw "Restoration barrier does not absorb unexpected cleanup failures."
}
if ($barrier.Finally.Extent.Text -notmatch 'Write-StatusBestEffort') {
  throw "Restoration loop can still be interrupted by a status-log failure."
}

Write-Output (
  "OpenXrLauncherSafetyContractTest: PASS " +
  "rootCases=$($rootCases.Count) exactPath=2 " +
  "required=$($requiredPatterns.Count) forbidden=$($forbiddenPatterns.Count) " +
  "restorationBarrier=1"
)
