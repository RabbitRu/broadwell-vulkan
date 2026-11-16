$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskTools = Join-Path $taskRoot '.tools'
$taskDownloads = Join-Path $taskTools 'downloads'
$taskDrive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskRoot))
if ($taskDrive.AvailableFreeSpace -lt 25GB) { throw 'At least 25 GiB free space is required before installing build tools.' }
$taskDependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $taskDownloads,(Join-Path $taskRoot 'third_party\nlohmann') -Force | Out-Null
$taskScratch = Join-Path $taskTools 'tmp'
New-Item -ItemType Directory -Path $taskScratch -Force | Out-Null
$env:TEMP = $taskScratch
$env:TMP = $taskScratch
function Get-PinnedFile($dependency, $destination, [switch]$Signed) {
    if (-not (Test-Path -LiteralPath $destination)) {
        & curl.exe -fLsS --retry 2 --connect-timeout 15 --max-time 600 -o $destination $dependency.url
        if ($LASTEXITCODE) { throw "Download failed: $($dependency.url)" }
    }
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dependency.sha256) {
        throw "Dependency checksum mismatch: $destination"
    }
    if ($Signed -and (Get-AuthenticodeSignature -LiteralPath $destination).Status -ne 'Valid') {
        throw "Installer signature validation failed: $destination"
    }
}
$taskPythonInstaller = Join-Path $taskDownloads 'python-3.12.10-amd64.exe'
Get-PinnedFile $taskDependencies.python $taskPythonInstaller -Signed
$taskPythonDirectory = Join-Path $taskTools 'python'
if (-not (Test-Path -LiteralPath (Join-Path $taskPythonDirectory 'python.exe'))) {
    $taskArgs = '/quiet InstallAllUsers=0 TargetDir="'+$taskPythonDirectory+'" Include_launcher=0 Include_test=0 PrependPath=0 Shortcuts=0'
    $taskProcess = Start-Process -FilePath $taskPythonInstaller -ArgumentList $taskArgs -WindowStyle Hidden -Wait -PassThru
    if ($taskProcess.ExitCode -notin @(0,3010)) { throw "Python install failed: $($taskProcess.ExitCode)" }
}
$taskVsInstaller = Join-Path $taskDownloads 'vs_BuildTools.exe'
Get-PinnedFile $taskDependencies.msvcBootstrapper $taskVsInstaller -Signed
$taskVsDirectory = Join-Path $taskTools 'vs'
if (-not (Test-Path -LiteralPath (Join-Path $taskVsDirectory 'VC\Tools\MSVC'))) {
    $taskArgs = '--quiet --wait --norestart --nocache --installPath "'+$taskVsDirectory+'"'
    if (Test-Path -LiteralPath (Join-Path $taskVsDirectory 'VC\Tools\MSVC')) { $taskArgs = 'modify '+$taskArgs }
    foreach ($component in $taskDependencies.msvcBootstrapper.components) { $taskArgs += ' --add '+$component }
    $taskProcess = Start-Process -FilePath $taskVsInstaller -ArgumentList $taskArgs -WindowStyle Hidden -Wait -PassThru
    if ($taskProcess.ExitCode -notin @(0,3010)) { throw "MSVC install failed: $($taskProcess.ExitCode)" }
}
$taskPythonExe = Join-Path $taskPythonDirectory 'python.exe'
& $taskPythonExe -m pip install --disable-pip-version-check --no-warn-script-location -r (Join-Path $PSScriptRoot 'requirements.txt')
if ($LASTEXITCODE) { throw 'Python dependency installation failed' }
Get-PinnedFile $taskDependencies.mesa (Join-Path $taskDownloads 'mesa-26.2.7.tar.xz')
Get-PinnedFile $taskDependencies.nlohmann (Join-Path $taskRoot 'third_party\nlohmann\json.hpp')
& $taskPythonExe (Join-Path $PSScriptRoot 'prepare-sources.py')
if ($LASTEXITCODE) { throw 'Mesa source preparation failed' }
$taskCache = Join-Path $taskRoot 'third_party\mesa-26.2.7\subprojects\packagecache'
New-Item -ItemType Directory -Path $taskCache -Force | Out-Null
Get-PinnedFile $taskDependencies.zlib (Join-Path $taskCache 'zlib-1.3.1.tar.gz')
Get-PinnedFile $taskDependencies.zlibMeson (Join-Path $taskCache 'zlib_1.3.1-1_patch.zip')
& $taskPythonExe (Join-Path $PSScriptRoot 'fetch-clang.py')
if ($LASTEXITCODE) { throw 'Pinned x64 Clang preparation failed' }
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
$taskToolchain | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskTools 'installed-toolchain.json') -Encoding UTF8
& $taskPythonExe -m pip freeze | Set-Content -LiteralPath (Join-Path $taskTools 'installed-python.txt') -Encoding UTF8
