<#
Runs a presenting program for a few seconds and reports the GPU 3D load of
every process meanwhile (the "GPU Engine" counters): a compositor that copies
the window shows up as load of dwm.exe, a flip does not.

  .\tools\dev\test-flip-probe.ps1 -Exe build\probes\d3d11_flip_probe.exe -Arguments '10','flip_discard','1'
  -Seconds   how long to sample (default 6), after -Delay seconds (default 3)
  -Env       NAME=value pairs for the program
#>
param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @(), [int]$Delay = 3, [int]$Seconds = 6, [string[]]$Env = @())
$ErrorActionPreference = 'Stop'
$start = [Diagnostics.ProcessStartInfo]::new((Resolve-Path $Exe).Path, ($Arguments -join ' '))
$start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
$start.WorkingDirectory = Split-Path (Resolve-Path $Exe).Path
foreach ($pair in $Env) { $name, $value = $pair -split '=', 2; $start.EnvironmentVariables[$name] = $value }
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
Start-Sleep -Seconds $Delay
$sum = @{}; $samples = 0
$ErrorActionPreference = 'Continue'
for ($i = 0; $i -lt $Seconds -and -not $process.HasExited; ++$i) {
   $set = Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -SampleInterval 1 -MaxSamples 1 -ErrorAction SilentlyContinue
   if (-not $set) { continue }
   ++$samples
   foreach ($s in $set.CounterSamples) {
      if ($s.InstanceName -match 'pid_(\d+)_') { $id = [int]$Matches[1]; $sum[$id] = [double]$sum[$id] + $s.CookedValue }
   }
}
if (-not $process.WaitForExit(60000)) { $process.Kill(); 'program killed after 60 s' }
$stdout.Result.Trim(); if ($stderr.Result.Trim()) { $stderr.Result.Trim() }
"exit code $($process.ExitCode); GPU 3D load over $samples s:"
$total = 0.0
$sum.GetEnumerator() | Where-Object { $_.Value / [math]::Max($samples, 1) -ge 0.3 } | Sort-Object Value -Descending | ForEach-Object {
   $name = try { (Get-Process -Id $_.Key -ErrorAction Stop).ProcessName } catch { if ($_.Key -eq $process.Id) { 'the program' } else { "pid $($_.Key)" } }
   if ($_.Key -eq $process.Id) { $name = 'the program' }
   $avg = $_.Value / [math]::Max($samples, 1); $total += $avg
   '  {0,5:N1} %  {1}' -f $avg, $name
}
'  {0,5:N1} %  total' -f $total
