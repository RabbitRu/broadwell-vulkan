param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
& (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-icd.ps1') -SkipBuild:$SkipBuild
& (Join-Path $PSScriptRoot 'run-icd.ps1')
$taskPackage=Join-Path $taskRoot 'build\icd'
$taskReportPath=Join-Path $taskPackage 'diagnostics\report.json'
$taskReport=Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
if (-not $taskReport.passed -or $taskReport.forbiddenModules.Count -or $taskReport.gpuConfirmedSubmissions -lt 114 -or
    -not $taskReport.fenceStatusResetAndTimeoutValidated -or -not $taskReport.binarySemaphoreReuseValidated -or
    -not $taskReport.fenceWaitAnyAndAllValidated -or -not $taskReport.bothEmptySubmissionFormsValidated -or
    $taskReport.wddmVulkanMemoryCases -ne 72 -or -not $taskReport.mappedMemoryBindingAndLifetimeValidated -or
    $taskReport.mesaStatePoolGpuReadbacks -ne 270 -or -not $taskReport.mesaStatePoolAndStateTableGrowthValidated -or
    $taskReport.bufferDescriptorChecks -ne 54 -or -not $taskReport.descriptorAndPipelineLayoutLifecycleValidated -or
    $taskReport.shaderCommandBuffersDisabled -or -not $taskReport.vulkanComputeValidated -or
    -not $taskReport.computePipelineCreationAndIsaValidated -or -not $taskReport.pipelineCacheRoundtripValidated) { throw 'ICD compute validation failed' }
$taskComputeCases=@($taskReport.computeHardwareCases | ForEach-Object { $_ | ForEach-Object { $_ } })
$taskPipelineCases=@($taskReport.computePipelineCases | ForEach-Object { $_ | ForEach-Object { $_ } })
if ($taskPipelineCases.Count -ne 72 -or @($taskPipelineCases | Where-Object {-not $_.isaValidated}).Count) {
    throw 'ICD pipeline creation/ISA validation failed'
}
if ($taskComputeCases.Count -ne 342 -or @($taskComputeCases | Where-Object {-not $_.passed -or $_.sameCommandBufferExecutions -ne 2}).Count) {
    throw 'ICD compute case count or repeat/guard verification failed'
}
$taskImageCases=@($taskReport.storageImageHardwareCases | ForEach-Object { $_ | ForEach-Object { $_ } })
if (-not $taskReport.storageImagesWithIndependentCpuDetileValidated -or $taskImageCases.Count -ne 18 -or
    @($taskImageCases | Where-Object {-not $_.passed -or -not $_.cpuDetileAndShaderReadAgreed -or -not $_.auxDisabled}).Count) {
    throw 'ICD storage-image validation failed'
}
$taskResult=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    loader='Windows System32 vulkan-1.dll';
    icdSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
    clientSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'IrisIcdProbe.exe') -Algorithm SHA256).Hash.ToLowerInvariant();
    manifestSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'intel_hasvk_wddm_icd.json') -Algorithm SHA256).Hash.ToLowerInvariant();
    reportSha256=(Get-FileHash -LiteralPath $taskReportPath -Algorithm SHA256).Hash.ToLowerInvariant();
    gpuConfirmedSubmissions=$taskReport.gpuConfirmedSubmissions; deviceCreationCycles=$taskReport.deviceCreationCycles;
    hardwareAdapterEnumeratedThroughLoader=$true; vulkanDeviceCreationValidated=$true;
    fencesAndBinarySemaphoreReuseValidated=$true; unsupportedFeaturesAndExtensionsRejected=$true;
    fenceWaitAnyAndAllValidated=$true; bothEmptySubmissionFormsValidated=$true;
    wddmVulkanMemoryCases=$taskReport.wddmVulkanMemoryCases; mappedMemoryBindingAndLifetimeValidated=$true;
    applicationBufferGpuShaderAccessValidated=$true;
    mesaStatePoolGpuReadbacks=$taskReport.mesaStatePoolGpuReadbacks; mesaStatePoolAndStateTableGrowthValidated=$true;
    bufferDescriptorChecks=$taskReport.bufferDescriptorChecks; descriptorAndPipelineLayoutLifecycleValidated=$true;
    descriptorGpuShaderAccessValidated=$true;
    computeHardwareCases=$taskComputeCases.Count; repeatedComputeCommandBuffers=2;
    computePipelineCreationAndIsaValidated=$true; pipelineCacheRoundtripValidated=$true;
    computePipelineCreations=$taskPipelineCases.Count;
    storageImageHardwareCases=$taskImageCases.Count; storageImagesWithIndependentCpuDetileValidated=$true;
    systemRegistrationChanged=$false; vulkanDriverEnvironmentScopedToChild=$true;
    completeVulkanImplementation=$false; vulkanComputeValidated=$true; hardwareVkcubeValidated=$false}
$taskResult | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\icd-validation.json') -Encoding UTF8
Write-Output 'PASS: real Vulkan loader, HasVK SPIR-V compute, WDDM GPU execution, readback, guards and synchronization'
