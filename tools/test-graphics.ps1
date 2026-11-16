param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-mesa-compiler.ps1') -Graphics
    & (Join-Path $PSScriptRoot 'build-native.ps1') -BuildOnly
}
& (Join-Path $PSScriptRoot 'build-icd.ps1') -SkipBuild
$taskPackage=Join-Path $taskRoot 'build\icd'
$taskReportPath=Join-Path $taskPackage 'diagnostics\graphics-report.json'
& (Join-Path $PSScriptRoot 'run-icd.ps1') -Executable (Join-Path $taskPackage 'IrisGraphicsProbe.exe') -Arguments @($taskReportPath)
$taskReport=Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
if (-not $taskReport.passed -or -not $taskReport.forbiddenModulesAbsent -or $taskReport.cases.Count -ne 6 -or
    $taskReport.texturedDepthCases.Count -ne 12 -or $taskReport.queryCases.Count -ne 6 -or
    $taskReport.drawCases.Count -ne 11 -or $taskReport.computeCases.Count -ne 3 -or @($taskReport.drawCases | Where-Object {-not $_.guardsIntact -or $_.quads -ne 7}).Count -or
    $taskReport.transfer.imageCases.Count -ne 2 -or -not $taskReport.transfer.bufferCommands.copyRegions -or
    $taskReport.formatCoverage.formats.Count -lt 42 -or $taskReport.formatCoverage.shapes.Count -ne 4 -or
    $taskReport.missingCoreCommands.Count -gt 1 -or
    @($taskReport.queryCases | Where-Object {-not $_.guardsIntact -or -not $_.gpuCopy64And32Verified -or -not $_.resetReuseVerified -or
        $_.samplesPassed -lt 400 -or $_.twoDraws -ne 2*$_.samplesPassed -or $_.statistics.iaVertices -ne 3}).Count -or
    @(@($taskReport.cases)+@($taskReport.texturedDepthCases) | Where-Object {-not $_.guardsIntact -or $_.comparedPixels -lt 4000}).Count) {
    throw 'Hardware Vulkan graphics verification failed'
}
$taskResult=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    icdSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
    clientSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'IrisGraphicsProbe.exe') -Algorithm SHA256).Hash.ToLowerInvariant();
    reportSha256=(Get-FileHash -LiteralPath $taskReportPath -Algorithm SHA256).Hash.ToLowerInvariant();
    triangleCases=6; textureUniformSamplerDepthCases=12; offscreenVulkanGraphicsValidated=$true;
    queryCases=6; occlusionPipelineStatisticsTimestampQueriesValidated=$true;
    drawCases=11; computeCases=3; compute3dGroupsSharedMemoryIndirectDispatchValidated=$true; blendMsaaResolveStencilDynamicScissorValidated=$true; chainedCommandBuffersAndSecondariesValidated=$true; vertexInputIndexedInstancedIndirectDrawsValidated=$true;
    transferImageCases=2; bufferAndImageTransferCommandsValidated=$true;
    formatRoundTrips=$taskReport.formatCoverage.formats.Count; imageShapeRoundTrips=$taskReport.formatCoverage.shapes.Count;
    unsupportedOptionalFormats=@($taskReport.formatCoverage.unsupportedOptionalFormats);
    coreCommands=$taskReport.coreCommands; missingCoreCommands=@($taskReport.missingCoreCommands);
    linearAndYTiledTargetsValidated=$true; independentColorAndD16PixelChecks=$true; guardsIntact=$true;
    intelUmdAndD3d12Absent=$true; completeVulkanImplementation=$false; hardwareVkcubeValidated=$false}
$taskResult | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\graphics-validation.json') -Encoding UTF8
Write-Output 'PASS: hardware Vulkan triangle, texture, dynamic UBO, sampler, D16 and queries with independent pixel/guard checks'
