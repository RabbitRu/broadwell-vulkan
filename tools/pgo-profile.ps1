<#
Profile for the PGO build of the ICD (build-mesa-compiler.ps1 -Pgo).

  .\tools\build-mesa-compiler.ps1 -Pgo generate; .\tools\build-icd.ps1 -SkipBuild
  .\tools\pgo-profile.ps1 [-Output tools\pgo\hasvk.profdata] [-NoTalos]
  .\tools\build-icd.ps1        # uses tools\pgo\hasvk.profdata when present

Runs the instrumented build\icd on the workloads whose driver CPU matters,
each process writing a .profraw at exit, then merges them:
  - the draw recording bench (replay_pass_probe, REPLAY_RECORD_ONLY, snapshot
    of a Talos shadow pass in build\talos\pass3): command recording;
  - The Talos Principle's benchmark under Vulkan (tools\run-talos.ps1):
    a whole game frame, pipeline and descriptor creation, submission, present;
  - vkcubepp, 600 frames: present and the plain frame loop.
The profile only steers code layout and inlining; behaviour is unchanged.
#>
param([string]$Output = '', [switch]$NoTalos)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'import-toolchain.ps1') -UseClang
if (-not $Output) { $Output = Join-Path $PSScriptRoot 'pgo\hasvk.profdata' }
$raw = Join-Path $root 'build\pgo\raw'
if (Test-Path -LiteralPath $raw) { Remove-Item -LiteralPath $raw -Recurse -Force }
New-Item -ItemType Directory -Force $raw | Out-Null
# The profile runtime opens the file with the ANSI code page: 8.3 path.
$rawShort = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($raw).ShortPath
$profileFile = Join-Path $rawShort 'hasvk-%p.profraw'
$icd = Join-Path $root 'build\icd\intel_hasvk_wddm_icd.json'
$ErrorActionPreference = 'Continue'

$env:LLVM_PROFILE_FILE = $profileFile
$env:VK_DRIVER_FILES = $icd
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
"draw recording bench"
$env:HASVK_WDDM_INTERNAL_CACHE = '0'; $env:REPLAY_RECORD_ONLY = '20000'
& (Join-Path $root 'build\probes\replay_pass_probe.exe') (Join-Path $root 'build\talos\pass3') 2>&1 | Select-String 'ns per draw' | ForEach-Object { '  ' + $_.Line.Trim() }
Remove-Item Env:HASVK_WDDM_INTERNAL_CACHE, Env:REPLAY_RECORD_ONLY
"vkcubepp"
$cube = Start-Process 'C:\VulkanSDK-local\Bin\vkcubepp.exe' -ArgumentList '--c', '600' -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $raw 'cube.out') -RedirectStandardError (Join-Path $raw 'cube.err')
if (-not $cube.WaitForExit(120000)) { $cube.Kill(); throw 'vkcubepp did not finish' }
if (-not $NoTalos) {
    "Talos benchmark (Vulkan)"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run-talos.ps1') -Api Vulkan -Tag pgo-profile -Env "LLVM_PROFILE_FILE=$profileFile" |
        Select-String 'Average|fail|Crash' | ForEach-Object { '  ' + $_.Line.Trim() }
}
Remove-Item Env:LLVM_PROFILE_FILE, Env:VK_DRIVER_FILES, Env:VK_LOADER_LAYERS_DISABLE
$ErrorActionPreference = 'Stop'

$files = @(Get-ChildItem -LiteralPath $raw -Filter '*.profraw')
if ($files.Count -lt 2) { throw "Only $($files.Count) .profraw files in ${raw}: is build\icd the instrumented build (-Pgo generate)?" }
New-Item -ItemType Directory -Force (Split-Path $Output) | Out-Null
$merged = Join-Path $rawShort 'merged.profdata'
& (Join-Path $taskToolchain.llvmBin 'llvm-profdata.exe') merge -o $merged @($files | ForEach-Object { Join-Path $rawShort $_.Name })
if ($LASTEXITCODE) { throw 'llvm-profdata merge failed' }
Copy-Item -LiteralPath $merged -Destination $Output -Force
"$($files.Count) profiles merged into $Output ($([math]::Round((Get-Item $Output).Length / 1MB, 1)) MiB)"
