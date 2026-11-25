<#
Presents by exclusive flip (mesa\anv_wddm_present.c) on the real screen:
native\tests\flip_wsi_probe.cpp covers the primary screen with a Vulkan window
for a few seconds per case, and native\tests\screen_capture.cpp reads what the
display shows (Desktop Duplication; GDI's screen DC does not see an output
that is owned exclusively).

  IMMEDIATE, FIFO, IMMEDIATE with an sRGB swapchain: the output is taken, the
     pixels on the screen are the frame's (background, a white rectangle with
     exact edges), FIFO runs at the refresh rate
  a topmost window shown for two seconds: composed meanwhile, flips again after
  MAILBOX and a window smaller than the screen: never flipped

Needs the probes (tools\dev\build-probes.ps1) and the driver in build\icd.
The screen is taken over for about a minute in all; nothing has to be closed.
#>
param([string]$Icd = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Icd) { $Icd = Join-Path $root 'build\icd\intel_hasvk_wddm_icd.json' }
$probe = Join-Path $root 'build\probes\flip_wsi_probe.exe'; $capture = Join-Path $root 'build\probes\screen_capture.exe'
foreach ($f in $probe, $capture, $Icd) { if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f (tools\dev\build-probes.ps1, tools\build-icd.ps1)" } }
$out = Join-Path $root 'build\flip-test'; New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System.Runtime.InteropServices;
public static class FlipTestScreen { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
'@
[void][FlipTestScreen]::SetProcessDPIAware()
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$failures = @()
function Start-Probe([string[]]$arguments) {
   $start = [Diagnostics.ProcessStartInfo]::new($probe, ($arguments -join ' '))
   $start.UseShellExecute = $false; $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
   $start.EnvironmentVariables['VK_DRIVER_FILES'] = (Resolve-Path $Icd).Path
   $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
   $start.EnvironmentVariables['HASVK_WDDM_PRESENT_FLIP_DEBUG'] = '1'
   $process = [Diagnostics.Process]::Start($start)
   [pscustomobject]@{ process = $process; stdout = $process.StandardOutput.ReadToEndAsync(); stderr = $process.StandardError.ReadToEndAsync() }
}
function Complete-Probe($run) {
   if (-not $run.process.WaitForExit(60000)) { $run.process.Kill(); throw 'The probe did not end within 60 s' }
   [pscustomobject]@{ exit = $run.process.ExitCode; text = $run.stdout.Result; log = $run.stderr.Result
      taken = ([regex]::Matches($run.stderr.Result, 'the output is ours')).Count
      fps = $(if ($run.stdout.Result -match '= ([\d.]+) FPS') { [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) } else { 0 }) }
}
# Flipped cases: a held colour (0.25, 0.5, 0.75) with a white rectangle at (100,50)-(400,250).
foreach ($case in @{ name = 'IMMEDIATE'; args = @('3', 'immediate', '5'); colour = '4080bf' },
                  @{ name = 'FIFO'; args = @('3', 'fifo', '5'); colour = '4080bf'; fifo = $true },
                  @{ name = 'IMMEDIATE sRGB'; args = @('3', 'immediate', '5', 'srgb'); colour = '89bbe1' }) {
   $run = Start-Probe $case.args
   Start-Sleep -Seconds 6
   $far = "$($screen.Width - 10),$($screen.Height - 10)"
   $picture = Join-Path $out (($case.name -replace '\W', '-') + '.bmp')
   $pixels = & $capture $picture 2500 '10,10' $far '99,49' '400,250' '100,50' '399,249' 2>&1
   $result = Complete-Probe $run
   $seen = @($pixels | Select-String 'pixel \(\d+,\d+\) = ([0-9a-f]{6})' | ForEach-Object { $_.Matches[0].Groups[1].Value })
   $expected = @($case.colour, $case.colour, $case.colour, $case.colour, 'ffffff', 'ffffff')
   $ok = $result.exit -eq 0 -and $result.taken -eq 1 -and $seen.Count -eq 6 -and -not (Compare-Object $seen $expected -SyncWindow 0)
   if ($case.fifo -and ($result.fps -lt 50 -or $result.fps -gt 70)) { $ok = $false }
   '{0}: {1}  ({2:N1} FPS, screen {3})' -f $case.name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $result.fps, ($seen -join ' ')
   if (-not $ok) { $failures += $case.name; $result.text.Trim(); ($result.log -split "`n" | Select-String 'flip|error') }
}
# A window above for two seconds (second 3 to 5, the held colour by then):
# the output is given back, the frames are copied for DWM meanwhile (the
# screen still shows them exactly), and the output is taken again.
$run = Start-Probe @('1', 'immediate', '7', 'cover=3')
Start-Sleep -Seconds 4
$pixels = & $capture (Join-Path $out 'cover.bmp') 2500 '10,10' '99,49' '100,50' '399,249' 2>&1
$result = Complete-Probe $run
$seen = @($pixels | Select-String 'pixel \(\d+,\d+\) = ([0-9a-f]{6})' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$ok = $result.exit -eq 0 -and $result.taken -eq 2 -and $seen.Count -eq 4 -and -not (Compare-Object $seen @('4080bf', '4080bf', 'ffffff', 'ffffff') -SyncWindow 0)
'a window above: {0}  (the output taken {1} times, {2:N1} FPS, screen while composed {3})' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $result.taken, $result.fps, ($seen -join ' ')
if (-not $ok) { $failures += 'cover'; ($result.log -split "`n" | Select-String 'flip|error') }
# Never flipped.
foreach ($case in @{ name = 'MAILBOX'; args = @('3', 'mailbox') }, @{ name = 'window of 1280x800'; args = @('3', 'immediate', '0', 'size=1280x800') }) {
   $result = Complete-Probe (Start-Probe $case.args)
   $ok = $result.exit -eq 0 -and $result.taken -eq 0 -and $result.fps -gt 0
   '{0}: {1}  (composed, {2:N1} FPS)' -f $case.name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $result.fps
   if (-not $ok) { $failures += $case.name }
}
if ($failures) { throw "Presents by exclusive flip failed: $($failures -join ', ')" }
'PASS: exclusive flips shown pixel-exact (IMMEDIATE, FIFO, sRGB), given back to the compositor and taken again, not used for MAILBOX or a smaller window'
