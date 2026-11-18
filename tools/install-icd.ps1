# System registration of the HasVK/WDDM ICD, so that any Vulkan application
# finds it without VK_DRIVER_FILES.
#
#   .\tools\install-icd.ps1 -WhatIf          # show what would be done, change nothing
#   .\tools\install-icd.ps1                  # install (administrator PowerShell)
#   .\tools\install-icd.ps1 -Uninstall       # remove files and registration
#
# Install copies the ICD package (build\icd: vulkan_hasvk_wddm.dll, z-1.dll and
# the manifest) to -Destination (default %ProgramFiles%\HasVK-WDDM) and adds the
# manifest to HKLM\SOFTWARE\Khronos\Vulkan\Drivers (REG_DWORD 0 = enabled),
# where the Vulkan loader looks for drivers. 64-bit applications only: the ICD
# is 64-bit, so nothing is registered for 32-bit (WOW6432Node) applications.
# Only for the Iris 6100 (8086:162b) with Intel driver 20.19.15.5171: the ICD
# reports no device on anything else.
[CmdletBinding(SupportsShouldProcess)]
param([switch]$Uninstall, [string]$Destination = (Join-Path $env:ProgramFiles 'HasVK-WDDM'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskKey = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
$taskManifest = Join-Path $Destination 'intel_hasvk_wddm_icd.json'
$taskFiles = @('vulkan_hasvk_wddm.dll', 'z-1.dll', 'intel_hasvk_wddm_icd.json')
$taskAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $taskAdmin -and -not $WhatIfPreference) { throw 'Run from an administrator PowerShell (or pass -WhatIf to only show the steps).' }

if ($Uninstall) {
    if ((Test-Path $taskKey) -and $null -ne (Get-ItemProperty -Path $taskKey -Name $taskManifest -ErrorAction SilentlyContinue)) {
        if ($PSCmdlet.ShouldProcess("$taskKey\$taskManifest", 'Remove ICD registration')) { Remove-ItemProperty -Path $taskKey -Name $taskManifest }
    } else { Write-Output "Not registered: $taskManifest" }
    foreach ($name in $taskFiles) {
        $path = Join-Path $Destination $name
        if ((Test-Path -LiteralPath $path) -and $PSCmdlet.ShouldProcess($path, 'Delete file')) { Remove-Item -LiteralPath $path -Confirm:$false }
    }
    if ((Test-Path -LiteralPath $Destination) -and -not (Get-ChildItem -LiteralPath $Destination -Force) -and $PSCmdlet.ShouldProcess($Destination, 'Remove empty directory')) {
        Remove-Item -LiteralPath $Destination -Confirm:$false
    }
    if ($WhatIfPreference) { Write-Output 'Dry run: nothing was changed.' } else { Write-Output 'Uninstalled.' }
    return
}

# The source tree's build\icd, or icd\ of an unpacked release package.
$taskPackage = @((Join-Path $taskRoot 'build\icd'), (Join-Path $taskRoot 'icd')) | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'vulkan_hasvk_wddm.dll') } | Select-Object -First 1
if (-not $taskPackage) { throw "No ICD package next to this script: run tools\build-icd.ps1 first." }
foreach ($name in $taskFiles) { if (-not (Test-Path -LiteralPath (Join-Path $taskPackage $name))) { throw "Missing $name in ${taskPackage}: run tools\build-icd.ps1 first." } }
# Other registered drivers stay as they are; only report them.
if (Test-Path $taskKey) {
    (Get-Item $taskKey).Property | Where-Object { $_ -ne $taskManifest } | ForEach-Object { Write-Output "Other registered Vulkan driver (left as is): $_" }
}
if (-not (Test-Path -LiteralPath $Destination) -and $PSCmdlet.ShouldProcess($Destination, 'Create directory')) { New-Item -ItemType Directory -Path $Destination | Out-Null }
foreach ($name in $taskFiles) {
    $source = Join-Path $taskPackage $name
    if ($PSCmdlet.ShouldProcess((Join-Path $Destination $name), "Copy from $source")) { Copy-Item -LiteralPath $source -Destination $Destination -Force }
}
if (-not (Test-Path $taskKey) -and $PSCmdlet.ShouldProcess($taskKey, 'Create registry key')) { New-Item -Path $taskKey -Force | Out-Null }
if ($PSCmdlet.ShouldProcess("$taskKey\$taskManifest", 'Register ICD (REG_DWORD 0)')) {
    New-ItemProperty -Path $taskKey -Name $taskManifest -PropertyType DWord -Value 0 -Force | Out-Null
}
if (-not $WhatIfPreference) {
    # The installed copy must load through the loader without VK_DRIVER_FILES.
    $probe = Join-Path $taskPackage 'IrisIcdProbe.exe'
    if (Test-Path -LiteralPath $probe) {
        $report = Join-Path $env:TEMP 'hasvk-install-check.json'
        $start = [Diagnostics.ProcessStartInfo]::new($probe, ('"' + $report + '"'))
        $start.UseShellExecute = $false; $start.WorkingDirectory = $taskPackage
        $start.EnvironmentVariables.Remove('VK_DRIVER_FILES'); $start.EnvironmentVariables.Remove('VK_ICD_FILENAMES')
        $p = [Diagnostics.Process]::Start($start); $p.WaitForExit()
        if ($p.ExitCode) { Write-Warning "The installed ICD did not pass IrisIcdProbe (exit $($p.ExitCode)); see $report" } else { Write-Output 'Installed ICD passes IrisIcdProbe through the system loader.' }
    }
}
if ($WhatIfPreference) { Write-Output 'Dry run: nothing was changed.' } else { Write-Output "Installed to $Destination." }
