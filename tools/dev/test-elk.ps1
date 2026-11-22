param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SkipBuild) {
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-mesa-compiler.ps1')
    & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-native.ps1') -BuildOnly
}
$taskCompiler=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\IrisElkCompile.exe'
$taskProbe=Join-Path $taskRoot 'build\native\HasvkWddmProbe.exe'
$taskMesaBuild=Join-Path $taskRoot 'build\mesa-compiler-clang'
$taskPreviousPath=$env:PATH
$taskResults=@()
try {
    $env:PATH=(Join-Path $taskMesaBuild 'subprojects\zlib-1.3.1')+';'+$env:PATH
    foreach ($taskValue in @('0','0x51c0ffee','0xf1e2d3c4')) {
        $taskOutput=Join-Path $taskRoot ('build\elk\'+$taskValue)
        New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
        $taskKernel=Join-Path $taskOutput 'kernel.bin'
        $taskMetadata=Join-Path $taskOutput 'kernel.json'
        & $taskCompiler $taskKernel $taskMetadata $taskValue
        if ($LASTEXITCODE) { throw "ELK compilation failed: $taskValue" }
        $taskReport=Get-Content -LiteralPath $taskMetadata -Raw | ConvertFrom-Json
        $taskReport | Add-Member -NotePropertyName kernelSha256 -NotePropertyValue (Get-FileHash -LiteralPath $taskKernel -Algorithm SHA256).Hash.ToLowerInvariant() -Force
        $taskReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $taskMetadata -Encoding UTF8
        & $taskProbe elk 0 $taskOutput unused $taskOutput
        if ($LASTEXITCODE) { throw "ELK hardware execution failed: $taskOutput\report.json" }
        $taskResultPath=Join-Path $taskOutput 'report.json'
        $taskResult=Get-Content -LiteralPath $taskResultPath -Raw | ConvertFrom-Json
        if (-not $taskResult.overallPassed -or -not $taskResult.cleanupValidated -or $taskResult.forbiddenModules.Count) {
            throw "Invalid ELK hardware proof: $taskResultPath"
        }
        $taskResults += [ordered]@{value=$taskValue; passed=$true; kernelSha256=$taskReport.kernelSha256;
            reportSha256=(Get-FileHash -LiteralPath $taskResultPath -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
} finally { $env:PATH=$taskPreviousPath }
$taskValidation=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    compilerSha256=(Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant();
    probeSha256=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant(); cases=$taskResults;
    nirElkGenxmlToHardwareValidated=$true; capturedKernelStateOrAllocationMetadataUsed=$false; vulkanIcdImplemented=$false}
$taskValidation | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\elk-validation.json') -Encoding UTF8
Write-Output 'PASS: three freshly compiled NIR kernels executed on hardware'
