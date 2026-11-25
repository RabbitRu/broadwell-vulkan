param([string[]]$Order, [string]$Prefix = 'ub', [string]$IcdDir = 'build\icd-exp', [switch]$Sample)
$ErrorActionPreference = 'Continue'
$icd = (Resolve-Path (Join-Path $IcdDir 'intel_hasvk_wddm_icd.json')).Path
# variant -> environment (set in this process: run-dota passes it on; its -Env splits on commas)
$variants = @{
   base  = @{}
   cw    = @{ HASVK_EXP_CLAMP = '2048' }
   call  = @{ HASVK_EXP_CLAMP = '1' }
   m38   = @{ HASVK_EXP_MOCS_RT = '38,8' }
   m38a  = @{ HASVK_EXP_MOCS_RT = '38' }
   m18   = @{ HASVK_EXP_MOCS_RT = '18,8' }
   m58   = @{ HASVK_EXP_MOCS_RT = '58,8' }
   m68   = @{ HASVK_EXP_MOCS_RT = '68,8' }
   m70   = @{ HASVK_EXP_MOCS_RT = '70,8' }
   a79   = @{ HASVK_EXP_MOCS = '79' }
   a7b   = @{ HASVK_EXP_MOCS = '7b' }
   a38rt = @{ HASVK_EXP_MOCS_RT = '38' }
   ccs   = @{ HASVK_EXP_CCS = '1' }
   hiz1  = @{ HASVK_EXP_HIZ1 = '1' }
   both  = @{ HASVK_EXP_CCS = '1'; HASVK_EXP_HIZ1 = '1' }
   on     = @{}
   off    = @{ HASVK_EXP_CCS = '0' }
   noskip = @{ HASVK_EXP_CCS_NOSKIP = '1' }
}
$names = 'HASVK_EXP_CLAMP', 'HASVK_EXP_MOCS_RT', 'HASVK_EXP_MOCS', 'HASVK_EXP_CCS', 'HASVK_EXP_HIZ1', 'HASVK_EXP_CCS_NOSKIP'
$rows = @()
$n = 0
"=== $Prefix " + (Get-Date)
foreach ($v in $Order) {
   $n++
   foreach ($name in $names) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
   foreach ($k in $variants[$v].Keys) { Set-Item "Env:$k" $variants[$v][$k] }
   $tag = "$Prefix-$n-$v"
   if ($Sample) { & .\tools\dev\run-dota.ps1 -Api Vulkan -Icd $icd -Tag $tag -Sample | Select-Object -First 2 }
   else { & .\tools\dev\run-dota.ps1 -Api Vulkan -Icd $icd -Tag $tag | Select-Object -First 2 }
   $c = Get-Content "build\dota\$tag.console.log" -ErrorAction SilentlyContinue | Select-String '\[Demo\] \d+ frames .* ([\d.]+) fps' | Select-Object -Last 1
   $fps = if ($c) { [double]::Parse([regex]::Match($c.Line, '([\d.]+) fps').Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) } else { 0 }
   $exp = Get-Content "build\dota\$tag.stdout.txt" -ErrorAction SilentlyContinue | Select-String 'EXPERIMENT' | Select-Object -First 1
   $rows += [pscustomobject]@{ run = $n; variant = $v; fps = $fps; note = $(if ($exp) { $exp.Line.Trim() } else { '' }) }
   "  -> $v $fps"
}
foreach ($name in $names) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
$rows | Format-Table -AutoSize | Out-String -Width 200
$rows | Group-Object variant | ForEach-Object { '{0,-6} {1}' -f $_.Name, (($_.Group | ForEach-Object { $_.fps }) -join ' / ') }
Get-ChildItem C:\ProgramData\Microsoft\Windows\WER\ReportQueue, C:\Windows\LiveKernelReports -Recurse -Filter *141* -ErrorAction SilentlyContinue |
   Sort-Object CreationTime | Select-Object -Last 1 CreationTime
"$Prefix-DONE " + (Get-Date)
