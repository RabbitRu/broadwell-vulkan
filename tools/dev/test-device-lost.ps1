# Device loss on a real GPU hang (native\tests\device_lost_probe.cpp, shader
# native\tests\hang.comp): Windows resets the GPU (TDR; the screen freezes for
# a second or two). Expects VK_ERROR_DEVICE_LOST, no blocking afterwards, and
# a working new device in the same process. Report: native\artifacts\device-lost.json.
param([switch]$SkipBuild, [string]$SdkPath = 'C:\VulkanSDK-local\Bin')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SkipBuild) { & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-native.ps1') -BuildOnly; & (Join-Path (Split-Path $PSScriptRoot -Parent) 'build-icd.ps1') -SkipBuild | Out-Null }
$taskPackage = Join-Path $taskRoot 'build\icd'
$taskShader = Join-Path $taskPackage 'hang.spv'
# glslang cannot open the project's non-ASCII path: compile through %TEMP% (8.3, ASCII).
$taskTemp = Join-Path $env:TEMP 'hasvk-hang'
New-Item -ItemType Directory -Path $taskTemp -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRoot 'native\tests\hang.comp') -Destination (Join-Path $taskTemp 'hang.comp') -Force
$ErrorActionPreference = 'Continue'   # glslang prints the file name on stderr
& (Join-Path $SdkPath 'glslangValidator.exe') -V -o (Join-Path $taskTemp 'hang.spv') (Join-Path $taskTemp 'hang.comp') 2>&1 | Out-Null
$taskGlslang = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($taskGlslang) { throw 'hang.comp compilation failed' }
Copy-Item -LiteralPath (Join-Path $taskTemp 'hang.spv') -Destination $taskShader -Force
$taskReport = Join-Path $taskRoot 'native\artifacts\device-lost.json'
& (Join-Path $PSScriptRoot 'run-icd.ps1') -Executable (Join-Path $taskPackage 'IrisDeviceLostProbe.exe') -Arguments @($taskReport, $taskShader)
