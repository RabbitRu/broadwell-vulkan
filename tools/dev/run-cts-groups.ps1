# Targeted CTS: a list of "pattern:every" groups under one name prefix.
#   .\tools\dev\run-cts-groups.ps1 -Prefix ext-a -Groups 'subgroups.vote.*:1','glsl.shader_clock.*:1'
# Results in C:\VulkanCTS-local\results\<prefix>-*, summary in build\cts-<prefix>.json.
# -Jobs N: N deqp-vk processes per group, failures rerun serially (run-cts.ps1).
param([Parameter(Mandatory)][string]$Prefix, [Parameter(Mandatory)][string[]]$Groups, [int]$Jobs = 1)
$ErrorActionPreference = 'Continue'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = @()
# "powershell -File" passes 'a:1','b:2' as one string: split on commas too.
foreach ($entry in ($Groups | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
    $pattern, $every = $entry -split ':'
    if (-not $every) { $every = 1 }
    $name = $Prefix + '-' + (($pattern -replace '[^A-Za-z0-9]+', '-').Trim('-'))
    $summary = Join-Path 'C:\VulkanCTS-local\results' ($name + '\summary.json')
    if (Test-Path $summary) { Remove-Item $summary -Confirm:$false }
    try { & (Join-Path $PSScriptRoot 'run-cts.ps1') -Case ('dEQP-VK.' + $pattern) -Every ([int]$every) -Name $name -CaseSeconds 60 -Jobs $Jobs *> (Join-Path $root "build\cts-$name.log") } catch { }
    if (Test-Path $summary) {
        $j = Get-Content $summary -Raw | ConvertFrom-Json
        $out += [ordered]@{group = $pattern; every = [int]$every; counts = $j.counts; notPassing = $j.notPassing; flakyUnderParallel = $j.flakyUnderParallel}
    } else { $out += [ordered]@{group = $pattern; every = [int]$every; failed = $true} }
}
$out | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root "build\cts-$Prefix.json") -Encoding UTF8
foreach ($o in $out) { "$($o.group) every $($o.every): " + ($(if ($o.failed) { 'no summary' } else { $o.counts | ConvertTo-Json -Compress })) }
