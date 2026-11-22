# Window-system integration on the real window path (native\tests\wsi_probe.cpp):
# surface formats checked pixel by pixel, FIFO/IMMEDIATE/MAILBOX pacing,
# SUBOPTIMAL/OUT_OF_DATE on resize and minimize, recreation with oldSwapchain.
# Opens a small window for a few seconds and closes it itself.
# Report: native\artifacts\wsi-validation.json.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SkipBuild) { & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-native.ps1') -BuildOnly; & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-icd.ps1') -SkipBuild | Out-Null }
$taskPackage = Join-Path $taskRoot 'build\icd'
$taskReport = Join-Path $taskRoot 'native\artifacts\wsi-validation.json'
& (Join-Path $PSScriptRoot 'run-icd.ps1') -Executable (Join-Path $taskPackage 'IrisWsiProbe.exe') -Arguments @($taskReport)
$taskResult = Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
if (-not $taskResult.passed) { throw "WSI verification failed: $($taskResult.error)" }
if ($taskResult.note) { Write-Output "NOTE: $($taskResult.note)" }
Write-Output 'PASS: surface formats pixel-exact, present modes paced as declared, resize and recreation'
