param([string]$Case, [string]$CaseFile, [string]$Name, [string]$Root = 'C:\VulkanCTS-local', [int]$CaseSeconds = 120, [int]$MaxCases = 0,
    [int]$MaxCrashes = 50, [int]$Every = 1, [int]$Jobs = 1, [string]$ShaderCache, [switch]$RecordAll)
# -CaseFile <file>: run exactly the cases listed there (one name per line), e.g. to recheck earlier failures.
# -Every N keeps every Nth matching case: a quick, evenly spread sample of a large group.
# -Jobs N: N deqp-vk processes at once (cases dealt round-robin, each process
#   with its own shader cache). Every case that does not pass is then run again
#   alone in one process: summary.json counts the serial result, and lists
#   cases that failed only in parallel under flakyUnderParallel.
# -ShaderCache <file>: deqp-vk shader cache file (default: its own shadercache.bin).
# -RecordAll: also write every case's status to summary.json (results).
# Runs Khronos deqp-vk cases matching -Case (dEQP wildcard, e.g. dEQP-VK.api.smoke.*)
# against the local ICD and writes build\cts\<Name>\summary.json.
# Unattended: no window (--deqp-visibility=hidden), crash dialogs suppressed, a
# case that crashes or exceeds -CaseSeconds is recorded and the run resumes
# with the next case. VK_DRIVER_FILES is set for the child process only.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$taskPackage=Join-Path $taskRoot 'build\icd'
$taskBuild=Get-Content -LiteralPath (Join-Path $Root 'build.json') -Raw | ConvertFrom-Json
$taskExe=$taskBuild.executable
$taskWork=Split-Path $taskExe -Parent
if (-not $Case -and -not $CaseFile) { throw 'Pass -Case or -CaseFile' }
if ($CaseFile) { if (-not $Name) { $Name=[IO.Path]::GetFileNameWithoutExtension($CaseFile) }; $Case='file:'+$CaseFile }
if (-not $Name) { $Name=($Case -replace '[^A-Za-z0-9._-]', '_').Trim('_', '.') }
# deqp-vk takes file names as narrow strings, so logs live under the ASCII CTS
# root; only the summary is copied into the project.
$taskOut=Join-Path $Root ('results\'+$Name)
$taskSummaryDirectory=Join-Path $taskRoot 'build\cts'
New-Item -ItemType Directory -Path $taskSummaryDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $taskOut -Force | Out-Null
if (-not ('CtsErrorMode' -as [type])) { Add-Type @'
using System.Runtime.InteropServices;
public static class CtsErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode); }
'@ }
[CtsErrorMode]::SetErrorMode(0x8003) | Out-Null   # no critical-error, GP-fault or open-file boxes; inherited by deqp-vk
function Start-Deqp([string[]]$Arguments, [int]$Seconds, [string]$LogName) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$taskExe; $start.WorkingDirectory=$taskWork; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    # deqp's own command-line parser does not strip quotes; all paths here are space-free.
    if ($ShaderCache) { $Arguments += '--deqp-shadercache-filename='+$ShaderCache }
    $start.Arguments=$Arguments -join ' '
    [IO.File]::WriteAllText((Join-Path $taskOut ($LogName+'.cmdline.txt')), $start.Arguments, [Text.UTF8Encoding]::new($false))
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.EnvironmentVariables['VK_DRIVER_FILES']=Join-Path $taskPackage 'intel_hasvk_wddm_icd.json'
    $start.EnvironmentVariables.Remove('VK_ICD_FILENAMES'); $start.EnvironmentVariables.Remove('VK_ADD_DRIVER_FILES')
    $start.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
    $start.EnvironmentVariables['PATH']=$taskPackage+';'+$env:PATH
    $process=[Diagnostics.Process]::Start($start)
    $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=-not $process.WaitForExit($Seconds*1000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit() }
    [IO.File]::WriteAllText((Join-Path $taskOut ($LogName+'.stdout.log')), $stdout.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $taskOut ($LogName+'.stderr.log')), $stderr.Result, [Text.UTF8Encoding]::new($false))
    $result=[ordered]@{exitCode=$process.ExitCode; timedOut=$timedOut}
    $process.Dispose(); $result
}
# Full case list of this deqp-vk build for this ICD, cached per ICD binary.
$taskIcdHash=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
$taskListCache=Join-Path $Root 'results\caselist.txt'   # depends on the CTS build, not on the ICD
if (-not (Test-Path -LiteralPath $taskListCache)) {
    $generated=Join-Path $taskWork 'dEQP-VK-cases.txt'
    if (Test-Path -LiteralPath $generated) { Remove-Item -LiteralPath $generated -Confirm:$false }
    # Build the argument first: inside the array literal '+(Join-Path ...)' became a separate element.
    $logArgument='--deqp-log-filename='+(Join-Path $taskOut 'caselist.qpa')
    $list=Start-Deqp @('--deqp-runmode=txt-caselist', '--deqp-visibility=hidden', $logArgument) 1800 'caselist'
    if ($list.exitCode -or $list.timedOut -or -not (Test-Path -LiteralPath $generated)) { throw "deqp-vk could not enumerate its cases (exit $($list.exitCode)); see $taskOut\caselist.stderr.log" }
    Move-Item -LiteralPath $generated -Destination $taskListCache -Force
}
$taskPattern='^'+[regex]::Escape($Case).Replace('\*','.*')+'$'
# The list is about a gigabyte ("TEST: <name>" per line): stream it and test the literal prefix first.
$taskPrefix='TEST: '+($Case -split '\*')[0]; $taskRegex=[regex]::new($taskPattern)
$taskSelected=[Collections.Generic.List[string]]::new()
if (-not $CaseFile) {   # a case file needs no scan of the gigabyte list (parallel jobs each have one)
    foreach ($line in [IO.File]::ReadLines($taskListCache)) {
        # Not $name: variable names are case-insensitive, and that is the -Name parameter.
        if ($line.StartsWith($taskPrefix, [StringComparison]::Ordinal)) { $caseName=$line.Substring(6); if ($taskRegex.IsMatch($caseName)) { $taskSelected.Add($caseName) } }
    }
}
$taskMatched=$taskSelected.Count
if ($Every -gt 1) { $taskSampled=[Collections.Generic.List[string]]::new(); for ($i=0; $i -lt $taskSelected.Count; $i+=$Every) { $taskSampled.Add($taskSelected[$i]) }; $taskSelected=$taskSampled }
$taskCases=$taskSelected.ToArray()
# ReadAllLines, not Get-Content: Get-Content strings carry PSPath and other note
# properties, and ConvertTo-Json then writes each case name as an object.
if ($CaseFile) { $taskCases=@([IO.File]::ReadAllLines($CaseFile) | Where-Object { $_ -match '^dEQP-' }); $taskMatched=$taskCases.Count }
if (-not $taskCases.Count) { throw "No CTS cases match $Case" }
if ($MaxCases -gt 0 -and $taskCases.Count -gt $MaxCases) { $taskCases=$taskCases[0..($MaxCases-1)] }
if ($Jobs -gt 1 -and $taskCases.Count -gt 1) {
    # Parallel pass: one child run-cts.ps1 (serial) per shard, then a serial
    # rerun of everything that did not pass.
    $taskWatch=[Diagnostics.Stopwatch]::StartNew()
    $shards=@(); for ($k=0; $k -lt $Jobs; ++$k) { $shards += ,([Collections.Generic.List[string]]::new()) }
    for ($i=0; $i -lt $taskCases.Count; ++$i) { $shards[$i % $Jobs].Add($taskCases[$i]) }
    $mainCache=Join-Path $taskWork 'shadercache.bin'
    $children=@()
    for ($k=0; $k -lt $Jobs; ++$k) {
        if (-not $shards[$k].Count) { continue }
        $stale=Join-Path $Root ('results\'+$Name+'-j'+$k+'\summary.json')
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Confirm:$false }
        $shardFile=Join-Path $taskOut ("shard-$k.txt")
        [IO.File]::WriteAllLines($shardFile, [string[]]$shards[$k], [Text.ASCIIEncoding]::new())
        $cache=Join-Path $taskWork ("shadercache-j$k.bin")
        if ((Test-Path -LiteralPath $mainCache) -and -not (Test-Path -LiteralPath $cache)) { Copy-Item -LiteralPath $mainCache -Destination $cache }
        $child=[Diagnostics.ProcessStartInfo]::new('powershell.exe')
        $child.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -CaseFile "'+$shardFile+'" -Name "'+$Name+'-j'+$k+
            '" -Root "'+$Root+'" -CaseSeconds '+$CaseSeconds+' -MaxCrashes '+$MaxCrashes+' -ShaderCache "'+$cache+'" -RecordAll'
        $child.UseShellExecute=$false; $child.CreateNoWindow=$true; $child.RedirectStandardOutput=$true; $child.RedirectStandardError=$true
        $process=[Diagnostics.Process]::Start($child)
        $children += [ordered]@{process=$process; name=($Name+'-j'+$k); out=$process.StandardOutput.ReadToEndAsync(); err=$process.StandardError.ReadToEndAsync()}
    }
    foreach ($c in $children) { $c.process.WaitForExit() }
    $parallelSeconds=[math]::Round($taskWatch.Elapsed.TotalSeconds,1)
    $statuses=@{}
    foreach ($c in $children) {
        $s=Join-Path $Root ('results\'+$c.name+'\summary.json')
        if (-not (Test-Path -LiteralPath $s)) { throw "Parallel job $($c.name) wrote no summary: $($c.err.Result)" }
        $j=Get-Content -LiteralPath $s -Raw | ConvertFrom-Json
        foreach ($p in $j.results.PSObject.Properties) { $statuses[$p.Name]=$p.Value }
    }
    $good=@('Pass','NotSupported','QualityWarning','CompatibilityWarning')
    $rerun=@($taskCases | Where-Object { $statuses.ContainsKey($_) -and $statuses[$_] -notin $good })
    $missing=@($taskCases | Where-Object { -not $statuses.ContainsKey($_) })   # a job gave up (too many crashes)
    $rerun += $missing
    $flaky=@(); $serialSeconds=0
    if ($rerun.Count) {
        $rerunFile=Join-Path $taskOut 'serial-rerun.txt'
        [IO.File]::WriteAllLines($rerunFile, [string[]]$rerun, [Text.ASCIIEncoding]::new())
        $serialWatch=[Diagnostics.Stopwatch]::StartNew()
        & $PSCommandPath -CaseFile $rerunFile -Name ($Name+'-serial') -Root $Root -CaseSeconds $CaseSeconds -MaxCrashes $MaxCrashes -RecordAll | Out-Null
        $serialSeconds=[math]::Round($serialWatch.Elapsed.TotalSeconds,1)
        $j=Get-Content -LiteralPath (Join-Path $Root ('results\'+$Name+'-serial\summary.json')) -Raw | ConvertFrom-Json
        foreach ($p in $j.results.PSObject.Properties) {
            $before=$(if ($statuses.ContainsKey($p.Name)) { $statuses[$p.Name] } else { 'NotRun' })
            if ($p.Value -in $good -and $before -notin $good) { $flaky += [ordered]@{case=$p.Name; parallel=$before; serial=$p.Value} }
            $statuses[$p.Name]=$p.Value
        }
    }
    $taskCounts=[ordered]@{}; foreach ($group in ($statuses.GetEnumerator() | Group-Object Value | Sort-Object Name)) { $taskCounts[$group.Name]=$group.Count }
    $taskBad=@($taskCases | Where-Object { $statuses.ContainsKey($_) -and $statuses[$_] -notin $good } | ForEach-Object { [ordered]@{case=$_; status=$statuses[$_]} })
    $taskSummary=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); cts=$taskBuild.tag; pattern=$Case;
        icdSha256=(Get-FileHash -LiteralPath (Join-Path $taskPackage 'vulkan_hasvk_wddm.dll') -Algorithm SHA256).Hash.ToLowerInvariant();
        matched=$taskMatched; every=$Every; selected=$taskCases.Count; executed=$statuses.Count; jobs=$Jobs;
        parallelSeconds=$parallelSeconds; serialRerunCases=$rerun.Count; serialRerunSeconds=$serialSeconds; seconds=[math]::Round($taskWatch.Elapsed.TotalSeconds,1);
        counts=$taskCounts; notPassing=$taskBad; flakyUnderParallel=$flaky}
    if ($RecordAll) { $all=[ordered]@{}; foreach ($c in $taskCases) { if ($statuses.ContainsKey($c)) { $all[$c]=$statuses[$c] } }; $taskSummary.results=$all }
    $taskSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOut 'summary.json') -Encoding UTF8
    Copy-Item -LiteralPath (Join-Path $taskOut 'summary.json') -Destination (Join-Path $taskSummaryDirectory ($Name+'.json')) -Force
    [ordered]@{pattern=$Case; selected=$taskCases.Count; executed=$statuses.Count; jobs=$Jobs; counts=$taskCounts; flakyUnderParallel=$flaky.Count;
        seconds=$taskSummary.seconds; summary=(Join-Path $taskOut 'summary.json')} | ConvertTo-Json -Depth 4
    return
}
$taskResults=[ordered]@{}
$taskRun=0; $taskCrashes=0
$taskWatch=[Diagnostics.Stopwatch]::StartNew()
# deqp-vk runs a case list in the order of its own test tree, not of the file,
# and exits non-zero whenever a case failed. What is left after a run is
# therefore "the cases without a result", never "the cases after the last one
# seen" (that reran most of a list over and over once it held a failure).
$remaining=[Collections.Generic.List[string]]::new([string[]]$taskCases)
while ($remaining.Count -and $taskCrashes -le $MaxCrashes) {
    $listFile=Join-Path $taskOut ("cases-{0:d3}.txt" -f $taskRun); $qpa=Join-Path $taskOut ("results-{0:d3}.qpa" -f $taskRun)
    [IO.File]::WriteAllLines($listFile, [string[]]$remaining, [Text.ASCIIEncoding]::new())
    $run=Start-Deqp @('--deqp-caselist-file='+$listFile, '--deqp-log-filename='+$qpa, '--deqp-visibility=hidden', '--deqp-log-images=disable',
        '--deqp-log-shader-sources=disable', '--deqp-log-flush=enable') ([Math]::Max($CaseSeconds, [Math]::Min(36000, $CaseSeconds*$remaining.Count))) ("run-{0:d3}" -f $taskRun)
    $current=$null; $finished=0; $sessionEnded=$false
    if (Test-Path -LiteralPath $qpa) {
        foreach ($line in [IO.File]::ReadLines($qpa)) {
            if ($line.StartsWith('#beginTestCaseResult ')) { $current=$line.Substring(21).Trim() }
            elseif ($current -and $line -match '<Result StatusCode="([^"]+)"') { $taskResults[$current]=$Matches[1]; $current=$null; ++$finished }
            elseif ($line.StartsWith('#endSession')) { $sessionEnded=$true }
        }
    }
    if ($current) {     # the process died or hung inside this case
        $taskResults[$current]=$(if ($run.timedOut) { 'Timeout' } else { 'Crash' }); ++$taskCrashes
    } elseif ($finished -eq 0 -and -not $sessionEnded) {   # nothing ran at all: charge the first case
        $taskResults[$remaining[0]]=$(if ($run.timedOut) { 'Timeout' } else { 'Crash' }); ++$taskCrashes
    }
    ++$taskRun
    # A session that ended without running anything: the rest are names this
    # deqp-vk does not have. (A session also "ends" when a ResourceError aborts
    # the run, so an ended session alone does not mean the list is done.)
    if ($sessionEnded -and -not $current -and $finished -eq 0) { break }
    $left=[Collections.Generic.List[string]]::new()
    foreach ($case in $remaining) { if (-not $taskResults.Contains($case)) { $left.Add($case) } }
    $remaining=$left
}
$taskGroups=$taskResults.GetEnumerator() | Group-Object Value
$taskCounts=[ordered]@{}; foreach ($group in ($taskGroups | Sort-Object Name)) { $taskCounts[$group.Name]=$group.Count }
$taskBad=@($taskResults.GetEnumerator() | Where-Object { $_.Value -notin @('Pass','NotSupported','QualityWarning','CompatibilityWarning') } |
    ForEach-Object { [ordered]@{case=$_.Key; status=$_.Value} })
$taskSummary=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); cts=$taskBuild.tag; pattern=$Case; icdSha256=$taskIcdHash;
    matched=$taskMatched; every=$Every; selected=$taskCases.Count; executed=$taskResults.Count; processRuns=$taskRun; seconds=[math]::Round($taskWatch.Elapsed.TotalSeconds,1);
    counts=$taskCounts; notPassing=$taskBad}
if ($RecordAll) { $taskSummary.results=$taskResults }
$taskSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOut 'summary.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $taskOut 'summary.json') -Destination (Join-Path $taskSummaryDirectory ($Name+'.json')) -Force
[ordered]@{pattern=$Case; selected=$taskCases.Count; executed=$taskResults.Count; counts=$taskCounts; seconds=$taskSummary.seconds; summary=(Join-Path $taskOut 'summary.json')} | ConvertTo-Json -Depth 4
