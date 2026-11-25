# Experiment behind the driver's presents by exclusive flip: frames on the
# screen without the compositor and without D3D, by this project's own device
# (native\tests\flip_exclusive_probe.cpp): primaries, exclusive ownership of
# the output, a mode set and D3DKMTPresent flips, as DXGI's full-screen state
# does it (tools\dev\trace-d3d11-present.ps1 -Fullscreen). Three primaries
# (red, green, blue) are flipped in turn; the GPU load of dwm.exe meanwhile is
# reported (none). The screen is taken for the run and given back.
param([int]$Frames = 600, [ValidateRange(0, 4)][int]$Interval = 1)
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'import-toolchain.ps1')
$taskOut = Join-Path $taskRoot 'build\flip-exclusive'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
Push-Location $taskOut
try {
    $taskSources = @('native\src\hasvk_wddm.cpp', 'native\src\intel_2019_profile.cpp', 'native\tests\flip_exclusive_probe.cpp') | ForEach-Object { Join-Path $taskRoot $_ }
    $ErrorActionPreference = 'Continue'
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /utf-8 /W3 /D_WIN32_WINNT=0x0A00 /I (Join-Path $taskRoot 'native\include') @taskSources `
        /Fe:FlipExclusiveProbe.exe /link gdi32.lib user32.lib dxgi.lib | Out-Null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw 'Flip exclusive probe build failed' }
} finally { Pop-Location }
& (Join-Path $PSScriptRoot 'test-flip-probe.ps1') -Exe (Join-Path $taskOut 'FlipExclusiveProbe.exe') -Arguments "$Frames", "$Interval" -Delay 3 -Seconds 4
