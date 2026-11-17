param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-mesa-compiler.ps1')
    & (Join-Path $PSScriptRoot 'build-native.ps1') -BuildOnly
}
$taskPackage=Join-Path $taskRoot 'build\icd'
New-Item -ItemType Directory -Path $taskPackage,(Join-Path $taskPackage 'diagnostics') -Force | Out-Null
$taskShaders=Join-Path $taskPackage 'shaders'
New-Item -ItemType Directory -Path $taskShaders -Force | Out-Null
$taskShaderInputs=Join-Path $taskRoot 'build\icd-shader-inputs'
& (Join-Path $taskRoot '.tools\python\python.exe') (Join-Path $PSScriptRoot 'gen-spirv-probes.py') --output $taskShaderInputs
if ($LASTEXITCODE) { throw 'ICD test SPIR-V generation failed' }
& (Join-Path $taskRoot '.tools\python\python.exe') (Join-Path $PSScriptRoot 'gen-storage-image-probes.py') --output $taskShaderInputs
if ($LASTEXITCODE) { throw 'ICD image SPIR-V generation failed' }
& (Join-Path $taskRoot '.tools\python\python.exe') (Join-Path $PSScriptRoot 'gen-graphics-probes.py') --output $taskShaderInputs
if ($LASTEXITCODE) { throw 'ICD graphics SPIR-V generation failed' }
& (Join-Path $taskRoot '.tools\python\python.exe') (Join-Path $PSScriptRoot 'gen-compute-probes.py') --output $taskShaderInputs
if ($LASTEXITCODE) { throw 'ICD compute SPIR-V generation failed' }
foreach ($name in @('storage-image-write.spv','storage-image-read.spv','triangle.vert.spv','triangle.frag.spv','textured.vert.spv','textured.frag.spv','attributes.vert.spv','attributes.frag.spv','compute-ids3d.spv','compute-shared.spv')) {
    Copy-Item -LiteralPath (Join-Path $taskShaderInputs $name) -Destination $taskShaders
}
foreach ($algorithm in @('xor32','lcg32','mix32')) {
    foreach ($suffix in @('','-local7','-local32','-local64')) {
        Copy-Item -LiteralPath (Join-Path $taskShaderInputs ($algorithm+$suffix+'.spv')) -Destination $taskShaders
    }
}
$taskSource=Join-Path $taskRoot 'build\mesa-compiler-clang'
Copy-Item -LiteralPath (Join-Path $taskSource 'src\intel\wddm_compiler\vulkan_hasvk_wddm.dll') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskSource 'subprojects\zlib-1.3.1\z-1.dll') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskRoot 'build\native\IrisIcdProbe.exe') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $taskRoot 'build\native\IrisGraphicsProbe.exe') -Destination $taskPackage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-icd.ps1') -Destination (Join-Path $taskPackage 'Run-Icd.ps1')
$taskManifest=[ordered]@{file_format_version='1.0.0'; ICD=[ordered]@{library_path='.\vulkan_hasvk_wddm.dll'; api_version='1.2.0'}}
[IO.File]::WriteAllText((Join-Path $taskPackage 'intel_hasvk_wddm_icd.json'), ($taskManifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
$taskMesaManifest=Get-Content -LiteralPath (Join-Path $taskSource 'build.json') -Raw | ConvertFrom-Json
$taskPackageManifest=[ordered]@{builtUtc=[DateTime]::UtcNow.ToString('o'); scope='experimental hardware ICD; consult matching validation hashes for tested functionality'; graphicsModulesBuilt=$taskMesaManifest.graphicsModulesBuilt;
    systemIcdRegistered=$false; completeVulkanImplementation=$false;
    files=@(@(Get-ChildItem -LiteralPath $taskPackage -File | Where-Object {$_.Name -ne 'package.json'}) + @(Get-ChildItem -LiteralPath $taskShaders -Filter '*.spv' -File) | ForEach-Object {
        [ordered]@{name=$_.FullName.Substring($taskPackage.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })}
$taskPackageManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskPackage 'package.json') -Encoding UTF8
Write-Output $taskPackage
