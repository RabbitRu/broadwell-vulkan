param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskStarted=[DateTime]::UtcNow.ToString('o')
$taskAttemptPath=Join-Path $taskRoot 'native\artifacts\latest-attempt.json'
$taskAttempt=[ordered]@{schema=1; startedUtc=$taskStarted; status='running'; stage='build'}
$taskAttempt | ConvertTo-Json | Set-Content -LiteralPath $taskAttemptPath -Encoding UTF8
try {
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-mesa-compiler.ps1')
    & (Join-Path $PSScriptRoot 'build-native.ps1') -BuildOnly
}
$taskAttempt.stage='native'
& (Join-Path $PSScriptRoot 'test-native.ps1') -SkipBuild
$taskAttempt.stage='elk'
& (Join-Path $PSScriptRoot 'test-elk.ps1') -SkipBuild
$taskAttempt.stage='spirv'
& (Join-Path $PSScriptRoot 'test-spirv.ps1') -SkipBuild
$taskAttempt.stage='images'
& (Join-Path $PSScriptRoot 'test-images.ps1') -SkipBuild
$taskAttempt.stage='icd'
& (Join-Path $PSScriptRoot 'test-icd.ps1') -SkipBuild
$taskMesaBuild=Get-Content -LiteralPath (Join-Path $taskRoot 'build\mesa-compiler-clang\build.json') -Raw | ConvertFrom-Json
if ($taskMesaBuild.graphicsModulesBuilt) {
    $taskAttempt.stage='graphics'
    & (Join-Path $PSScriptRoot 'test-graphics.ps1') -SkipBuild
}
$taskAttempt.stage='csharp-controls'
& (Join-Path $taskRoot '.tools\python\python.exe') (Join-Path $PSScriptRoot 'render-readback.py')
if ($LASTEXITCODE) { throw 'GPU readback PNG encoding failed' }
& (Join-Path $taskRoot 'gpu-probe\run.ps1') -NativeOnly -OutputDirectory (Join-Path $taskRoot 'build\csharp-control-store')
& (Join-Path $taskRoot 'gpu-probe\run.ps1') -NativeCompute -ElementCount 257 -OutputDirectory (Join-Path $taskRoot 'build\csharp-control-compute')
$taskArtifacts=Join-Path $taskRoot 'native\artifacts'
$taskNative=Get-Content -LiteralPath (Join-Path $taskArtifacts 'validation.json') -Raw | ConvertFrom-Json
$taskSpirv=Get-Content -LiteralPath (Join-Path $taskArtifacts 'spirv-validation.json') -Raw | ConvertFrom-Json
$taskElk=Get-Content -LiteralPath (Join-Path $taskArtifacts 'elk-validation.json') -Raw | ConvertFrom-Json
$taskImages=Get-Content -LiteralPath (Join-Path $taskArtifacts 'images-validation.json') -Raw | ConvertFrom-Json
$taskIcd=Get-Content -LiteralPath (Join-Path $taskArtifacts 'icd-validation.json') -Raw | ConvertFrom-Json
$taskProbe=Join-Path $taskRoot 'build\native\HasvkWddmProbe.exe'
$taskCompiler=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\IrisElkCompile.exe'
$taskImageProbe=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\IrisImageProbe.exe'
$taskProbeHash=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant()
$taskCompilerHash=(Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant()
if ($taskNative.executableSha256 -ne $taskProbeHash -or $taskSpirv.probeSha256 -ne $taskProbeHash -or
    $taskElk.probeSha256 -ne $taskProbeHash -or $taskSpirv.compilerSha256 -ne $taskCompilerHash -or
    $taskElk.compilerSha256 -ne $taskCompilerHash -or
    $taskImages.probeSha256 -ne (Get-FileHash -LiteralPath $taskImageProbe -Algorithm SHA256).Hash.ToLowerInvariant()) {
    throw 'Validation artifacts refer to different executable versions'
}
$taskProofs=@()
$taskProofNames=@('validation.json','elk-validation.json','spirv-validation.json','images-validation.json','icd-validation.json')
if ($taskMesaBuild.graphicsModulesBuilt) { $taskProofNames += 'graphics-validation.json' }
foreach ($name in $taskProofNames) {
    $taskProofs += [ordered]@{file=$name; sha256=(Get-FileHash -LiteralPath (Join-Path $taskArtifacts $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$taskResult=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    mesa='26.2.7'; nativePositiveCases=$taskNative.positiveCases.Count; expectedRejections=$taskNative.negativeCases.Count+$taskSpirv.negativeCasesPassed;
    nirHardwareCases=$taskElk.cases.Count; spirvHardwareCases=$taskSpirv.cases.Count; imageHardwareCases=$taskImages.cases.Count;
    csharpControlsPassed=$true; allImplementedHardwareTestsPassed=$true; proofs=$taskProofs;
    vulkanControlPlaneIcdValidated=$true; gpuConfirmedVulkanSubmissions=$taskIcd.gpuConfirmedSubmissions;
    wddmVulkanMemoryCases=$taskIcd.wddmVulkanMemoryCases;
    mesaStatePoolGpuReadbacks=$taskIcd.mesaStatePoolGpuReadbacks;
    bufferDescriptorChecks=$taskIcd.bufferDescriptorChecks;
    vulkanComputeValidated=$taskIcd.vulkanComputeValidated; vulkanComputeHardwareCases=$taskIcd.computeHardwareCases;
    pipelineCacheRoundtripValidated=$taskIcd.pipelineCacheRoundtripValidated;
    vulkanStorageImageHardwareCases=$taskIcd.storageImageHardwareCases;
    offscreenVulkanGraphicsValidated=$taskMesaBuild.graphicsModulesBuilt;
    vulkanGraphicsHardwareCases=$(if ($taskMesaBuild.graphicsModulesBuilt) {18} else {0});
    completeVulkanIcdImplemented=$false; standardVkcubeLaunched=$false; tenMinuteVkcubeValidated=$false}
$taskResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskArtifacts 'progress.json') -Encoding UTF8
Write-Output 'PASS: native transport, ELK/SPIR-V, ISL/BLORP, Vulkan compute/storage images and preserved C# controls'
$taskAttempt.status='passed'; $taskAttempt.stage='complete'
} catch {
    $taskAttempt.status='failed'; $taskAttempt.error=$_.Exception.Message
    throw
} finally {
    $taskAttempt.completedUtc=[DateTime]::UtcNow.ToString('o')
    $taskAttempt | ConvertTo-Json | Set-Content -LiteralPath $taskAttemptPath -Encoding UTF8
}
