<#
Plays a Dota 2 replay as a timedemo (every frame rendered as fast as the
game can, no wall-clock pacing) unattended and reports the game's own result
(Source2Bench.csv) and console lines.

Always launched with -insecure: VAC stays off for these runs, nothing here
touches matchmaking. The player's Dota configs (Steam userdata 570 cfg,
user_convars/user_keys) are saved before and put back after the run, because
command-line convars (fps_max ...) are written into them on exit.

Never waits for a person: a modal dialog, a crash or -TimeoutSeconds ends the
run (the process is killed) and it is reported as failed.

  -Api Vulkan|Direct3D11
  -Icd <json>        Vulkan only: VK_DRIVER_FILES (default: build\icd; "installed"
                     uses the registered ICD)
  -Demo name         replay in game\dota\replays (default 9093360730)
  -Start/-End tick   timedemo window (timedemo_start/timedemo_end); 30 ticks a second
  -Env NAME=v,...    extra environment variables
  -Extra '...'       extra command line (+cvar value, -switch ...)  -Tag name          output name in build\dota
  -Sample            per-thread CPU, CPU frequency and GPU load for -SampleSeconds,
                     starting -SampleDelay s after the map has spawned (the replay
                     then still fast-forwards to -Start for a few seconds; the
                     measured window follows); report build\dota\<Tag>.sample.txt
  -RipModule dll     instead, stacks with build\probes\rip_sampler.exe (with that
                     module on the stack; vulkan_hasvk_wddm.dll, igd10iumd64.dll for
                     Intel's DX11 driver); report build\dota\<Tag>.rip.txt
  -D3D11Proxy dll    Direct3D11 only: the timing proxy (native\tests\d3d11_proxy,
                     build\probes\d3d11-proxy\d3d11.dll) is copied next to dota2.exe
                     for this run and removed afterwards; its GPU time per render
                     target goes to build\dota\<Tag>.d3d11-profile.txt
#>
param(
   [ValidateSet('Vulkan', 'Direct3D11')][string]$Api = 'Vulkan',
   [string]$Icd = '',
   [string]$Demo = '9093360730',
   [int]$Start = 30000,
   [int]$End = 33000,
   [string[]]$Env = @(),
   [string]$Extra = '',
   [string]$Tag = '',
   [int]$TimeoutSeconds = 600,
   [switch]$Sample,
   [string]$RipModule = '',
   [int]$SampleDelay = 25,
   [int]$SampleSeconds = 30,
   [string]$D3D11Proxy = '',
   [string]$Game ='C:\Program Files (x86)\Steam\steamapps\common\dota 2 beta'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $Game 'game\bin\win64\dota2.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Dota 2 not found: $exe" }
$dota = Join-Path $Game 'game\dota'
if (-not (Test-Path -LiteralPath (Join-Path $dota "replays\$Demo.dem"))) { throw "Replay not found: replays\$Demo.dem" }
if (Get-Process dota2 -ErrorAction SilentlyContinue) { throw 'Dota 2 is already running' }
$out = Join-Path $root 'build\dota'
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $Tag) { $Tag = $Api }

Add-Type -Namespace DotaRun -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, System.IntPtr lparam);
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int size);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumChildWindows(System.IntPtr parent, EnumProc proc, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowText(System.IntPtr hwnd, System.Text.StringBuilder text, int size);
public static string DialogText = "";
public static bool HasDialog(uint pid) {
   bool found = false;
   EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      var name = new System.Text.StringBuilder(64); GetClassName(h, name, 64);
      if (name.ToString() != "#32770") return true;
      found = true;
      var all = new System.Text.StringBuilder();
      var title = new System.Text.StringBuilder(512); GetWindowText(h, title, 512); all.Append(title).Append(" | ");
      EnumChildWindows(h, (c, x) => { var t = new System.Text.StringBuilder(1024); GetWindowText(c, t, 1024); if (t.Length > 0) all.Append(t).Append(" | "); return true; }, System.IntPtr.Zero);
      DialogText = all.ToString();
      return false;
   }, System.IntPtr.Zero);
   return found;
}
'@

$apiSwitch = if ($Api -eq 'Vulkan') { '-vulkan' } else { '-dx11' }
$cmdline = "-insecure -novid -nojoy -condebug $apiSwitch +fps_max 0 $Extra +timedemo replays/$Demo +timedemo_start $Start +timedemo_end $End " +
           '-testscript_inline "Test_WaitForCheckPoint DemoPlaybackFinished; quit"'
# The game appends a summary row to Source2BenchV2.csv and writes its own
# per-section profile of the timedemo (vprof lite) to timedemo_profile.csv.
$bench = Join-Path $dota 'Source2BenchV2.csv'
$profile = Join-Path $dota 'timedemo_profile.csv'
$console = Join-Path $dota 'console.log'
foreach ($f in $bench, $profile, $console) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }

# The player's settings, put back afterwards.
$saved = @{}
foreach ($f in @(Get-ChildItem 'C:\Program Files (x86)\Steam\userdata\*\570\local\cfg\*' -File -ErrorAction SilentlyContinue) +
               @(Get-ChildItem 'C:\Program Files (x86)\Steam\userdata\*\570\remote\user_*.vcfg' -File -ErrorAction SilentlyContinue)) {
   $saved[$f.FullName] = [IO.File]::ReadAllBytes($f.FullName)
}

$startInfo = [Diagnostics.ProcessStartInfo]::new($exe, $cmdline)
$startInfo.WorkingDirectory = Split-Path $exe
$startInfo.UseShellExecute = $false
# The game's and the driver's stdout/stderr go to build\dota\<Tag>.stdout.txt.
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
if ($Api -eq 'Vulkan' -and $Icd -ne 'installed') {
   if (-not $Icd) { $Icd = Join-Path $root 'build\icd\intel_hasvk_wddm_icd.json' }
   $startInfo.EnvironmentVariables['VK_DRIVER_FILES'] = (Resolve-Path $Icd).Path
}
foreach ($pair in ($Env | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
   $name, $value = $pair -split '=', 2
   $startInfo.EnvironmentVariables[$name] = $value
   "  env $name=$value"
}

$proxyTarget = Join-Path (Split-Path $exe) 'd3d11.dll'
$proxyPlaced = $false
if ($D3D11Proxy) {
   if ($Api -ne 'Direct3D11') { throw '-D3D11Proxy needs -Api Direct3D11' }
   if (Test-Path -LiteralPath $proxyTarget) { throw "$proxyTarget already exists; not overwriting the game's file" }
   $proxyLog = Join-Path $out "$Tag.d3d11-profile.txt"
   if (Test-Path -LiteralPath $proxyLog) { Remove-Item -LiteralPath $proxyLog -Force }
   $startInfo.EnvironmentVariables['D3D11_PROFILE_LOG'] = $proxyLog
}

$sampler = $null; $sampleOut = $null
if ($Sample -or $RipModule) {
   $mode = if ($RipModule) { 'rip' } else { 'counters' }
   $sampleOut = Join-Path $out ("$Tag." + $(if ($RipModule) { 'rip' } else { 'sample' }) + '.txt')
   if (Test-Path -LiteralPath $sampleOut) { Remove-Item -LiteralPath $sampleOut -Force }
   $samplerArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'talos-window.ps1')`"",
                    '-Out', "`"$sampleOut`"", '-Mode', $mode, '-NotBefore', (Get-Date).ToFileTimeUtc(), '-Process', 'dota2',
                    '-Log', "`"$console`"", '-Marker', 'DOTABaseGameMode::Spawn()', '-Delay', $SampleDelay, '-Seconds', $SampleSeconds)
   if ($RipModule) { $samplerArgs += @('-Module', $RipModule, '-Symbols', "`"$(Join-Path $root 'build\icd')`"") }
   $sampler = Start-Process powershell.exe -ArgumentList $samplerArgs -WindowStyle Hidden -PassThru -RedirectStandardError "$sampleOut.err"
}
$verdict = ''
try {
   if ($D3D11Proxy) { Copy-Item -LiteralPath $D3D11Proxy -Destination $proxyTarget; $proxyPlaced = $true }
   $watch = [Diagnostics.Stopwatch]::StartNew()
   $p = [Diagnostics.Process]::Start($startInfo)
   $stdout = $p.StandardOutput.ReadToEndAsync()
   $stderr = $p.StandardError.ReadToEndAsync()
   while (-not $p.WaitForExit(1000)) {
      if ($watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { $verdict = "timeout after $TimeoutSeconds s"; break }
      if ([DotaRun.Win]::HasDialog([uint32]$p.Id)) {
         Start-Sleep 2; [void][DotaRun.Win]::HasDialog([uint32]$p.Id)
         $verdict = 'modal dialog: ' + [DotaRun.Win]::DialogText; break
      }
   }
} finally {
   if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {}; $p.WaitForExit() }
   if ($proxyPlaced) { Remove-Item -LiteralPath $proxyTarget -Force; "  proxy removed: $proxyTarget" }
   foreach ($path in $saved.Keys) { [IO.File]::WriteAllBytes($path, $saved[$path]) }
}
$wall = [math]::Round($watch.Elapsed.TotalSeconds)
if ($stdout) { [IO.File]::WriteAllText((Join-Path $out "$Tag.stdout.txt"), $stdout.Result + "`n" + $stderr.Result) }
if (Test-Path -LiteralPath $console) { Copy-Item -LiteralPath $console (Join-Path $out "$Tag.console.log") -Force }
if (Test-Path -LiteralPath $bench) { Copy-Item -LiteralPath $bench (Join-Path $out "$Tag.Source2BenchV2.csv") -Force }
if (Test-Path -LiteralPath $profile) { Copy-Item -LiteralPath $profile (Join-Path $out "$Tag.timedemo_profile.csv") -Force }
"$Api [$Tag]: exit=$($p.ExitCode) wall=${wall}s $verdict"
# "[Demo] 2999 frames 111.444 seconds 26.91 fps (37.16 ms/f) 2.294 fps variability, P5/50/95 frame time ..."
$result = Get-Content -LiteralPath $console -ErrorAction SilentlyContinue | Select-String '\[Demo\] \d+ frames .* fps' | Select-Object -Last 1
if ($result) { '  ' + ($result.Line -replace '^.*\[Demo\]\s*', '') }
else { '  no timedemo result in console.log'; if (-not $verdict) { $verdict = 'no result' } }
if ($D3D11Proxy) { "  D3D11 profile: $proxyLog" }
if ($sampler) {
   if (-not $sampler.WaitForExit(120000)) { try { $sampler.Kill() } catch {} }
   if (Test-Path -LiteralPath $sampleOut) {
      "  measurement: $sampleOut"
      if (-not $RipModule) { Get-Content -LiteralPath $sampleOut | ForEach-Object { "    $_" } }
   } else { '  measurement: no report' }
}
if ($verdict -or $p.ExitCode) { exit 1 }
