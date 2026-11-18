# Unattended GravityMark run: a modal error box of GravityMark is read and
# closed automatically (no click needed), the process is killed at -Seconds.
#   .\tools\run-gravitymark.ps1 -Api vk -Asteroids 20000            # through this ICD
#   .\tools\run-gravitymark.ps1 -Api d3d11 -Asteroids 20000         # Intel's own driver
# Report: native\artifacts\gravitymark-<api>.json (exit code, frame times, FPS, errors).
param([ValidateSet('vk', 'd3d11', 'd3d12', 'gl')][string]$Api = 'vk', [int]$Asteroids = 20000, [int]$Width = 1280, [int]$Height = 720,
    [int]$Seconds = 240, [string]$Bin = 'C:\GravityMark-extract\GravityMark\bin', [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
if (-not ('GmDialog' -as [type])) { Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class GmDialog {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  /* Text of the first dialog box (#32770) of process pid, which is then closed; null if none. */
  public static string CloseDialog(uint pid) {
    string found = null;
    EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner); if (owner != pid) return true;
      var cls = new StringBuilder(64); GetClassName(h, cls, 64); if (cls.ToString() != "#32770") return true;
      var text = new StringBuilder();
      EnumChildWindows(h, (c, l2) => { var t = new StringBuilder(1024); GetWindowText(c, t, 1024); if (t.Length > 0) text.Append(t).Append(" | "); return true; }, IntPtr.Zero);
      found = text.ToString(); PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); return false;
    }, IntPtr.Zero);
    return found;
  }
}
'@ }
$times = Join-Path (Split-Path $Bin -Parent) "times_$Api.txt"
if (Test-Path -LiteralPath $times) { Remove-Item -LiteralPath $times -Confirm:$false }
$arguments = "-$Api -temporal 1 -fps 1 -info 1 -benchmark 1 -close 1 -count 1 -width $Width -height $Height -vsync 0 -asteroids $Asteroids -times $times $Extra"
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path $Bin 'GravityMark.exe'), $arguments)
$start.WorkingDirectory = $Bin; $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
if ($Api -eq 'vk') {
    $start.EnvironmentVariables['VK_DRIVER_FILES'] = Join-Path $taskRoot 'build\icd\intel_hasvk_wddm_icd.json'
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    $start.EnvironmentVariables['HASVK_WDDM_ASSERTS_TO_STDERR'] = '1'
    $start.EnvironmentVariables['HASVK_WDDM_PRESENT_STATS'] = '1'
}
$watch = [Diagnostics.Stopwatch]::StartNew()
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
$dialogs = @(); $timedOut = $false
while (-not $process.WaitForExit(500)) {
    $text = [GmDialog]::CloseDialog([uint32]$process.Id)
    if ($text) { $dialogs += $text }
    if ($watch.Elapsed.TotalSeconds -gt $Seconds) { $timedOut = $true; $process.Kill(); $process.WaitForExit(); break }
}
$log = ($stdout.Result + $stderr.Result) -split "`r?`n" | Where-Object { $_.Trim() }
$frames = @()
if (Test-Path -LiteralPath $times) { $frames = @(Get-Content -LiteralPath $times | ForEach-Object { if ($_ -match '([0-9]+(\.[0-9]+)?)\s*$') { [double]$Matches[1] } }) }
$result = [ordered]@{api = $Api; asteroids = $Asteroids; width = $Width; height = $Height; exitCode = $process.ExitCode; timedOut = $timedOut;
    seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1); dialogs = $dialogs; frames = $frames.Count;
    errors = @($log | Where-Object { $_ -match '^E:' } | Select-Object -First 20); tail = @($log | Select-Object -Last 15)}
if ($frames.Count) { $result.timesFileHead = @(Get-Content -LiteralPath $times -TotalCount 3) }
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskRoot "native\artifacts\gravitymark-$Api.json") -Encoding UTF8
$result | ConvertTo-Json -Depth 4
