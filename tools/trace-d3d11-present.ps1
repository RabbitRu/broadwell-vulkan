param([int]$Frames = 4, [ValidateSet('discard', 'sequential', 'flip_discard', 'flip_sequential')][string]$SwapEffect = 'discard',
      [int]$SyncInterval = 1, [switch]$NoHook)
# -NoHook: no syscall log, 200 presents back to back and their mean wall and
# thread CPU time (calls-<effect>-<interval>-nohook.log).
# Research (hypothesis E7): trace the D3DKMT calls Intel's D3D11 driver and the
# DXGI runtime make to present a few frames in a window
# (native\tests\d3d11_present_trace.cpp). The window closes itself; the log goes
# to build\d3d11-trace\calls-<swap effect>.log.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskOut = Join-Path $taskRoot 'build\d3d11-trace'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
Push-Location $taskOut
try {
    $ErrorActionPreference = 'Continue'
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /utf-8 /W3 /D_WIN32_WINNT=0x0A00 (Join-Path $taskRoot 'native\tests\d3d11_present_trace.cpp') `
        /Fe:D3D11PresentTrace.exe /link d3d11.lib dxgi.lib opengl32.lib user32.lib gdi32.lib psapi.lib dwmapi.lib | Out-Null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw 'D3D11 present trace build failed' }
    $taskLog = Join-Path $taskOut ("calls-$SwapEffect-$SyncInterval" + $(if ($NoHook) { '-nohook' } else { '' }) + '.log')
    $taskArgs = @($taskLog, $(if ($NoHook) { 200 } else { $Frames }), $SwapEffect, $SyncInterval) + $(if ($NoHook) { @('nohook') } else { @() })
    $taskProcess = Start-Process -FilePath (Join-Path $taskOut 'D3D11PresentTrace.exe') -ArgumentList $taskArgs -PassThru -NoNewWindow
    if (-not $taskProcess.WaitForExit(60000)) { $taskProcess.Kill(); throw 'D3D11 present trace exceeded 60 s' }
    if ($taskProcess.ExitCode) { throw "D3D11 present trace failed: $($taskProcess.ExitCode)" }
    Write-Output "Trace: $taskLog"
} finally { Pop-Location }
