param([switch]$UseClang)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskTools = Join-Path $taskRoot '.tools'
$taskLock=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-lock.json') -Raw | ConvertFrom-Json
# HASVK_UNLOCKED_TOOLCHAIN=1 (the CI build on a hosted runner): take the MSVC
# and Windows SDK that are installed instead of the exact builds of the lock
# and report the differences as warnings. clang-cl, which compiles the driver,
# stays pinned. Release packages are built with the locked toolchain.
$taskUnlocked = $env:HASVK_UNLOCKED_TOOLCHAIN -eq '1'
$taskDrift = @()
function Get-NewestVersionDirectory([string]$path) {
    Get-ChildItem -LiteralPath $path -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
$msvcRoot = Join-Path $taskTools 'vs\VC\Tools\MSVC'
$msvcDirectory = Get-Item -LiteralPath (Join-Path $msvcRoot $taskLock.msvc) -ErrorAction SilentlyContinue
if (-not $msvcDirectory -and $taskUnlocked) { $msvcDirectory = Get-NewestVersionDirectory $msvcRoot }
if (-not $msvcDirectory) { throw 'MSVC missing; run tools\bootstrap.ps1 first.' }
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-Item -LiteralPath (Join-Path $sdkRoot ('Include\'+$taskLock.windowsSdk)) -ErrorAction SilentlyContinue
if (-not $sdkVersion -and $taskUnlocked) { $sdkVersion = Get-NewestVersionDirectory (Join-Path $sdkRoot 'Include') }
if (-not $sdkVersion) { throw 'Windows SDK 10.0.26100 is missing.' }
$taskCompilerBin = Join-Path $msvcDirectory.FullName 'bin\Hostx64\x64'
$taskPython = Join-Path $taskTools 'python'
$taskScratch = Join-Path $taskTools 'tmp'
New-Item -ItemType Directory -Path $taskScratch -Force | Out-Null
$env:TEMP = $taskScratch
$env:TMP = $taskScratch
$taskSdkBin = Join-Path $sdkRoot "bin\$($sdkVersion.Name)\x64"
$env:PATH = "$taskCompilerBin;$taskSdkBin;$taskPython;$(Join-Path $taskPython 'Scripts');$env:PATH"
$env:INCLUDE = @((Join-Path $msvcDirectory.FullName 'include'), (Join-Path $sdkVersion.FullName 'ucrt'),
    (Join-Path $sdkVersion.FullName 'shared'), (Join-Path $sdkVersion.FullName 'um'), (Join-Path $sdkVersion.FullName 'winrt')) -join ';'
$env:LIB = @((Join-Path $msvcDirectory.FullName 'lib\x64'), (Join-Path $sdkRoot "Lib\$($sdkVersion.Name)\ucrt\x64"),
    (Join-Path $sdkRoot "Lib\$($sdkVersion.Name)\um\x64")) -join ';'
$env:CC = 'cl'
$env:CXX = 'cl'
$taskToolchain = [ordered]@{
    msvc = $msvcDirectory.Name
    compiler = (Join-Path $taskCompilerBin 'cl.exe')
    compilerSha256 = (Get-FileHash -LiteralPath (Join-Path $taskCompilerBin 'cl.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    windowsSdk = $sdkVersion.Name
    python = (Join-Path $taskPython 'python.exe')
}
if ($taskToolchain.compilerSha256 -ne $taskLock.msvcCompilerSha256) { $taskDrift += "MSVC $($msvcDirectory.Name) differs from tools\toolchain-lock.json" }
foreach ($header in $taskLock.sdkHeaders.PSObject.Properties) {
    $taskHeaderPath=Join-Path $sdkVersion.FullName ('shared\'+$header.Name)
    if ((Get-FileHash -LiteralPath $taskHeaderPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $header.Value) {
        $taskDrift += "Windows KMT ABI header differs from lock: $taskHeaderPath"
    }
}
if ($taskDrift -and -not $taskUnlocked) { throw $taskDrift[0] }
foreach ($taskDifference in $taskDrift) { Write-Warning "unlocked toolchain: $taskDifference" }
$taskToolchain.locked = -not $taskDrift
$taskClangBin=Join-Path $taskTools ('llvm-'+$taskLock.clangVersion+'\bin')
$taskClangExe=Join-Path $taskClangBin 'clang-cl.exe'
if (Test-Path -LiteralPath $taskClangExe) {
    $taskToolchain.llvmBin = $taskClangBin
    $taskToolchain.clang = $taskClangExe
    $taskToolchain.clangVersion = $taskLock.clangVersion
    $taskToolchain.clangSha256 = (Get-FileHash -LiteralPath $taskClangExe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($taskToolchain.clangSha256 -ne $taskLock.clangCompilerSha256) { throw 'clang-cl differs from tools\toolchain-lock.json' }
}
if ($UseClang) {
    if (-not (Test-Path -LiteralPath $taskClangExe)) { throw 'clang-cl missing; run tools\bootstrap.ps1 first.' }
    $env:PATH="$taskClangBin;$env:PATH"
    $env:CC=$taskClangExe
    $env:CXX=$taskClangExe
}
