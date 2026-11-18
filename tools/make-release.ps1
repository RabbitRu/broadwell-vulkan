# Release package: build\release\HasVK-WDDM-<date>-<commit>.zip with the ICD
# (vulkan_hasvk_wddm.dll, z-1.dll, manifest), the install script and the
# installation guide, plus SHA-256 sums. Test executables are not included.
#   .\tools\make-release.ps1            # package the current build\icd
param([switch]$Build)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
if ($Build) { & (Join-Path $PSScriptRoot 'build-icd.ps1') | Out-Null }
$taskPackage = Join-Path $taskRoot 'build\icd'
$git = Join-Path $taskRoot '.tools\mingit\cmd\git.exe'
$commit = (& $git -C $taskRoot rev-parse --short HEAD).Trim()
$dirty = [bool](& $git -C $taskRoot status --porcelain --untracked-files=no -- mesa native tools)
$name = 'HasVK-WDDM-' + (Get-Date -Format 'yyyyMMdd') + '-' + $commit + $(if ($dirty) { '-dirty' } else { '' })
$stage = Join-Path $taskRoot ("build\release\" + $name)
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Path (Join-Path $stage 'icd'), (Join-Path $stage 'tools') -Force | Out-Null
foreach ($file in @('vulkan_hasvk_wddm.dll', 'z-1.dll', 'intel_hasvk_wddm_icd.json', 'IrisIcdProbe.exe')) {
    Copy-Item -LiteralPath (Join-Path $taskPackage $file) -Destination (Join-Path $stage 'icd')
}
Copy-Item -LiteralPath (Join-Path $taskPackage 'shaders') -Destination (Join-Path $stage 'icd') -Recurse   # for IrisIcdProbe
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install-icd.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $taskRoot 'docs\install.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $taskRoot 'docs\cts-expected-failures.md') -Destination $stage
$sums = Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $sums)
$zip = $stage + '.zip'
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Confirm:$false }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Output $zip
