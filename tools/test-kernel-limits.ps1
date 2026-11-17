# Risk check: which MI commands and registers HasVK needs work in our WDDM
# context (native\tests\kernel_limits_probe.cpp). Unattended; report in
# native\artifacts\kernel-limits.json.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskOut = Join-Path $taskRoot 'build\kernel-limits'
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
Push-Location $taskOut
try {
    $taskSources = @('native\src\hasvk_wddm.cpp', 'native\src\intel_2019_profile.cpp', 'native\tests\kernel_limits_probe.cpp') | ForEach-Object { Join-Path $taskRoot $_ }
    & cl.exe /nologo /std:c++17 /EHsc /MT /O2 /Zi /utf-8 /W4 /D_WIN32_WINNT=0x0A00 /I (Join-Path $taskRoot 'native\include') @taskSources `
        /Fe:KernelLimitsProbe.exe /link gdi32.lib user32.lib dxgi.lib
    if ($LASTEXITCODE) { throw 'Kernel limits probe build failed' }
    $taskReport = Join-Path $taskRoot 'native\artifacts\kernel-limits.json'
    $taskProcess = Start-Process -FilePath (Join-Path $taskOut 'KernelLimitsProbe.exe') -ArgumentList @($taskReport) -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $taskOut 'stdout.log') -RedirectStandardError (Join-Path $taskOut 'stderr.log')
    if (-not $taskProcess.WaitForExit(120000)) { $taskProcess.Kill(); throw 'Kernel limits probe exceeded 120 s' }
    Get-Content (Join-Path $taskOut 'stdout.log'); Get-Content (Join-Path $taskOut 'stderr.log')
    if ($taskProcess.ExitCode) { throw "Kernel limits probe failed: $($taskProcess.ExitCode)" }
} finally { Pop-Location }
