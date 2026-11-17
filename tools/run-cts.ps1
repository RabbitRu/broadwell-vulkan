param([string]$Case, [string]$CaseFile, [string]$Name, [string]$Root = 'C:\VulkanCTS-local', [int]$CaseSeconds = 120, [int]$MaxCases = 0,
    [int]$MaxCrashes = 50, [int]$Every = 1)
# -CaseFile <file>: run exactly the cases listed there (one name per line), e.g. to recheck earlier failures.
# -Every N keeps every Nth matching case: a quick, evenly spread sample of a large group.
# Runs Khronos deqp-vk cases matching -Case (dEQP wildcard, e.g. dEQP-VK.api.smoke.*)
# against the local ICD and writes build\cts\<Name>\summary.json.
# Unattended: no window (--deqp-visibility=hidden), crash dialogs suppressed, a
# case that crashes or exceeds -CaseSeconds is recorded and the run resumes
# with the next case. VK_DRIVER_FILES is set for the child process only.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
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
Add-Type @'
using System.Runtime.InteropServices;
public static class CtsErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode); }
'@
[CtsErrorMode]::SetErrorMode(0x8003) | Out-Null   # no critical-error, GP-fault or open-file boxes; inherited by deqp-vk
function Start-Deqp([string[]]$Arguments, [int]$Seconds, [string]$LogName) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$taskExe; $start.WorkingDirectory=$taskWork; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    # deqp's own command-line parser does not strip quotes; all paths here are space-free.
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
foreach ($line in [IO.File]::ReadLines($taskListCache)) {
    if ($line.StartsWith($taskPrefix, [StringComparison]::Ordinal)) { $name=$line.Substring(6); if ($taskRegex.IsMatch($name)) { $taskSelected.Add($name) } }
}
$taskMatched=$taskSelected.Count
if ($Every -gt 1) { $taskSampled=[Collections.Generic.List[string]]::new(); for ($i=0; $i -lt $taskSelected.Count; $i+=$Every) { $taskSampled.Add($taskSelected[$i]) }; $taskSelected=$taskSampled }
$taskCases=$taskSelected.ToArray()
if ($CaseFile) { $taskCases=@(Get-Content -LiteralPath $CaseFile | Where-Object { $_ -match '^dEQP-' }); $taskMatched=$taskCases.Count }
if (-not $taskCases.Count) { throw "No CTS cases match $Case" }
if ($MaxCases -gt 0 -and $taskCases.Count -gt $MaxCases) { $taskCases=$taskCases[0..($MaxCases-1)] }
$taskResults=[ordered]@{}
$taskNext=0; $taskRun=0; $taskCrashes=0
$taskWatch=[Diagnostics.Stopwatch]::StartNew()
while ($taskNext -lt $taskCases.Count -and $taskCrashes -le $MaxCrashes) {
    $remaining=$taskCases[$taskNext..($taskCases.Count-1)]
    $listFile=Join-Path $taskOut ("cases-{0:d3}.txt" -f $taskRun); $qpa=Join-Path $taskOut ("results-{0:d3}.qpa" -f $taskRun)
    [IO.File]::WriteAllLines($listFile, [string[]]$remaining, [Text.ASCIIEncoding]::new())
    $run=Start-Deqp @('--deqp-caselist-file='+$listFile, '--deqp-log-filename='+$qpa, '--deqp-visibility=hidden', '--deqp-log-images=disable',
        '--deqp-log-shader-sources=disable', '--deqp-log-flush=enable') ([Math]::Max($CaseSeconds, [Math]::Min(36000, $CaseSeconds*$remaining.Count))) ("run-{0:d3}" -f $taskRun)
    $current=$null; $finished=0; $last=$null
    if (Test-Path -LiteralPath $qpa) {
        foreach ($line in [IO.File]::ReadLines($qpa)) {
            if ($line.StartsWith('#beginTestCaseResult ')) { $current=$line.Substring(21).Trim() }
            elseif ($current -and $line -match '<Result StatusCode="([^"]+)"') { $taskResults[$current]=$Matches[1]; $last=$current; $current=$null; ++$finished }
        }
    }
    if ($current) {     # the process died or hung inside this case
        $taskResults[$current]=$(if ($run.timedOut) { 'Timeout' } else { 'Crash' }); ++$taskCrashes
        $taskNext=[Array]::IndexOf($taskCases, $current)+1
    } elseif ($finished -eq 0 -and ($run.exitCode -or $run.timedOut)) {
        $taskResults[$remaining[0]]=$(if ($run.timedOut) { 'Timeout' } else { 'Crash' }); ++$taskCrashes; ++$taskNext
    } elseif ($run.exitCode -or $run.timedOut) {   # died between cases (e.g. after a DeviceLost result)
        $taskNext=[Array]::IndexOf($taskCases, $last)+1
    } else { $taskNext=$taskCases.Count }
    ++$taskRun
}
$taskGroups=$taskResults.GetEnumerator() | Group-Object Value
$taskCounts=[ordered]@{}; foreach ($group in ($taskGroups | Sort-Object Name)) { $taskCounts[$group.Name]=$group.Count }
$taskBad=@($taskResults.GetEnumerator() | Where-Object { $_.Value -notin @('Pass','NotSupported','QualityWarning','CompatibilityWarning') } |
    ForEach-Object { [ordered]@{case=$_.Key; status=$_.Value} })
$taskSummary=[ordered]@{schema=1; testedUtc=[DateTime]::UtcNow.ToString('o'); cts=$taskBuild.tag; pattern=$Case; icdSha256=$taskIcdHash;
    matched=$taskMatched; every=$Every; selected=$taskCases.Count; executed=$taskResults.Count; processRuns=$taskRun; seconds=[math]::Round($taskWatch.Elapsed.TotalSeconds,1);
    counts=$taskCounts; notPassing=$taskBad}
$taskSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOut 'summary.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $taskOut 'summary.json') -Destination (Join-Path $taskSummaryDirectory ($Name+'.json')) -Force
[ordered]@{pattern=$Case; selected=$taskCases.Count; executed=$taskResults.Count; counts=$taskCounts; seconds=$taskSummary.seconds; summary=(Join-Path $taskOut 'summary.json')} | ConvertTo-Json -Depth 4
