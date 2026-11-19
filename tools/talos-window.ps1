<#
Measures a running Talos benchmark inside its measured window; started by
run-talos.ps1 (-Sample, -RipModule), not by hand. Waits for a Talos process
started after this script and for "Starting automated benchmark" in its log,
then 3 s more, and measures for -Seconds (the benchmark lasts 30 s).

  -Mode counters (default): per-thread CPU of Talos.exe and machine counters.
     Thread CPU comes from QueryThreadCycleTime (TSC cycles while running, exact);
     .NET TotalProcessorTime / GetThreadTimes are updated per timer tick and
     undercount short bursts several times over. Counters: CPU actual frequency,
     % Processor Performance, % Performance Limit and its flags, % Processor
     Time, GPU 3D engine utilization in total, of Talos and of dwm.exe, and
     dwm's CPU. PDH resolves counter names in the thread's UI language: a
     process started with a Russian UI finds no "\Processor Information"
     counters, hence SetThreadUILanguage(English) below.
  -Mode rip: build\probes\rip_sampler.exe on Talos with -Module on the stack
     (stacks, hottest functions and lines; build it with tools\build-probes.ps1).
#>
param([Parameter(Mandatory)][string]$Out, [ValidateSet('counters', 'rip')][string]$Mode = 'counters',
      [string]$Module = 'vulkan_hasvk_wddm.dll', [string]$Symbols = '', [int]$Seconds = 24,
      [long]$NotBefore = 0,   # FileTime (UTC): the game process must start after it (run-talos passes its start)
      [string]$Log = 'C:\Program Files (x86)\Steam\steamapps\common\The Talos Principle\Log\Talos.log')
$ErrorActionPreference = 'Stop'
$begun = if ($NotBefore) { [DateTime]::FromFileTimeUtc($NotBefore).ToLocalTime() } else { Get-Date }
Add-Type -Namespace TalosWindow -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern ushort SetThreadUILanguage(ushort id);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr OpenThread(uint access, bool inherit, uint id);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool QueryThreadCycleTime(System.IntPtr h, out ulong cycles);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern bool CloseHandle(System.IntPtr h);
public static ulong Cycles(uint id) { System.IntPtr h = OpenThread(0x0800, false, id); if (h == System.IntPtr.Zero) return 0; ulong c; QueryThreadCycleTime(h, out c); CloseHandle(h); return c; }
'@
[void][TalosWindow.Native]::SetThreadUILanguage(0x409)

$deadline = (Get-Date).AddSeconds(300)
$game = $null
while ((Get-Date) -lt $deadline -and -not $game) {
   $game = Get-Process Talos -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -gt $begun } | Select-Object -First 1
   if (-not $game) { Start-Sleep -Milliseconds 200 }
}
if (-not $game) { "no Talos process started" | Set-Content -LiteralPath $Out -Encoding utf8; exit 1 }
$started = $false
while ((Get-Date) -lt $deadline -and -not $started -and -not $game.HasExited) {
   try { $started = (Get-Item -LiteralPath $Log).LastWriteTime -gt $game.StartTime -and
                    [bool](Select-String -LiteralPath $Log -Pattern 'Starting automated benchmark' -SimpleMatch -Quiet) } catch {}
   if (-not $started) { Start-Sleep -Milliseconds 200 }
}
if (-not $started) { "the benchmark did not start" | Set-Content -LiteralPath $Out -Encoding utf8; exit 1 }
Start-Sleep -Seconds 3

if ($Mode -eq 'rip') {
   $exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\probes\rip_sampler.exe'
   if (-not (Test-Path -LiteralPath $exe)) { "rip_sampler.exe missing: run tools\build-probes.ps1" | Set-Content -LiteralPath $Out -Encoding utf8; exit 1 }
   & $exe $game.Id $Seconds $Symbols $Module *> $Out
   exit 0
}

function Snapshot {
   $threads = @{}
   foreach ($thread in (Get-Process -Id $game.Id).Threads) { $threads[$thread.Id] = [TalosWindow.Native]::Cycles([uint32]$thread.Id) }
   # Other processes: CPU by process times (timer ticks: fine for seconds of CPU).
   $processes = @{}
   foreach ($p in Get-Process) { try { $processes["$($p.ProcessName) $($p.Id)"] = $p.TotalProcessorTime.TotalSeconds } catch {} }
   @{ when = [Diagnostics.Stopwatch]::GetTimestamp(); threads = $threads; processes = $processes }
}
$dwm = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
$paths = @('\Processor Information(_Total)\Actual Frequency', '\Processor Information(_Total)\% Processor Performance',
           '\Processor Information(_Total)\% Performance Limit', '\Processor Information(_Total)\Performance Limit Flags',
           '\Processor Information(_Total)\% Processor Time', '\Processor Information(_Total)\% Privileged Time',
           '\Processor Information(_Total)\% DPC Time', '\Processor Information(_Total)\% Interrupt Time',
           '\GPU Engine(*engtype_3D)\Utilization Percentage', '\Process(dwm)\% Processor Time')
$a = Snapshot
$samples = @(Get-Counter -Counter $paths -SampleInterval 1 -MaxSamples $Seconds -ErrorAction SilentlyContinue)
$b = Snapshot
$wall = ($b.when - $a.when) / [Diagnostics.Stopwatch]::Frequency
$tsc = 2.7e9   # invariant TSC of the i5-5257U (2.700 GHz, measured by HASVK_WDDM_TIMELINE)
$sum = @{}; $flags = @{}; $gpu = 0.0; $gameGpu = 0.0; $dwmGpu = 0.0; $n = 0
foreach ($set in $samples) {
   if (-not $set.CounterSamples) { continue }
   ++$n
   foreach ($c in $set.CounterSamples) {
      $path = $c.Path.ToLowerInvariant()
      if ($path -like '*engtype_3d*') {
         $gpu += $c.CookedValue
         if ($path -like "*pid_$($game.Id)_*") { $gameGpu += $c.CookedValue }
         elseif ($dwm -and $path -like "*pid_$($dwm.Id)_*") { $dwmGpu += $c.CookedValue }
      } elseif ($path -like '*limit flags') { $flags[[int]$c.CookedValue] = 1 + [int]$flags[[int]$c.CookedValue] }
      else { $name = ($path -split '\\')[-2..-1] -join ' '; $sum[$name] = [double]$sum[$name] + $c.CookedValue }
   }
}
$lines = @("Talos $($game.Id): $([math]::Round($wall, 1)) s measured, starting 3 s after the benchmark start")
if ($n) {
   $lines += "counters (averages of $n samples):"
   foreach ($key in $sum.Keys | Sort-Object) { $lines += "  $key = $([math]::Round($sum[$key] / $n, 1))" }
   $lines += "  GPU 3D: total $([math]::Round($gpu / $n, 1)) %, Talos $([math]::Round($gameGpu / $n, 1)) %, dwm $([math]::Round($dwmGpu / $n, 1)) %"
   $lines += "  performance limit flags: " + (($flags.Keys | Sort-Object | ForEach-Object { '0x{0:x} x{1}' -f $_, $flags[$_] }) -join ', ')
} else { $lines += "no counter samples (PDH)" }
$rows = foreach ($id in $b.threads.Keys) { if ($a.threads.ContainsKey($id)) { [pscustomobject]@{ Thread = $id; Pct = ($b.threads[$id] - $a.threads[$id]) / $tsc / $wall * 100 } } }
$lines += "threads of Talos.exe, % of one logical CPU (QueryThreadCycleTime): all $([math]::Round(($rows | Measure-Object Pct -Sum).Sum, 1)) %"
$lines += $rows | Sort-Object Pct -Descending | Select-Object -First 10 | ForEach-Object { "  thread $($_.Thread): $([math]::Round($_.Pct, 1)) %" }
$others = foreach ($key in $b.processes.Keys) {
   if ($a.processes.ContainsKey($key) -and $key -ne "Talos $($game.Id)") {
      [pscustomobject]@{ Process = $key; Pct = ($b.processes[$key] - $a.processes[$key]) / $wall * 100 }
   }
}
$lines += "other processes, % of one logical CPU (process times): all $([math]::Round(($others | Measure-Object Pct -Sum).Sum, 1)) %"
$lines += $others | Sort-Object Pct -Descending | Select-Object -First 8 | Where-Object { $_.Pct -ge 0.5 } |
   ForEach-Object { "  $($_.Process): $([math]::Round($_.Pct, 1)) %" }
$lines | Set-Content -LiteralPath $Out -Encoding utf8
