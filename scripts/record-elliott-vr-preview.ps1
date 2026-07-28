[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$WalkMarkerPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [uint32]$LauncherPid,
  [ValidateRange(10, 12)]
  [uint32]$DurationSeconds = 12,
  [ValidateRange(30, 60)]
  [uint32]$FrameRate = 60,
  [ValidateRange(60, 600)]
  [uint32]$WaitSeconds = 360,
  [string]$FfmpegPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
  $FfmpegPath = (Get-Command ffmpeg -ErrorAction Stop).Source
}
$FfmpegPath = (Resolve-Path -LiteralPath $FfmpegPath).Path
$WalkMarkerPath = [IO.Path]::GetFullPath($WalkMarkerPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
  New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
$ffmpegLog = "$OutputPath.ffmpeg.log"
$captureReceipt = "$OutputPath.capture.txt"

if (-not ("ElliottPreviewCapture.NativeMethods" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace ElliottPreviewCapture
{
    public static class NativeMethods
    {
        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(
            EnumWindowsProc callback,
            IntPtr parameter);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(
            IntPtr hwnd,
            out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(
            IntPtr hwnd,
            StringBuilder className,
            int maximumCount);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hwnd, int command);

        [DllImport("user32.dll")]
        private static extern bool SetWindowPos(
            IntPtr hwnd,
            IntPtr insertAfter,
            int x,
            int y,
            int width,
            int height,
            uint flags);

        private const int SW_HIDE = 0;
        private const int SW_SHOWNOACTIVATE = 4;
        private const uint SWP_NOSIZE = 0x0001;
        private const uint SWP_NOMOVE = 0x0002;
        private const uint SWP_NOACTIVATE = 0x0010;
        private const uint SWP_SHOWWINDOW = 0x0040;

        public static IntPtr FindSimulatorPreview(uint processId)
        {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr hwnd, IntPtr parameter)
            {
                uint owner;
                GetWindowThreadProcessId(hwnd, out owner);
                if (owner != processId)
                    return true;
                StringBuilder className = new StringBuilder(256);
                GetClassName(hwnd, className, className.Capacity);
                if (className.ToString().IndexOf(
                        "OpenXR Simulator",
                        StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    found = hwnd;
                    return false;
                }
                return true;
            }, IntPtr.Zero);
            return found;
        }

        public static void ShowWithoutActivation(IntPtr hwnd)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(
                hwnd,
                IntPtr.Zero,
                0,
                0,
                0,
                0,
                SWP_NOMOVE
                    | SWP_NOSIZE
                    | SWP_NOACTIVATE
                    | SWP_SHOWWINDOW);
        }

        public static void Hide(IntPtr hwnd)
        {
            ShowWindow(hwnd, SW_HIDE);
        }
    }
}
"@
}

$markerDirectory = Split-Path -Parent $WalkMarkerPath
if (-not (Test-Path -LiteralPath $markerDirectory -PathType Container)) {
  throw "Walk-marker directory was not found: $markerDirectory"
}
$markerName = Split-Path -Leaf $WalkMarkerPath
$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$watcher = New-Object IO.FileSystemWatcher($markerDirectory, $markerName)
$watcher.IncludeSubdirectories = $false
$watcher.NotifyFilter =
  [IO.NotifyFilters]::FileName -bor [IO.NotifyFilters]::LastWrite
$watcher.EnableRaisingEvents = $true
try {
  while (-not (Test-Path -LiteralPath $WalkMarkerPath -PathType Leaf)) {
    if (-not (Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue)) {
      throw "The supervised launcher exited before the walk marker."
    }
    $remainingMilliseconds = [int][Math]::Ceiling(
      ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -le 0) {
      break
    }
    $waitMilliseconds = [Math]::Min(1000, $remainingMilliseconds)
    [void]$watcher.WaitForChanged(
      [IO.WatcherChangeTypes]::All,
      $waitMilliseconds)
  }
}
finally {
  $watcher.EnableRaisingEvents = $false
  $watcher.Dispose()
}
if (-not (Test-Path -LiteralPath $WalkMarkerPath -PathType Leaf)) {
  throw "Timed out waiting for the proved in-world walk marker."
}
$walkMarker = (
  Get-Content -LiteralPath $WalkMarkerPath -Raw -ErrorAction Stop
).Trim()
if ([string]::IsNullOrWhiteSpace($walkMarker)) {
  throw "The in-world walk marker was empty."
}

$hostProcess = Get-Process -Name gtaiv_xr_host -ErrorAction Stop |
  Sort-Object StartTime -Descending |
  Select-Object -First 1
$windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
$hwnd = [IntPtr]::Zero
while ([DateTime]::UtcNow -lt $windowDeadline) {
  $hwnd =
    [ElliottPreviewCapture.NativeMethods]::FindSimulatorPreview(
      [uint32]$hostProcess.Id)
  if ($hwnd -ne [IntPtr]::Zero) {
    break
  }
  Start-Sleep -Milliseconds 25
}
if ($hwnd -eq [IntPtr]::Zero) {
  throw "The hidden Elliott composed-preview HWND was not found."
}

$started = Get-Date
try {
  [ElliottPreviewCapture.NativeMethods]::ShowWithoutActivation($hwnd)
  $source =
    "gfxcapture=hwnd=$($hwnd.ToInt64()):capture_cursor=0:" +
    "capture_border=0:display_border=0:max_framerate=${FrameRate}:" +
    "width=3840:height=1920:resize_mode=scale_aspect"
  $ffmpegOutput = @()
  $ffmpegExitCode = -1
  $previousErrorActionPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = "Continue"
    $ffmpegOutput = @(
      & $FfmpegPath `
        -hide_banner `
        -loglevel info `
        -y `
        -f lavfi `
        -i $source `
        -t $DurationSeconds `
        -an `
        -c:v h264_nvenc `
        -preset p5 `
        -tune hq `
        -rc vbr `
        -cq 18 `
        -b:v 25M `
        -maxrate 40M `
        -bufsize 80M `
        -metadata:s:v:0 stereo_mode=left_right `
        -movflags +faststart `
        $OutputPath 2>&1
    )
    $ffmpegExitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }
  $ffmpegOutput | Set-Content -LiteralPath $ffmpegLog -Encoding UTF8
  if ($ffmpegExitCode -ne 0) {
    throw "ffmpeg capture failed with exit code $ffmpegExitCode."
  }
}
finally {
  [ElliottPreviewCapture.NativeMethods]::Hide($hwnd)
}

if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
  throw "ffmpeg exited without producing the requested video."
}
$video = Get-Item -LiteralPath $OutputPath
if ($video.Length -le 1024) {
  throw "The captured video is unexpectedly small: $($video.Length) bytes."
}

@(
  "capture=PASS"
  "source=Elliott OpenXR composed SBS preview"
  "simulatorInput=CLI/headless"
  "windowActivation=False"
  "hwnd=$($hwnd.ToInt64())"
  "hostPid=$($hostProcess.Id)"
  "launcherPid=$LauncherPid"
  "durationSeconds=$DurationSeconds"
  "frameRate=$FrameRate"
  "started=$($started.ToString('o'))"
  "walkMarker=$walkMarker"
  "output=$OutputPath"
  "bytes=$($video.Length)"
) | Set-Content -LiteralPath $captureReceipt -Encoding UTF8

Write-Output "ELLIOTT VR VIDEO CAPTURE PASS"
Write-Output "Output: $OutputPath"
Write-Output "Receipt: $captureReceipt"
