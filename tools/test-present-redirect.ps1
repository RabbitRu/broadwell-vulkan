# Experiment: windowed present through a shared surface created by our own
# transport (no D3D), see native\tests\present_redirect_probe.cpp. Needs the
# surface descriptor captured by tools\trace-gl-present.ps1 (build\gl-trace).
# Options: glcontext (context private data as the OpenGL ICD; required for the
# present to be shown), hint=N, glheader, pixelformat (experiment variants).
param([ValidateSet('own', 'dwmapi')][string]$Surface = 'own', [string[]]$Options = @('glcontext'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskOut = Join-Path $taskRoot 'build\present-redirect'
$taskTrace = Join-Path $taskRoot 'build\gl-trace'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
Push-Location $taskOut
try {
    $taskSources = @('native\src\hasvk_wddm.cpp', 'native\src\intel_2019_profile.cpp', 'native\tests\present_redirect_probe.cpp') | ForEach-Object { Join-Path $taskRoot $_ }
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /Zi /utf-8 /W4 /D_WIN32_WINNT=0x0A00 /I (Join-Path $taskRoot 'native\include') @taskSources `
        /Fe:PresentRedirectProbe.exe /link gdi32.lib user32.lib dxgi.lib dwmapi.lib opengl32.lib
    if ($LASTEXITCODE) { throw 'Present redirect probe build failed' }
    $taskProcess = Start-Process -FilePath (Join-Path $taskOut 'PresentRedirectProbe.exe') -NoNewWindow -PassThru `
        -ArgumentList @((Join-Path $taskTrace 'surface-descriptor.bin'), (Join-Path $taskTrace 'surface-runtime.bin'), $Surface + $Options) `
        -RedirectStandardOutput (Join-Path $taskOut 'stdout.log') -RedirectStandardError (Join-Path $taskOut 'stderr.log')
    if (-not $taskProcess.WaitForExit(60000)) { $taskProcess.Kill(); throw 'Present redirect probe exceeded 60 s' }
    Get-Content (Join-Path $taskOut 'stdout.log'); Get-Content (Join-Path $taskOut 'stderr.log')
    if ($taskProcess.ExitCode) { throw "Present redirect probe failed: $($taskProcess.ExitCode)" }
} finally { Pop-Location }
