param([Parameter(Mandatory)][string]$Executable, [string[]]$Arguments = @(), [switch]$Validate, [string]$LayerPath = 'C:\VulkanSDK-local\Bin', [int]$Frames = 12, [string]$WorkingDirectory)
# -WorkingDirectory: where the client must run (deqp-vk needs its own data directory).
# Runs a client through the local ICD under lldb (batch mode) and prints the
# symbolized stack of the first crash. Addresses inside the ICD and the client
# are resolved with llvm-symbolizer because lldb does not load their PDBs.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskLlvmBin=Join-Path $taskRoot ('.tools\llvm-'+(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-lock.json') -Raw | ConvertFrom-Json).clangVersion+'\bin')
$taskPackage=Join-Path $taskRoot 'build\icd'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$taskSaved=@{}
function Set-TaskEnv([string]$Name, [string]$Value) { $taskSaved[$Name]=[Environment]::GetEnvironmentVariable($Name); [Environment]::SetEnvironmentVariable($Name, $Value) }
try {
    Set-TaskEnv 'VK_DRIVER_FILES' (Join-Path $taskPackage 'intel_hasvk_wddm_icd.json')
    Set-TaskEnv 'VK_LOADER_LAYERS_DISABLE' '~implicit~'
    Set-TaskEnv 'PYTHONHOME' (Join-Path $taskRoot '.tools\python310-debug')
    Set-TaskEnv 'PATH' ($taskPackage+';'+(Join-Path $taskRoot '.tools\python310-debug')+';'+$env:PATH)
    if ($Validate) { Set-TaskEnv 'VK_LAYER_PATH' $LayerPath; Set-TaskEnv 'VK_INSTANCE_LAYERS' 'VK_LAYER_KHRONOS_validation' }
    Push-Location $(if ($WorkingDirectory) { $WorkingDirectory } else { $taskPackage })
    try {
        $ErrorActionPreference='Continue'   # Windows PowerShell turns native stderr into errors
        # The LLVM release archive has no lldb; use the one from a full LLVM install
        # in .tools\llvm (LLVM-19.1.5-win64.exe, linked against Python 3.10).
        $taskLldb=Join-Path $taskLlvmBin 'lldb.exe'
        if (-not (Test-Path -LiteralPath $taskLldb)) { $taskLldb=Join-Path $taskRoot '.tools\llvm\bin\lldb.exe' }
        $taskOutput=& $taskLldb --batch -o run -k "bt $Frames" -k 'image list -o -f' -k quit -- $Executable @Arguments 2>$null
    } finally { Pop-Location }
} finally { foreach ($name in $taskSaved.Keys) { [Environment]::SetEnvironmentVariable($name, $taskSaved[$name]) } }
$taskSlides=@{}
foreach ($line in $taskOutput) { if ($line -match '\[\s*\d+\]\s+(0x[0-9a-fA-F]+)\s+(.+)$') { $taskSlides[[IO.Path]::GetFileName($Matches[2].Trim())]=@([Convert]::ToUInt64($Matches[1],16), $Matches[2].Trim()) } }
$taskSymbolizer=Join-Path $taskLlvmBin 'llvm-symbolizer.exe'
$taskStop=$taskOutput | Where-Object { $_ -match 'stop reason' } | Select-Object -First 1
if (-not $taskStop) { 'No crash: process exited normally'; $taskOutput | Select-Object -Last 3; return }
$taskStop
$taskSeen=@{}
foreach ($line in $taskOutput) {
    if ($line -match 'frame #(\d+): (0x[0-9a-fA-F]+)(?: ([^`\s]+)`?(.*))?') {
        if ($taskSeen[$Matches[1]]) { continue }; $taskSeen[$Matches[1]]=1
        $index=$Matches[1]; $address=[Convert]::ToUInt64($Matches[2],16); $module=$Matches[3]; $text=$Matches[4]
        $object=$null
        if ($module -eq 'vulkan_hasvk_wddm.dll') { $object=Join-Path $taskRoot 'build\mesa-compiler-clang\src\intel\wddm_compiler\vulkan_hasvk_wddm.dll' }
        elseif ($module -and $taskSlides[$module] -and $taskSlides[$module][1] -eq $Executable) { $object=$Executable }
        if ($object -and $address) {
            $resolved=& $taskSymbolizer --obj=$object -i -p ('0x'+($address-$taskSlides[$module][0]-1).ToString('x'))
            "#$index $module " + (($resolved | Where-Object { $_ }) -join ' <- ')
        } else { "#$index $module $text" }
    }
}
