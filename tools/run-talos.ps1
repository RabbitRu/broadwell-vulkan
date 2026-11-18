<#
Runs The Talos Principle's built-in benchmark (Demo level, 30 s bot run,
auto-quit) unattended and reports the FPS lines from Talos.log.

Never waits for a person: if the game writes "Crashed!" to its log (it then
shows a modal crash dialog), shows any other modal dialog, or exceeds
-TimeoutSeconds, the process is killed and the run is reported as failed.

  -Api Vulkan|Direct3D11
  -Icd <json>        Vulkan only: VK_DRIVER_FILES (default: build\icd; "installed"
                     uses the registered ICD)
  -Env NAME=v,...    extra environment variables (HASVK_WDDM_PROFILE_PASSES=1, ...)
  -Extra '...'       extra Talos command line (+cvar value ...)
  -Tag name          output name in build\talos (stdout/stderr and Talos.log copy)
#>
param(
   [ValidateSet('Vulkan', 'Direct3D11')][string]$Api = 'Vulkan',
   [string]$Icd = '',
   [string[]]$Env = @(),
   [string]$Extra = '',
   [string]$Tag = '',
   [int]$TimeoutSeconds = 300,
   [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\The Talos Principle'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $Game 'Bin\x64\Talos.exe'
if (-not (Test-Path $exe)) { throw "Talos not found: $exe" }
$out = Join-Path $root 'build\talos'
New-Item -ItemType Directory -Force $out | Out-Null
if (-not $Tag) { $Tag = $Api }
$log = Join-Path $Game 'Log\Talos.log'

Add-Type -Namespace TalosRun -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int size);
public static bool HasDialog(uint pid) {
   bool found = false;
   EnumWindows((h, l) => {
      uint owner; GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h)) return true;
      var name = new System.Text.StringBuilder(64); GetClassName(h, name, 64);
      if (name.ToString() == "#32770") { found = true; return false; }   // dialog box class
      return true;
   }, System.IntPtr.Zero);
   return found;
}
'@

$cmdline = "+gfx_strAPI `"$Api`" +bmk_bSaveFrameTimes 1 +bmk_bAutoQuit 1 $Extra +exec `"Content/Talos/Scripts/RunAutomatedBenchmark.lua`""
# The previous run's log may say "Crashed!"; the game writes a new one.
if (Test-Path $log) { Remove-Item $log -Force }
# Talos saves command-line cvars (gfx_strAPI, bmk_*, experiments) into the
# player's Steam userdata Talos.ini; put the player's settings back afterwards.
$inis = @(Get-ChildItem 'C:\Program Files (x86)\Steam\userdata\*\257510\local\Talos.ini' -ErrorAction SilentlyContinue)
$saved = @{}
foreach ($ini in $inis) { $saved[$ini.FullName] = [IO.File]::ReadAllBytes($ini.FullName) }
$start = [Diagnostics.ProcessStartInfo]::new($exe, $cmdline)
$start.WorkingDirectory = Split-Path $exe
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
if ($Api -eq 'Vulkan' -and $Icd -ne 'installed') {
   if (-not $Icd) { $Icd = Join-Path $root 'build\icd\intel_hasvk_wddm_icd.json' }
   $start.EnvironmentVariables['VK_DRIVER_FILES'] = (Resolve-Path $Icd).Path
}
# "powershell -File" passes "A=1,B=2" as one string: split on commas too.
foreach ($pair in ($Env | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
   $name, $value = $pair -split '=', 2
   $start.EnvironmentVariables[$name] = $value
   "  env $name=$value"
}

$watch = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($start)
$stdout = $p.StandardOutput.ReadToEndAsync()
$stderr = $p.StandardError.ReadToEndAsync()
$verdict = ''
while (-not $p.WaitForExit(1000)) {
   if ($watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { $verdict = "timeout after $TimeoutSeconds s"; break }
   $crashed = $false
   try { $crashed = [bool](Select-String -Path $log -Pattern 'Crashed!' -SimpleMatch -Quiet) } catch {}
   if ($crashed) { $verdict = 'crashed (Talos.log)'; break }
   if ([TalosRun.Win]::HasDialog([uint32]$p.Id)) { Start-Sleep 2; $verdict = 'modal dialog'; break }
}
if ($verdict) { try { $p.Kill() } catch {}; $p.WaitForExit() }
foreach ($path in $saved.Keys) { [IO.File]::WriteAllBytes($path, $saved[$path]) }
$text = $stdout.Result + "`n" + $stderr.Result
[IO.File]::WriteAllText((Join-Path $out "$Tag.stdout.txt"), $text)
if (Test-Path $log) { Copy-Item $log (Join-Path $out "$Tag.Talos.log") -Force }
$fps = @(Get-Content $log -ErrorAction SilentlyContinue | Select-String 'Average:|Low 1%' | ForEach-Object { $_.Line -replace '^\S+\s+INF:\s*', '' })
"$Api [$Tag]: exit=$($p.ExitCode) wall=$([math]::Round($watch.Elapsed.TotalSeconds))s $verdict"
if ($verdict -like 'crashed*' -or $verdict -eq 'modal dialog') {
   Get-Content $log | Select-String 'RIP:|Crashed!|ERR:' | Select-Object -First 5 | ForEach-Object { '  ' + $_.Line.Trim() }
}
$fps | Select-Object -Last 2 | ForEach-Object { "  $_" }
if ($verdict -or $fps.Count -eq 0) { exit 1 }
