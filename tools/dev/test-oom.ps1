# Device memory exhaustion outside CTS (native\tests\oom_probe.cpp): the heap
# is filled until vkAllocateMemory fails, which must be
# VK_ERROR_OUT_OF_DEVICE_MEMORY; with it full, pre-recorded and newly recorded
# work (20000 commands) must run correctly; after freeing, the same amount
# must fit again. The heap is shrunk to -HeapMiB (HASVK_WDDM_HEAP_MB) so the
# machine's own memory is not exhausted; the code paths are the same.
param([switch]$SkipBuild, [int]$HeapMiB = 512)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SkipBuild) { & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-native.ps1') -BuildOnly; & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-icd.ps1') -SkipBuild | Out-Null }
$taskPackage = Join-Path $taskRoot 'build\icd'
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path $taskPackage 'IrisOomProbe.exe'))
$start.WorkingDirectory = $taskPackage
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.EnvironmentVariables['VK_DRIVER_FILES'] = Join-Path $taskPackage 'intel_hasvk_wddm_icd.json'
$start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '*'
$start.EnvironmentVariables['HASVK_WDDM_HEAP_MB'] = "$HeapMiB"
$p = [Diagnostics.Process]::Start($start)
$out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {}; $p.WaitForExit(); throw 'OOM probe did not finish in 120 s (killed)' }
$text = $out.Result
$report = Join-Path $taskRoot 'native\artifacts\oom-validation.txt'
[IO.File]::WriteAllText($report, $text + "`n--- stderr`n" + $err.Result, [Text.UTF8Encoding]::new($false))
if ($p.ExitCode -ne 0) { throw "OOM check failed (exit $($p.ExitCode)); see $report" }
$filled = ($text -split "`n" | Where-Object { $_ -like 'filled:*' } | Select-Object -First 1).Trim()
Write-Output "PASS: heap of $HeapMiB MiB exhausted with VK_ERROR_OUT_OF_DEVICE_MEMORY, work runs with it full and after freeing ($filled)"
