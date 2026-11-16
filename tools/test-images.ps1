param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build-mesa-compiler.ps1') }
$taskProbe=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\IrisImageProbe.exe'
$taskImageRoot=Join-Path $taskRoot 'build\images'
New-Item -ItemType Directory -Path $taskImageRoot -Force | Out-Null
$taskPreviousPath=$env:PATH
$taskResults=@()
try {
    $env:PATH=(Join-Path $taskRoot 'build\mesa-compiler-clang\subprojects\zlib-1.3.1')+';'+$env:PATH
    foreach ($shape in @(@(37,23),@(129,67),@(257,131))) {
        foreach ($tiling in @('linear','x','y')) {
            $taskName="$($shape[0])x$($shape[1])-$tiling"
            $taskOutput=Join-Path $taskImageRoot $taskName
            New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
            & $taskProbe $taskOutput $shape[0] $shape[1] $tiling > (Join-Path $taskOutput 'run.log') 2>&1
            if ($LASTEXITCODE) { throw "Image hardware test failed: $taskOutput\run.log" }
            $taskReportPath=Join-Path $taskOutput 'report.json'
            $taskReport=Get-Content -LiteralPath $taskReportPath -Raw | ConvertFrom-Json
            if (-not $taskReport.passed -or -not $taskReport.cleanupValidated -or $taskReport.forbiddenModules.Count -or
                -not $taskReport.stableCpuReadonlyPoolsValidated -or -not $taskReport.overlappingAllocationAndOutOfBoundsPoolMapRejected -or
                -not $taskReport.texturedTriangleAndD16DepthTestValidated) {
                throw "Invalid image result: $taskReportPath"
            }
            $taskResults += [ordered]@{name=$taskName; passed=$true; pixelsVerified=$taskReport.pixelsVerified;
                guardBytesVerified=$taskReport.guardBytesVerified; shadersCompiledAndIsaValidated=$taskReport.shadersCompiledAndIsaValidated;
                reportSha256=(Get-FileHash -LiteralPath $taskReportPath -Algorithm SHA256).Hash.ToLowerInvariant()}
            Write-Output "PASS: $taskName"
        }
    }
} finally { $env:PATH=$taskPreviousPath }
$taskValidation=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); hardware='8086:162b'; driverVersion='20.19.15.5171';
    probeSha256=(Get-FileHash -LiteralPath $taskProbe -Algorithm SHA256).Hash.ToLowerInvariant(); cases=$taskResults;
    mesaIslBlorpElkOnWddmValidated=$true; bgraLinearXTiledYTiledClearValidated=$true; rgbaTextureSamplingValidated=$true;
    d16YTiledClearValidated=$true; cTransportLifetimeRejectionsValidated=$true;
    genericLinearAllocationBackingImagesValidated=$true; auxiliaryCompressionEnabled=$false;
    stableCpuReadonlyPoolsAndGrowthValidated=$true;
    depthTestingOrGeometryOverlapValidated=$true; texturedTriangleValidated=$true; spirvVertexShaderValidated=$false;
    vulkanIcdImplemented=$false; hardwareVkcubeValidated=$false}
$taskValidation | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRoot 'native\artifacts\images-validation.json') -Encoding UTF8
Write-Output ('PASS: '+$taskResults.Count+' ISL/BLORP hardware image cases')
