param([switch]$SkipBuild, [int]$CubeSeconds = 15, [string]$LayerPath = 'C:\VulkanSDK-local\Bin')
# Runs the hardware probes and the unmodified vkcube under
# VK_LAYER_KHRONOS_validation. Any "Validation Error" fails the run. A negative
# control (vkcube --force_errors) proves that the layer is active and logging.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SkipBuild) {
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-mesa-compiler.ps1')
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-native.ps1') -BuildOnly
}
& (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-icd.ps1') -SkipBuild | Out-Null
$taskPackage=Join-Path $taskRoot 'build\icd'
$taskDiagnostics=Join-Path $taskPackage 'diagnostics'
function Read-Validation([string]$Path) {
    $lines=@(); if (Test-Path -LiteralPath $Path) { $lines=@(Get-Content -LiteralPath $Path) }
    $ids=@($lines | ForEach-Object { if ($_ -match '^Validation (Error|Warning): \[ ([\w-]+) \]') { [ordered]@{severity=$Matches[1]; id=$Matches[2]} } })
    [ordered]@{errors=@($ids | Where-Object { $_.severity -eq 'Error' }).Count; warnings=@($ids | Where-Object { $_.severity -eq 'Warning' }).Count;
        ids=@($ids | ForEach-Object { $_.id } | Sort-Object -Unique)}
}
$taskRuns=@()
foreach ($client in @(@('IrisIcdProbe.exe','report.json'), @('IrisGraphicsProbe.exe','graphics-report.json'))) {
    $log=Join-Path $taskDiagnostics ('validation-'+[IO.Path]::GetFileNameWithoutExtension($client[0])+'.log')
    & (Join-Path $PSScriptRoot 'run-icd.ps1') -Executable (Join-Path $taskPackage $client[0]) -Arguments @((Join-Path $taskDiagnostics $client[1])) -ValidationLog $log -LayerPath $LayerPath | Out-Null
    $summary=Read-Validation $log
    $taskRuns += [ordered]@{client=$client[0]; errors=$summary.errors; warnings=$summary.warnings; messageIds=$summary.ids;
        clientSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage $client[0]) -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$taskCube=Join-Path $taskRoot 'build\vkcube-validate'
& (Join-Path $PSScriptRoot 'capture-vkcube.ps1') -Frames 1000000 -ExerciseSeconds $CubeSeconds -Validate -LayerPath $LayerPath -OutputDirectory $taskCube | Out-Null
$summary=Read-Validation (Join-Path $taskCube 'validation.log')
$taskRuns += [ordered]@{client='vkcube.exe (resize/minimize)'; errors=$summary.errors; warnings=$summary.warnings; messageIds=$summary.ids;
    clientSha256=(Get-FileHash -LiteralPath (Join-Path $taskRoot 'build\vkcube\vkcube.exe') -Algorithm SHA256).Hash.ToLowerInvariant()}
$taskControl=Join-Path $taskRoot 'build\vkcube-validate-control'
try { & (Join-Path $PSScriptRoot 'capture-vkcube.ps1') -Frames 30 -CaptureAfterMs @() -Validate -LayerPath $LayerPath -ExtraArguments '--force_errors' -OutputDirectory $taskControl | Out-Null } catch { }
$taskControlSummary=Read-Validation (Join-Path $taskControl 'validation.log')
if (-not $taskControlSummary.errors) { throw 'Negative control produced no validation errors: the layer is not active' }
$taskLayer=Get-Content -LiteralPath (Join-Path $LayerPath 'VkLayer_khronos_validation.json') -Raw | ConvertFrom-Json
$taskResult=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    layer='VK_LAYER_KHRONOS_validation'; layerApiVersion=$taskLayer.layer.api_version; layerImplementation=$taskLayer.layer.implementation_version;
    icdSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
    runs=$taskRuns; negativeControlErrors=$taskControlSummary.errors; uniqueHandlesDisabledForProbes=$true;
    validationErrors=@($taskRuns | ForEach-Object { $_.errors } | Measure-Object -Sum).Sum}
$taskResult | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\validation-layers.json') -Encoding UTF8
$taskResult | ConvertTo-Json -Depth 5
if ($taskResult.validationErrors) { throw "Validation layer reported $($taskResult.validationErrors) errors" }
Write-Output 'PASS: probes and vkcube are clean under VK_LAYER_KHRONOS_validation'
