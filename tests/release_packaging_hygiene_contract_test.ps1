$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$packScripts = @(
  "scripts\pack-mode167-zip.ps1",
  "scripts\pack-install-zip.ps1",
  "scripts\pack-mode170-zip.ps1",
  "scripts\pack-mode204-zip.ps1"
)
$expectedDxvkHash =
  "44A2E749694128710CCE3A545C954BD9D986DD6A8EA61BB08D931C2EF0190488"

$forbiddenPatterns = [ordered]@{
  GameInstallVariable = '\$GameDir\b'
  HardCodedGameInstall = '(?i)Program Files.*Grand Theft Auto IV'
  RockstarHudData = '(?i)hud\.dat'
  InstalledAsiLoader = '(?i)dinput8\.dll'
  InstalledCompleteEditionHook = '(?i)aCompleteEditionHook\.asi'
  InstalledFusionFixBinary =
    '(?i)GTAIV\.EFLC\.FusionFix\.(?:asi|ini|cfg)'
  InstalledFusionFixTree =
    '(?i)update[\\/]+GTAIV\.EFLC\.FusionFix'
  GameDxvkCache = '(?i)GTAIV\.dxvk-cache'
  InspirationDxvkCache = '(?i)inspo[\\/]+dxvk cache'
  LegacyBackupDxvkSource = '(?i)backups[\\/]+dxvk-stock-302'
  RecursiveCopy = '(?im)^\s*Copy-Item\b[^\r\n]*\s-Recurse\b'
  WildcardStageCompression =
    '(?i)Compress-Archive[^\r\n]*(?:Join-Path\s+\$Stage\s+["'']\*|\\\*)'
  DynamicEnumeration = '(?im)^\s*Get-ChildItem\b'
}

$requiredPatterns = [ordered]@{
  ExactDxvkHash = [regex]::Escape($expectedDxvkHash)
  DxvkHashCheck = '(?i)Get-FileHash\b[^\r\n]*-Algorithm\s+SHA256'
  Win32PeCheck = '(?i)0x014c'
  PeValidationFunction = '(?i)function\s+Assert-(?:Win32PeFile|X86Pe)\b'
  ExplicitDxvkInput =
    '(?i)(?:vendor[\\]+dxvk-3\.0\.2[\\]+x32[\\]+d3d9\.dll|' +
    'prebuilt[\\]+mode170[\\]+drop-into-GTAIV[\\]+d3d9\.dll)'
  FusionFixUpstream =
    'https://github\.com/ThirteenAG/GTAIV\.EFLC\.FusionFix'
  RockstarExclusion =
    '(?i)(?:no|not included[\s\S]{0,80})[^\r\n]*Rockstar[^\r\n]*(?:asset|file)'
  ProjectLicenseInput =
    '(?i)\$projectLicense\s*=\s*Join-Path\s+\$Root\s+["'']LICENSE["'']'
  OpenVrLicenseInput =
    '(?i)thirdparty[\\]+openvr[\\]+LICENSE'
  MinHookLicenseInput =
    '(?i)thirdparty[\\]+minhook[\\]+LICENSE\.txt'
  DxvkLicense = '(?i)(?:LICENSE-DXVK|DXVK-LICENSE)\.txt'
  DxvkLicenseText = '(?i)zlib/libpng license'
  ExplicitArchiveList = '(?i)\$packageEntries\s*=\s*@\('
}

$checked = 0
foreach ($relativePath in $packScripts) {
  $path = (Resolve-Path -LiteralPath (Join-Path $root $relativePath)).Path
  $source = Get-Content -LiteralPath $path -Raw

  $tokens = $null
  $parseErrors = $null
  $ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $path,
    [ref]$tokens,
    [ref]$parseErrors
  )
  if ($parseErrors.Count -ne 0) {
    throw (
      "$relativePath has PowerShell parser errors: " +
      (($parseErrors | ForEach-Object Message) -join " | ")
    )
  }

  foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
    if ($source -match $entry.Value) {
      throw "$relativePath contains forbidden package input: $($entry.Key)"
    }
  }
  foreach ($entry in $requiredPatterns.GetEnumerator()) {
    if ($source -notmatch $entry.Value) {
      throw "$relativePath is missing release contract: $($entry.Key)"
    }
  }

  $compressCommands = @($ast.FindAll(
    {
      param($node)
      $node -is [System.Management.Automation.Language.CommandAst] -and
      $node.GetCommandName() -eq "Compress-Archive"
    },
    $true
  ))
  if ($compressCommands.Count -ne 1) {
    throw (
      "$relativePath must contain exactly one Compress-Archive command; " +
      "found $($compressCommands.Count)."
    )
  }
  if ($compressCommands[0].Extent.Text -notmatch
      '(?i)-LiteralPath\s+\$packageEntries\b') {
    throw "$relativePath does not archive its explicit package entry list."
  }

  $checked++
}

Write-Output (
  "ReleasePackagingHygieneContractTest: PASS " +
  "scripts=$checked forbidden=$($forbiddenPatterns.Count) " +
  "required=$($requiredPatterns.Count)"
)
