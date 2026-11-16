param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-mesa-compiler.ps1')
    & (Join-Path $PSScriptRoot 'build-native.ps1') -BuildOnly
}
$taskPython=Join-Path $taskRoot '.tools\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'gen-spirv-probes.py')
if ($LASTEXITCODE) { throw 'SPIR-V test generation failed' }
$taskCompiler=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\IrisElkCompile.exe'
$taskProbe=Join-Path $taskRoot 'build\native\HasvkWddmProbe.exe'
$taskDirectory=Join-Path $taskRoot 'build\spirv'
$taskCases=Get-Content -LiteralPath (Join-Path $taskDirectory 'cases.json') -Raw | ConvertFrom-Json
$taskPreviousPath=$env:PATH
$taskResults=@()
try {
    $env:PATH=(Join-Path $taskRoot 'build\mesa-compiler-clang\subprojects\zlib-1.3.1')+';'+$env:PATH
    foreach ($case in $taskCases.cases) {
        $taskOutput=Join-Path $taskDirectory $case.name
        New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
        $taskKernel=Join-Path $taskOutput 'kernel.bin'
        $taskMetadata=Join-Path $taskOutput 'kernel.json'
        & $taskCompiler --spirv (Join-Path $taskDirectory $case.module) $taskKernel $taskMetadata $case.expectedValue
        if ($LASTEXITCODE) { throw "SPIR-V compilation failed: $($case.name)" }
        $taskReport=Get-Content -LiteralPath $taskMetadata -Raw | ConvertFrom-Json
        $taskReport | Add-Member -NotePropertyName kernelSha256 -NotePropertyValue (Get-FileHash -LiteralPath $taskKernel -Algorithm SHA256).Hash.ToLowerInvariant() -Force
        if ($case.elementCount) {
            $taskReport | Add-Member -NotePropertyName elementCount -NotePropertyValue $case.elementCount -Force
        } else {
            $taskReport | Add-Member -NotePropertyName initialValue -NotePropertyValue $case.initialValue -Force
        }
        $taskReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $taskMetadata -Encoding UTF8
        & $taskProbe elk 0 $taskOutput unused $taskOutput
        if ($LASTEXITCODE) { throw "SPIR-V hardware execution failed: $taskOutput\report.json" }
        $taskGpuResult=Get-Content -LiteralPath (Join-Path $taskOutput 'report.json') -Raw | ConvertFrom-Json
        $taskResults += [ordered]@{name=$case.name; initialValue=$case.initialValue; expectedValue=$case.expectedValue;
            actualValue=$taskGpuResult.elkCompute.actualValue; elementCount=$taskGpuResult.elkCompute.elementCount;
            localSize=$taskGpuResult.elkCompute.localSize; groupCountX=$taskGpuResult.elkCompute.groupCountX;
            simdWidth=$taskGpuResult.elkCompute.simdWidth; guardWordsVerified=$taskGpuResult.elkCompute.guardWordsVerified;
            moduleSha256=$case.moduleSha256; kernelSha256=$taskReport.kernelSha256; passed=$true}
    }
    foreach ($name in @('unsupported-binding','invalid')) {
        $taskPreference=$ErrorActionPreference
        try {
            $ErrorActionPreference='Continue'
            & $taskCompiler --spirv (Join-Path $taskDirectory ($name+'.spv')) (Join-Path $taskDirectory ($name+'.bin')) (Join-Path $taskDirectory ($name+'.json')) 0 2>&1 | Out-File (Join-Path $taskDirectory ($name+'.log'))
            $taskExit=$LASTEXITCODE
        } finally { $ErrorActionPreference=$taskPreference }
        if ($taskExit -ne 1) { throw "Invalid SPIR-V was not rejected cleanly: $name, exit $taskExit" }
    }
} finally { $env:PATH=$taskPreviousPath }
$taskValidation=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    compilerSha256=(Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant();
    probeSha256=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant(); cases=$taskResults;
    spirvToNirToElkToHardwareValidated=$true; singleSsboOnly=$true; fixed1DWorkgroupsOnly=$true; negativeCasesPassed=2;
    capturedKernelStateOrAllocationMetadataUsed=$false; vulkanIcdImplemented=$false; hardwareVkcubeValidated=$false}
$taskValidation | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\spirv-validation.json') -Encoding UTF8
Write-Output ('PASS: '+$taskResults.Count+' SPIR-V hardware cases, two expected compiler rejections')
