# Continuous rendering through the system-registered ICD (installed copy):
# vkcubepp for -Seconds at the display rate (FIFO), its window closes itself
# after the frame count (--c). Every 5 s: private memory, OS handles, GDI and
# USER objects, CPU time. After a 30 s warm-up nothing may grow beyond noise
# (private memory +1 MiB, handles +4, GDI/USER objects +0). A watchdog kills
# the process if it does not finish in time.
# Report: native\artifacts\stability-vkcubepp-<seconds>s.json.
param([int]$Seconds = 180, [int]$Hz = 60, [string]$Cube = 'C:\VulkanSDK-local\Bin\vkcubepp.exe')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Cube)) { throw "vkcubepp not found: $Cube" }
if (-not ('StabilityGui' -as [type])) { Add-Type @'
using System; using System.Runtime.InteropServices;
public static class StabilityGui { [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags); }
'@ }
$frames = $Seconds * $Hz
$start = [Diagnostics.ProcessStartInfo]::new($Cube, "--c $frames")
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
foreach ($name in 'VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES') { $start.EnvironmentVariables.Remove($name) }
$start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '*'
$p = [Diagnostics.Process]::Start($start)
$out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
$watch = [Diagnostics.Stopwatch]::StartNew()
$samples = @()
$timedOut = $false
while (-not $p.WaitForExit(5000)) {
    $p.Refresh()
    $samples += [pscustomobject][ordered]@{ t = [math]::Round($watch.Elapsed.TotalSeconds); privateMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 2);
        handles = $p.HandleCount; gdi = [StabilityGui]::GetGuiResources($p.Handle, 0); user = [StabilityGui]::GetGuiResources($p.Handle, 1);
        cpu = [math]::Round($p.TotalProcessorTime.TotalSeconds, 1) }
    if ($watch.Elapsed.TotalSeconds -gt $Seconds * 1.5 + 60) { $timedOut = $true; try { $p.Kill() } catch {}; $p.WaitForExit(); break }
}
$elapsed = [math]::Round($watch.Elapsed.TotalSeconds, 1)
$steady = @($samples | Where-Object { $_.t -ge 30 })
$growth = $null
if ($steady.Count -ge 2) {
    $first = $steady[0]; $last = $steady[-1]
    $growth = [ordered]@{ privateMB = [math]::Round($last.privateMB - $first.privateMB, 2); handles = $last.handles - $first.handles;
        gdi = [int]$last.gdi - [int]$first.gdi; user = [int]$last.user - [int]$first.user;
        maxPrivateMB = ($steady | Measure-Object privateMB -Maximum).Maximum; minPrivateMB = ($steady | Measure-Object privateMB -Minimum).Minimum }
}
$cpuShare = if ($samples.Count) { [math]::Round($samples[-1].cpu / $samples[-1].t * 100, 1) } else { 0 }
$passed = -not $timedOut -and $p.ExitCode -eq 0 -and $growth -and $growth.privateMB -le 1 -and $growth.handles -le 4 -and $growth.gdi -le 0 -and $growth.user -le 0
$report = [ordered]@{ schema = 1; testedUtc = [DateTime]::UtcNow.ToString('o'); icd = 'system-registered';
    icdSha256 = (Get-FileHash (Join-Path $env:ProgramFiles 'HasVK-WDDM\vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
    seconds = $Seconds; frames = $frames; elapsedSeconds = $elapsed; exitCode = $p.ExitCode; timedOut = $timedOut;
    cpuPercentOfOneCore = $cpuShare; growthAfterWarmup = $growth; passed = $passed; samples = $samples;
    stderrTail = (($err.Result -split "`n") | Where-Object { $_ -and $_ -notmatch 'SIMD\d+ shader' } | Select-Object -Last 10) }
$path = Join-Path $taskRoot "native\artifacts\stability-vkcubepp-${Seconds}s.json"
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $path -Encoding UTF8
$summary = "$frames frames in $elapsed s, exit $($p.ExitCode), CPU $cpuShare% of a core; after warm-up: private $($growth.privateMB) MiB, handles $($growth.handles), GDI $($growth.gdi), USER $($growth.user)"
if (-not $passed) { throw "Stability check failed: $summary (see $path)" }
Write-Output "PASS: $summary"
