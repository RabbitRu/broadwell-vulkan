$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
& $taskToolchain.python (Join-Path $PSScriptRoot 'prepare-vulkan-tools.py')
if ($LASTEXITCODE) { throw 'Vulkan-Tools source preparation failed' }
$taskDependencies=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
$taskCube=Join-Path $taskRoot ('third_party\'+$taskDependencies.vulkanTools.directory+'\cube')
$taskHeaders=Join-Path $taskRoot ('third_party\'+$taskDependencies.vulkanHeaders.directory+'\include')
$taskVolk=Join-Path $taskRoot ('third_party\'+$taskDependencies.volk.directory)
$taskBuild=Join-Path $taskRoot 'build\vkcube'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
Push-Location $taskBuild
try {
    & cl.exe /nologo /TC /std:c11 /MT /O2 /Zi /utf-8 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DVK_USE_PLATFORM_WIN32_KHR /I $taskHeaders /I $taskVolk /I $taskCube (Join-Path $taskCube 'cube.c') /Fe:vkcube.exe /link /DEBUG user32.lib gdi32.lib shell32.lib
    if ($LASTEXITCODE) { throw 'Unmodified Khronos vkcube build failed' }
    $taskManifest=[ordered]@{builtUtc=[DateTime]::UtcNow.ToString('o'); vulkanTools='1.3.301';
        vulkanToolsArchiveSha256=$taskDependencies.vulkanTools.sha256; vulkanHeaders='1.3.301'; volkCommit=$taskDependencies.volk.commit;
        upstreamSourcesModified=$false; precompiledUpstreamSpirvUsed=$true; win32Wsi=$true; hardwareExecutionValidated=$false;
        executableSha256=(Get-FileHash .\vkcube.exe -Algorithm SHA256).Hash.ToLowerInvariant();
        sourceFiles=@('cube.c','cube.vert.inc','cube.frag.inc' | ForEach-Object {[ordered]@{file=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $taskCube $_) -Algorithm SHA256).Hash.ToLowerInvariant()}})}
    $taskManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath .\build.json -Encoding UTF8
} finally { Pop-Location }
