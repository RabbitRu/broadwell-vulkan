param([switch]$BuildOnly, [switch]$ProbeVa, [switch]$RunOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskBuild = Join-Path $taskRoot 'build\native'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$taskSources = @('native\src\hasvk_wddm.cpp','native\src\hasvk_wddm_c.cpp','native\src\intel_2019_profile.cpp','native\tests\probe.cpp','native\tests\icd_probe.cpp','native\tests\graphics_probe.cpp')
$taskIncludes = @('/I', (Join-Path $taskRoot 'native\include'), '/I', (Join-Path $taskRoot 'third_party'), '/I', (Join-Path $taskRoot 'third_party\mesa-26.2.7\include'))
Push-Location $taskBuild
try {
    if (-not $RunOnly) {
    & $taskToolchain.python (Join-Path $PSScriptRoot 'gen-native-packets.py')
    if ($LASTEXITCODE) { throw 'Gen8 packet header generation failed' }
    foreach ($source in $taskSources) {
        & cl.exe /nologo /c /std:c++17 /EHsc /MT /O2 /Zi /utf-8 /W4 /WX /D_WIN32_WINNT=0x0A00 @taskIncludes (Join-Path $taskRoot $source)
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
    }
    if (-not $taskToolchain.clang) { throw 'clang-cl required for Mesa Gen8 packet helpers; run tools\bootstrap.ps1.' }
    $taskPacketIncludes = @('/I', $taskBuild, '/I', (Join-Path $taskRoot 'third_party\mesa-26.2.7\src'),
        '/I', (Join-Path $taskRoot 'third_party\mesa-26.2.7\include'), '/I', (Join-Path $taskRoot 'third_party\mesa-26.2.7\src\intel\genxml'))
    & $taskToolchain.clang /nologo /c /TC /std:c11 /MT /O2 /Zi /utf-8 /W4 /WX -Wno-unused-function @taskIncludes @taskPacketIncludes (Join-Path $taskRoot 'native\src\gen8_compute.c')
    if ($LASTEXITCODE) { throw 'Gen8 compute state compiler failed' }
    & lib.exe /nologo /OUT:hasvk_wddm.lib hasvk_wddm.obj hasvk_wddm_c.obj intel_2019_profile.obj
    if ($LASTEXITCODE -ne 0) { throw 'Static library build failed' }
    & link.exe /nologo /OUT:HasvkWddmProbe.exe /DEBUG probe.obj gen8_compute.obj hasvk_wddm.lib gdi32.lib dxgi.lib bcrypt.lib
    if ($LASTEXITCODE -ne 0) { throw 'Native probe link failed' }
    & link.exe /nologo /OUT:IrisIcdProbe.exe /DEBUG icd_probe.obj
    if ($LASTEXITCODE -ne 0) { throw 'ICD probe link failed' }
    & link.exe /nologo /OUT:IrisGraphicsProbe.exe /DEBUG graphics_probe.obj
    if ($LASTEXITCODE -ne 0) { throw 'Graphics probe link failed' }
    $taskManifest = [ordered]@{
        builtUtc = [DateTime]::UtcNow.ToString('o')
        toolchain = $taskToolchain
        executableSha256 = (Get-FileHash .\HasvkWddmProbe.exe -Algorithm SHA256).Hash.ToLowerInvariant()
        gen8PackHeaderSha256 = (Get-FileHash .\gen8_pack.h -Algorithm SHA256).Hash.ToLowerInvariant()
        sources = @($taskSources + @('native\src\gen8_compute.c','native\include\hasvk_wddm.hpp','native\include\hasvk_wddm.h','native\include\gen8_compute.h','tools\gen-native-packets.py') | ForEach-Object { [ordered]@{ file=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $taskRoot $_) -Algorithm SHA256).Hash.ToLowerInvariant() } })
    }
    $taskManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath .\build.json -Encoding UTF8
    }
    if ($BuildOnly) { return }
    $capture = Join-Path $taskRoot 'gpu-probe\artifacts\compute-bootstrap\compute-capture'
    $bootstrap = Join-Path $taskRoot 'gpu-probe\artifacts\native-bootstrap\bootstrap'
    $taskCases = @(@('profile',0), @('store',0), @('compute',16381), @('compute',257), @('compute',1))
    foreach ($taskBytes in @(4096,8192,12288,65536,69632,131072,1048576,4194304)) {
        $taskCases += ,@('generated-store',$taskBytes)
    }
    foreach ($taskCount in @(16381,257,1)) { $taskCases += ,@('generated-compute',$taskCount) }
    if ($ProbeVa) { $taskCases += ,@('va',0); $taskCases += ,@('va-pool',0); $taskCases += ,@('va-layout',0) }
    $taskCases += ,@('batch-chain',0)
    foreach ($case in $taskCases) {
        $taskOutput = Join-Path $taskBuild ($case[0]+'-'+$case[1])
        & .\HasvkWddmProbe.exe $case[0] $case[1] $capture $bootstrap $taskOutput
        if ($LASTEXITCODE -ne 0) { throw "Native probe failed: $taskOutput\report.json" }
    }
} finally { Pop-Location }
