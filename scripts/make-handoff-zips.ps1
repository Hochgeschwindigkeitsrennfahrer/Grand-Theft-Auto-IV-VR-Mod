$ErrorActionPreference = "Stop"
$docs = "C:\Users\amien.krause\Documents"
$stamp = Get-Date -Format "yyyyMMdd-HHmm"
# Prefer stable FULL folder; also keep stamp in LIESMICH for traceability
$dest = Join-Path $docs "gtaiv-vr-handoff-FULL"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$readme = @"
GTA IV VR - Handoff-Paket FULL ($stamp)

Enthalten:
  gtaiv-openxr.zip     Phase A - OpenXR + ASI (WMR)  inkl. .git
  gtaiv-dxvk-vr.zip    Phase B - VR-DXVK + OpenVR    inkl. .git

Zuhause:
1. Beide Zips nach Documents entpacken (Ordnernamen beibehalten).
2. Phase A: Cursor -> Open Folder -> gtaiv-openxr -> docs\HOME_CURSOR.md
3. Phase B: Cursor -> Open Folder -> gtaiv-dxvk-vr -> docs\HOME_CURSOR.md
4. Phase B danach: .\scripts\init-submodules.ps1 (Internet noetig)

Hinweis:
- .git IST enthalten (volle Historie; auch versteckte Dateien).
- Ausgeschlossen: .vs, out, build, bin, dxvk sowie *.user *.suo *.obj *.pdb *.dll *.exe *.asi *.log
- dxvk-Submodule zu Hause neu holen/bauen.
"@
Set-Content -Path (Join-Path $dest "LIESMICH.txt") -Value $readme -Encoding UTF8

function Add-ZipTree([System.IO.Compression.ZipArchive]$archive, [string]$rootDir, [string]$relativePrefix) {
  # Include hidden/system files (.git etc.)
  $items = Get-ChildItem -LiteralPath $rootDir -Force
  foreach ($item in $items) {
    $entryName = if ($relativePrefix) { "$relativePrefix/$($item.Name)" } else { $item.Name }
    $entryName = $entryName -replace '\\', '/'
    if ($item.PSIsContainer) {
      [void]$archive.CreateEntry("$entryName/")
      Add-ZipTree $archive $item.FullName $entryName
    } else {
      [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $archive, $item.FullName, $entryName,
        [System.IO.Compression.CompressionLevel]::Optimal)
    }
  }
}

function Zip-Project([string]$name) {
  $src = Join-Path $docs $name
  if (-not (Test-Path $src)) { throw "Missing $src" }
  $zip = Join-Path $dest "$name.zip"
  $temp = Join-Path $env:TEMP "gtaiv-zip-$name-$stamp"
  if (Test-Path $temp) { Remove-Item $temp -Recurse -Force }
  $stage = Join-Path $temp $name
  New-Item -ItemType Directory -Force -Path $stage | Out-Null

  # Copy including hidden (.git); exclude heavy/build dirs and binary artifacts
  & robocopy $src $stage /E /XD .vs out build bin dxvk /XF *.user *.suo *.obj *.pdb *.dll *.exe *.asi *.log /NFL /NDL /NJH /NJS /nc /ns /np
  if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $name code=$LASTEXITCODE" }

  if (Test-Path $zip) { Remove-Item $zip -Force }
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $fs = [System.IO.File]::Open($zip, [System.IO.FileMode]::CreateNew)
  try {
    $archive = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
      # Zip folder named $name so extract yields Documents\<name>\...
      Add-ZipTree $archive $stage $name
    } finally {
      $archive.Dispose()
    }
  } finally {
    $fs.Dispose()
  }

  Remove-Item $temp -Recurse -Force
  $item = Get-Item $zip
  "{0} = {1:N2} MB" -f $item.Name, ($item.Length / 1MB)
}

Zip-Project "gtaiv-openxr"
Zip-Project "gtaiv-dxvk-vr"
"DEST=$dest"
Get-ChildItem $dest | ForEach-Object { "{0}`t{1:N2} MB" -f $_.Name, ($_.Length / 1MB) }
