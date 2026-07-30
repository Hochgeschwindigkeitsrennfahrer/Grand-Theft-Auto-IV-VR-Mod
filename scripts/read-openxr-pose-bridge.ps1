[CmdletBinding()]
param(
  [string]$MappingName = "Local\GTAIV_XR_PoseBridge_v1",

  [ValidateRange(1, 100)]
  [uint32]$Attempts = 20
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
  $MappingName,
  [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read
)
$view = $null
try {
  $view = $mapping.CreateViewAccessor(
    0,
    384,
    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read
  )

  $bytes = [byte[]]::new(384)
  $stable = $false
  for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
    $before = $view.ReadInt32(16)
    if (($before -band 1) -ne 0) {
      Start-Sleep -Milliseconds 5
      continue
    }

    [void]$view.ReadArray(0, $bytes, 0, $bytes.Length)
    $after = $view.ReadInt32(16)
    if ($before -eq $after -and ($after -band 1) -eq 0) {
      $stable = $true
      break
    }
    Start-Sleep -Milliseconds 5
  }
  if (-not $stable) {
    throw "Could not read a stable OpenXR PoseBridge publication."
  }

  function Read-UInt32([int]$Offset) {
    [BitConverter]::ToUInt32($bytes, $Offset)
  }
  function Read-UInt64([int]$Offset) {
    [BitConverter]::ToUInt64($bytes, $Offset)
  }
  function Read-Int64([int]$Offset) {
    [BitConverter]::ToInt64($bytes, $Offset)
  }
  function Read-Float([int]$Offset) {
    [BitConverter]::ToSingle($bytes, $Offset)
  }
  function Read-Pose([int]$Offset) {
    [ordered]@{
      orientation = @(
        (Read-Float ($Offset + 0))
        (Read-Float ($Offset + 4))
        (Read-Float ($Offset + 8))
        (Read-Float ($Offset + 12))
      )
      position = @(
        (Read-Float ($Offset + 16))
        (Read-Float ($Offset + 20))
        (Read-Float ($Offset + 24))
      )
    }
  }
  function Read-Controller([int]$Offset) {
    [ordered]@{
      gripPose = Read-Pose $Offset
      aimPose = Read-Pose ($Offset + 32)
      trigger = Read-Float ($Offset + 64)
      squeeze = Read-Float ($Offset + 68)
      thumbstick = @(
        (Read-Float ($Offset + 72))
        (Read-Float ($Offset + 76))
      )
      buttons = "0x{0:X8}" -f (Read-UInt32 ($Offset + 80))
      activeFlags = "0x{0:X8}" -f (Read-UInt32 ($Offset + 84))
    }
  }

  [ordered]@{
    magic = "0x{0:X8}" -f (Read-UInt32 0)
    version = Read-UInt32 4
    structBytes = Read-UInt32 8
    producerPid = Read-UInt32 12
    publicationSequence = [BitConverter]::ToInt32($bytes, 16)
    flags = "0x{0:X8}" -f (Read-UInt32 20)
    producerEpoch = Read-UInt64 24
    frameId = Read-UInt64 32
    predictedDisplayTime = Read-Int64 40
    heartbeatTickMs = Read-UInt64 48
    referenceSpaceGeneration = Read-UInt32 56
    recenterRequestId = Read-UInt32 60
    hmdPose = Read-Pose 64
    leftController = Read-Controller 192
    rightController = Read-Controller 288
  } | ConvertTo-Json -Depth 10
}
finally {
  if ($null -ne $view) {
    $view.Dispose()
  }
  $mapping.Dispose()
}
