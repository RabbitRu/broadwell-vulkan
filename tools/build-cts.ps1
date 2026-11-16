param([string]$Tag = 'vulkan-cts-1.4.5.5', [string]$Root = 'C:\VulkanCTS-local', [int]$Jobs = 3, [string]$CMakeVersion = '3.31.6')
# Builds the Khronos Vulkan CTS (deqp-vk) for running against the local ICD.
# Sources: github.com/KhronosGroup/VK-GL-CTS at a release tag plus the
# dependencies its own external/fetch_sources.py pins; CMake from PyPI.
# Everything lives under $Root (an ASCII path: the project folder is not one).
# The build takes hours on this machine; progress goes to $Root\build.log.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
New-Item -ItemType Directory -Path $Root -Force | Out-Null
$taskLog=Join-Path $Root 'build.log'
function Step([string]$Name, [scriptblock]$Action) {
    Add-Content -LiteralPath $taskLog -Value ("=== {0} {1}" -f [DateTime]::Now.ToString('s'), $Name) -Encoding UTF8
    $ErrorActionPreference='Continue'
    & $Action *>> $taskLog
    $code=$LASTEXITCODE; $ErrorActionPreference='Stop'
    if ($code) { Add-Content -LiteralPath $taskLog -Value "FAILED: $Name ($code)" -Encoding UTF8; throw "$Name failed ($code); see $taskLog" }
}
$taskGit=Join-Path $taskRoot '.tools\mingit\cmd'
$env:PATH=$taskGit+';'+(Split-Path $taskToolchain.python -Parent)+';'+(Join-Path (Split-Path $taskToolchain.python -Parent) 'Scripts')+';'+$env:PATH
Step 'cmake from PyPI' { & $taskToolchain.python -m pip install --disable-pip-version-check "cmake==$CMakeVersion" }
# CMake writes its own path and Python's into batch files, where a non-ASCII
# character (as in this project's folder) is mangled. Use ASCII-path copies.
$taskPythonCopy=Join-Path $Root 'python'; $taskCMakeCopy=Join-Path $Root 'cmake'
Step 'copy python and cmake to an ASCII path' {
    & robocopy.exe (Split-Path $taskToolchain.python -Parent) $taskPythonCopy /MIR /NFL /NDL /NJH /NP /XD __pycache__ | Out-Null
    if ($LASTEXITCODE -ge 8) { exit $LASTEXITCODE }
    & robocopy.exe (Join-Path (Split-Path $taskToolchain.python -Parent) 'Lib\site-packages\cmake\data') $taskCMakeCopy /MIR /NFL /NDL /NJH /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { exit $LASTEXITCODE }
    $global:LASTEXITCODE=0
}
$taskPythonExe=Join-Path $taskPythonCopy 'python.exe'; $taskCMake=Join-Path $taskCMakeCopy 'bin\cmake.exe'
$env:PATH=(Join-Path $taskCMakeCopy 'bin')+';'+$taskPythonCopy+';'+$env:PATH
$taskSource=Join-Path $Root 'src'
if (-not (Test-Path -LiteralPath (Join-Path $taskSource 'CMakeLists.txt'))) {
    Step "clone $Tag" { & git.exe -c core.longpaths=true clone --depth 1 --branch $Tag https://github.com/KhronosGroup/VK-GL-CTS.git $taskSource }
}
Step 'fetch pinned dependencies' { Push-Location $taskSource; try { & $taskPythonExe external\fetch_sources.py } finally { Pop-Location } }
$taskBuild=Join-Path $Root 'build'
$taskCache=Join-Path $taskBuild 'CMakeCache.txt'
if ((Test-Path -LiteralPath $taskCache) -and -not (Select-String -LiteralPath $taskCache -SimpleMatch ($taskCMakeCopy -replace '\\','/') -Quiet)) { Remove-Item -LiteralPath $taskCache -Confirm:$false }
Step 'configure' { & $taskCMake -S $taskSource -B $taskBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DPython3_EXECUTABLE=$taskPythonExe" "-DPYTHON_EXECUTABLE=$taskPythonExe" '-DSELECTED_BUILD_TARGETS=deqp-vk' }
Step 'build deqp-vk' { & $taskCMake --build $taskBuild --target deqp-vk -- -j $Jobs }
$taskExe=Get-ChildItem -LiteralPath $taskBuild -Recurse -Filter deqp-vk.exe | Select-Object -First 1
if (-not $taskExe) { throw 'deqp-vk.exe not produced' }
[ordered]@{tag=$Tag; builtUtc=[DateTime]::UtcNow.ToString('o'); executable=$taskExe.FullName;
    sha256=(Get-FileHash -LiteralPath $taskExe.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); cmake=$CMakeVersion} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root 'build.json') -Encoding UTF8
Add-Content -LiteralPath $taskLog -Value "=== DONE $($taskExe.FullName)" -Encoding UTF8
