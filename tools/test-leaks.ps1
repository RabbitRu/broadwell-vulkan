# Resource leaks over many object lifetimes (native\tests\leak_probe.cpp):
# 150 device lifetimes with memory, buffer, image and a submission, then 60
# swapchain lifetimes on one window (it closes itself). Private memory, OS
# handles and GDI objects must stay flat after warm-up.
# Report: native\artifacts\leak-validation.json.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build-native.ps1') -BuildOnly; & (Join-Path $PSScriptRoot 'build-icd.ps1') -SkipBuild | Out-Null }
$taskPackage = Join-Path $taskRoot 'build\icd'
$taskReport = Join-Path $taskRoot 'native\artifacts\leak-validation.json'
& (Join-Path $PSScriptRoot 'run-icd.ps1') -Executable (Join-Path $taskPackage 'IrisLeakProbe.exe') -Arguments @($taskReport)
$taskResult = Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
if (-not $taskResult.passed) { throw "Leak check failed: $($taskResult.error)" }
Write-Output "PASS: no leaks over $($taskResult.devices.cycles) device and $($taskResult.swapchains.cycles) swapchain lifetimes"
