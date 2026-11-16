param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
if ($SkipBuild) { & (Join-Path $PSScriptRoot 'build-native.ps1') -ProbeVa -RunOnly }
else { & (Join-Path $PSScriptRoot 'build-native.ps1') -ProbeVa }
$taskBuild = Join-Path $taskRoot 'build\native'
$taskProbe = Join-Path $taskBuild 'HasvkWddmProbe.exe'
$taskExecutableHash=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant()
$taskCapture = Join-Path $taskRoot 'gpu-probe\artifacts\compute-bootstrap\compute-capture'
$taskBootstrap = Join-Path $taskRoot 'gpu-probe\artifacts\native-bootstrap\bootstrap'
$taskPositive = @(Get-ChildItem -LiteralPath $taskBuild -Directory | ForEach-Object {
    $taskReport = Join-Path $_.FullName 'report.json'
    if ((Test-Path -LiteralPath $taskReport) -and $_.Name -match '^(profile|store|compute|generated-store|generated-compute|va|va-pool|va-layout|batch-chain)-\d+$') {
        $taskResult = Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
        if (-not $taskResult.overallPassed -or -not $taskResult.cleanupValidated -or $taskResult.forbiddenModules.Count -or $taskResult.executableSha256 -ne $taskExecutableHash) {
            throw "Invalid positive result: $taskReport"
        }
        [ordered]@{ case=$_.Name; passed=$true; reportSha256=(Get-FileHash -LiteralPath $taskReport -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
})
$taskTampered = Join-Path $taskBuild 'negative-bootstrap'
New-Item -ItemType Directory -Path $taskTampered -Force | Out-Null
foreach ($name in @('buffer-4096.bin','buffer-8192.bin','submit-header.bin')) {
    Copy-Item -LiteralPath (Join-Path $taskBootstrap $name) -Destination (Join-Path $taskTampered $name)
}
$taskHeaderPath = Join-Path $taskTampered 'submit-header.bin'
$taskHeader = [IO.File]::ReadAllBytes($taskHeaderPath)
$taskHeader[0] = $taskHeader[0] -bxor 1
[IO.File]::WriteAllBytes($taskHeaderPath, $taskHeader)
$taskNegative = @()
$taskCases = @(
    @('compute',0,$taskBootstrap,'Unsupported compute count/buffer shape'),
    @('compute',16382,$taskBootstrap,'Unsupported compute count/buffer shape'),
    @('generated-store',0,$taskBootstrap,'Invalid store test size'),
    @('generated-store',4097,$taskBootstrap,'Invalid store test size'),
    @('store',0,$taskTampered,'Fixture SHA256 mismatch:*')
)
for ($taskIndex=0; $taskIndex -lt $taskCases.Count; $taskIndex++) {
    $case=$taskCases[$taskIndex]
    $taskOutput=Join-Path $taskBuild ('negative-'+$taskIndex)
    $taskPreviousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $taskProbe $case[0] $case[1] $taskCapture $case[2] $taskOutput 2>&1 | Out-File (Join-Path $taskBuild ('negative-'+$taskIndex+'.log'))
        $taskExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$taskPreviousPreference }
    $taskResult=Get-Content -LiteralPath (Join-Path $taskOutput 'report.json') -Raw | ConvertFrom-Json
    if ($taskExit -ne 1 -or $taskResult.overallPassed -or $taskResult.error -notlike $case[3]) {
        throw "Unexpected negative-test result: $taskOutput"
    }
    $taskNegative += [ordered]@{case=$taskIndex; rejected=$true; reason=$taskResult.error}
}
# A fresh process must still submit correctly after all rejection paths.
& $taskProbe generated-store 65536 $taskCapture $taskBootstrap (Join-Path $taskBuild 'post-negative-health') | Out-Null
if ($LASTEXITCODE) { throw 'GPU health check failed after negative cases' }
$taskArtifacts=Join-Path $taskRoot 'native\artifacts'
New-Item -ItemType Directory -Path $taskArtifacts -Force | Out-Null
$taskValidation=[ordered]@{
    schema=1
    testedUtc=[DateTime]::UtcNow.ToString('o')
    hardware='Intel Iris 6100 8086:162b'
    driverVersion='20.19.15.5171'
    executableSha256=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant()
    positiveCases=$taskPositive
    negativeCases=$taskNegative
    postNegativeHealthPassed=$true
    nativeCppTransportValidated=$true
    generatedLinearAllocationMetadataValidated=$true
    generated8192CommandHeaderValidated=$true
    fourGiBVaWindowWithThreeExactMappingsValidated=$true
    relocatedHasvkAddressLayoutValidated=$true
    batchChainingAndAllocationReuseValidated=$true
    independentSpirvCompilationValidated=$false
    vulkanIcdImplemented=$false
    hardwareVkcubeValidated=$false
}
$taskValidation | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskArtifacts 'validation.json') -Encoding UTF8
Write-Output ('PASS: '+$taskPositive.Count+' positive cases, '+$taskNegative.Count+' expected rejections, post-negative GPU health check')
