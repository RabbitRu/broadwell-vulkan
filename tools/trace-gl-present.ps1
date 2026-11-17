param([int]$Frames = 4)
# Research: trace the D3DKMT calls the Intel OpenGL ICD makes to present a few
# frames in a window (native\tests\gl_present_trace.cpp). The window closes
# itself; the log goes to build\gl-trace\calls.log.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskOut = Join-Path $taskRoot 'build\gl-trace'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
Push-Location $taskOut
try {
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /Zi /utf-8 /W4 /D_WIN32_WINNT=0x0A00 (Join-Path $taskRoot 'native\tests\gl_present_trace.cpp') `
        /Fe:GlPresentTrace.exe /link opengl32.lib user32.lib gdi32.lib psapi.lib dwmapi.lib
    if ($LASTEXITCODE) { throw 'GL present trace build failed' }
    $taskLog = Join-Path $taskOut 'calls.log'
    $taskProcess = Start-Process -FilePath (Join-Path $taskOut 'GlPresentTrace.exe') -ArgumentList @($taskLog, $Frames) -PassThru -NoNewWindow
    if (-not $taskProcess.WaitForExit(60000)) { $taskProcess.Kill(); throw 'GL present trace exceeded 60 s' }
    if ($taskProcess.ExitCode) { throw "GL present trace failed: $($taskProcess.ExitCode)" }
    Write-Output "Trace: $taskLog"
} finally { Pop-Location }
