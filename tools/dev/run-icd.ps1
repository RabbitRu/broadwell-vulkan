param([string]$Executable, [string[]]$Arguments, [string]$ValidationLog, [string]$LayerPath = 'C:\VulkanSDK-local\Bin')
# -ValidationLog <file>: run the client under VK_LAYER_KHRONOS_validation from
# $LayerPath (a copy-only Vulkan SDK; nothing is registered system-wide) and
# write its messages to that file.
$ErrorActionPreference='Stop'
$taskManifest=Join-Path $PSScriptRoot 'intel_hasvk_wddm_icd.json'
if (-not (Test-Path -LiteralPath $taskManifest)) {
    $taskManifest=Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'build\icd\intel_hasvk_wddm_icd.json'
}
$taskManifest=(Resolve-Path -LiteralPath $taskManifest).Path
$taskPackage=Split-Path $taskManifest -Parent
$taskDiagnostics=Join-Path $taskPackage 'diagnostics'
New-Item -ItemType Directory -Path $taskDiagnostics -Force | Out-Null
if (-not $Executable) { $Executable=Join-Path $taskPackage 'IrisIcdProbe.exe' }
if (-not $Arguments) { $Arguments=@((Join-Path $taskDiagnostics 'report.json')) }
$Executable=(Resolve-Path -LiteralPath $Executable).Path
function Quote-WindowsArgument([string]$Value) {
    '"'+[regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1')+'"'
}
$taskStart=[Diagnostics.ProcessStartInfo]::new()
$taskStart.FileName=$Executable
$taskStart.Arguments=($Arguments | ForEach-Object { Quote-WindowsArgument $_ }) -join ' '
$taskStart.WorkingDirectory=$taskPackage
$taskStart.UseShellExecute=$false
$taskStart.CreateNoWindow=$true
$taskStart.RedirectStandardOutput=$true
$taskStart.RedirectStandardError=$true
$taskStart.EnvironmentVariables['VK_DRIVER_FILES']=$taskManifest
$taskStart.EnvironmentVariables.Remove('VK_ICD_FILENAMES')
$taskStart.EnvironmentVariables.Remove('VK_ADD_DRIVER_FILES')
$taskStart.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
$taskStart.EnvironmentVariables['VK_LOADER_DEBUG']='error,warn'
$taskStart.EnvironmentVariables['PATH']=$taskPackage+';'+$env:PATH
if ($ValidationLog) {
    if (-not (Test-Path -LiteralPath (Join-Path $LayerPath 'VkLayer_khronos_validation.json'))) { throw "Validation layer not found in $LayerPath" }
    if (Test-Path -LiteralPath $ValidationLog) { Remove-Item -LiteralPath $ValidationLog -Confirm:$false }
    $taskStart.EnvironmentVariables['VK_LAYER_PATH']=$LayerPath
    $taskStart.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
    $taskStart.EnvironmentVariables['VK_KHRONOS_VALIDATION_DEBUG_ACTION']='VK_DBG_LAYER_ACTION_LOG_MSG'
    $taskStart.EnvironmentVariables['VK_KHRONOS_VALIDATION_LOG_FILENAME']=$ValidationLog
    # The probes pass object handles to the ICD's own diagnostic exports, so the
    # layer must not substitute its wrapped handles.
    $taskStart.EnvironmentVariables['VK_KHRONOS_VALIDATION_UNIQUE_HANDLES']='false'
}
$taskProcess=[Diagnostics.Process]::new()
$taskProcess.StartInfo=$taskStart
try {
    if (-not $taskProcess.Start()) { throw 'Failed to start local ICD client' }
    $taskOutput=$taskProcess.StandardOutput.ReadToEndAsync()
    $taskErrors=$taskProcess.StandardError.ReadToEndAsync()
    if (-not $taskProcess.WaitForExit(120000)) {
        $taskProcess.Kill(); $taskProcess.WaitForExit()
        throw 'Local ICD client exceeded the 120-second test deadline'
    }
    [IO.File]::WriteAllText((Join-Path $taskDiagnostics 'stdout.log'), $taskOutput.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $taskDiagnostics 'stderr.log'), $taskErrors.Result, [Text.UTF8Encoding]::new($false))
    Write-Output $taskOutput.Result
    if ($taskProcess.ExitCode) { throw "Local ICD client failed: $($taskProcess.ExitCode); see $taskDiagnostics" }
} finally { $taskProcess.Dispose() }
