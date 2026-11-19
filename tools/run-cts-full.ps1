<#
Full CTS mustpass run (vk-default, ~2.9 million cases, 8-10 hours), unattended
and resumable.

The mustpass lists of the CTS source tree are cut into chunks of -ChunkCases
cases; each chunk is one run-cts.ps1 run (-Jobs processes, then a serial rerun
of what did not pass). A chunk that already has a summary for this ICD binary
is skipped, so the same command continues an interrupted run. After a chunk its
logs are deleted (only summaries are kept: the .qpa logs of the whole run would
take tens of gigabytes), except the logs of chunks with cases that do not pass.

  powershell -File tools\run-cts-full.ps1 [-Jobs 2] [-ChunkCases 20000] [-Only 'api*'] [-MaxHours 0]

Progress: build\cts-full\progress.log (one line per chunk). Result:
build\cts-full\summary.json (counts, every case that does not pass, cases that
failed only in parallel, excluded cases) - rewritten after every chunk.

-Exclude: cases that are not run and are reported as excluded. Default: the two
cases of docs\cts-expected-failures.md whose result is known and costly - one
runs longer than the Windows TDR limit and loses the device (the GPU is reset),
one takes over 20 minutes.

The machine is kept awake while the script runs (SetThreadExecutionState: a
request of this process, not a system setting); the display may still turn off.
#>
param([string]$Root = 'C:\VulkanCTS-local', [int]$Jobs = 2, [int]$ChunkCases = 20000, [string]$Only = '*', [double]$MaxHours = 0,
      [int]$CaseSeconds = 120, [int]$MaxCrashes = 200, [switch]$List,
      [string[]]$Exclude = @('dEQP-VK.tessellation.misc_draw.tess_factor_barrier_bug',
                             'dEQP-VK.wsi.win32.swapchain.simulate_oom.min_image_count'))
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$mustpass = Join-Path $Root 'src\external\vulkancts\mustpass\main\vk-default'
if (-not (Test-Path -LiteralPath $mustpass)) { throw "mustpass lists not found: $mustpass" }
$outDir = Join-Path $taskRoot 'build\cts-full'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$progress = Join-Path $outDir 'progress.log'
$icd = Join-Path $taskRoot 'build\icd\vulkan_hasvk_wddm.dll'
$icdHash = (Get-FileHash -LiteralPath $icd -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not ('CtsFullAwake' -as [type])) { Add-Type @'
using System.Runtime.InteropServices;
public static class CtsFullAwake { [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags); }
'@ }
[CtsFullAwake]::SetThreadExecutionState([uint32]'0x80000001') | Out-Null   # ES_CONTINUOUS | ES_SYSTEM_REQUIRED

function Write-Progress-Line([string]$text) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $text"
    Add-Content -LiteralPath $progress -Value $line -Encoding UTF8
    Write-Output $line
}

# Chunks: mustpass files in name order, each cut into pieces of -ChunkCases.
$excluded = [Collections.Generic.HashSet[string]]::new([string[]]$Exclude)
$skipped = [Collections.Generic.List[string]]::new()
$chunks = [Collections.Generic.List[object]]::new()
$totalCases = 0
foreach ($file in Get-ChildItem -LiteralPath $mustpass -Recurse -File -Filter *.txt | Sort-Object FullName) {
    $group = $file.FullName.Substring($mustpass.Length + 1) -replace '\.txt$', '' -replace '[\\/]', '.'
    if ($group -notlike $Only) { continue }
    $cases = [Collections.Generic.List[string]]::new()
    foreach ($line in [IO.File]::ReadLines($file.FullName)) {
        if (-not $line.StartsWith('dEQP-VK.', [StringComparison]::Ordinal)) { continue }
        if ($excluded.Contains($line)) { $skipped.Add($line) } else { $cases.Add($line) }
    }
    $totalCases += $cases.Count
    for ($i = 0; $i -lt $cases.Count; $i += $ChunkCases) {
        $count = [Math]::Min($ChunkCases, $cases.Count - $i)
        $chunks.Add([pscustomobject]@{ name = ('full-{0}-{1:d3}' -f $group, [int]($i / $ChunkCases)); group = $group; cases = $cases.GetRange($i, $count) })
    }
}
if (-not $chunks.Count) { throw "no mustpass groups match $Only" }
if ($List) {   # the plan only: groups, cases, chunks
    $chunks | Group-Object group | ForEach-Object { '{0,9} cases {1,4} chunks  {2}' -f ($_.Group | ForEach-Object { $_.cases.Count } | Measure-Object -Sum).Sum, $_.Count, $_.Name }
    "$totalCases cases in $($chunks.Count) chunks, excluded $($skipped.Count)"
    return
}
Write-Progress-Line "start: $totalCases cases in $($chunks.Count) chunks, ICD $($icdHash.Substring(0, 12)), jobs $Jobs, excluded $($skipped.Count)"

$good = @('Pass', 'NotSupported', 'QualityWarning', 'CompatibilityWarning')
$counts = @{}; $notPassing = [Collections.Generic.List[object]]::new(); $flaky = [Collections.Generic.List[object]]::new()
$executed = 0; $done = 0; $ranSeconds = 0.0; $ranCases = 0
$watch = [Diagnostics.Stopwatch]::StartNew()
$started = [DateTime]::UtcNow.ToString('o')

function Save-Summary([bool]$finished) {
    $ordered = [ordered]@{}; foreach ($key in ($counts.Keys | Sort-Object)) { $ordered[$key] = $counts[$key] }
    $summary = [ordered]@{ schema = 1; cts = (Get-Content -LiteralPath (Join-Path $Root 'build.json') -Raw | ConvertFrom-Json).tag;
        mustpass = 'vk-default'; only = $Only; icdSha256 = $icdHash; startedUtc = $started; updatedUtc = [DateTime]::UtcNow.ToString('o');
        finished = $finished; chunks = $chunks.Count; chunksDone = $done; cases = $totalCases; executed = $executed; jobs = $Jobs;
        counts = $ordered; notPassing = $notPassing; flakyUnderParallel = $flaky; excluded = $skipped }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outDir 'summary.json') -Encoding UTF8
}

foreach ($chunk in $chunks) {
    $resultDir = Join-Path $Root ('results\' + $chunk.name)
    $summaryPath = Join-Path $resultDir 'summary.json'
    $summary = $null
    if (Test-Path -LiteralPath $summaryPath) {
        $previous = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
        if ($previous.icdSha256 -eq $icdHash -and $previous.selected -eq $chunk.cases.Count -and $previous.executed -ge $chunk.cases.Count) { $summary = $previous }
    }
    $resumed = [bool]$summary
    if (-not $summary) {
        if ($MaxHours -gt 0 -and $watch.Elapsed.TotalHours -ge $MaxHours) { Write-Progress-Line "stopped by -MaxHours before $($chunk.name)"; break }
        New-Item -ItemType Directory -Path $resultDir -Force | Out-Null
        $caseFile = Join-Path $resultDir 'chunk.txt'
        [IO.File]::WriteAllLines($caseFile, [string[]]$chunk.cases, [Text.ASCIIEncoding]::new())
        $chunkWatch = [Diagnostics.Stopwatch]::StartNew()
        try {
            & (Join-Path $PSScriptRoot 'run-cts.ps1') -CaseFile $caseFile -Name $chunk.name -Root $Root -Jobs $Jobs -CaseSeconds $CaseSeconds -MaxCrashes $MaxCrashes | Out-Null
        } catch {
            Write-Progress-Line "$($chunk.name): runner error: $($_.Exception.Message)"
        }
        $ranSeconds += $chunkWatch.Elapsed.TotalSeconds
        if (Test-Path -LiteralPath $summaryPath) { $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json }
        if ($summary) { $ranCases += [int]$summary.executed }
    }
    if (-not $summary) {
        $counts['RunnerError'] = [int]$counts['RunnerError'] + $chunk.cases.Count
        $notPassing.Add([ordered]@{ case = "$($chunk.name) ($($chunk.cases.Count) cases)"; status = 'RunnerError' })
    } else {
        foreach ($p in $summary.counts.PSObject.Properties) { $counts[$p.Name] = [int]$counts[$p.Name] + [int]$p.Value }
        foreach ($bad in @($summary.notPassing)) { if ($bad) { $notPassing.Add([ordered]@{ case = $bad.case; status = $bad.status }) } }
        foreach ($f in @($summary.flakyUnderParallel)) { if ($f) { $flaky.Add([ordered]@{ case = $f.case; parallel = $f.parallel; serial = $f.serial }) } }
        $executed += [int]$summary.executed
        $missing = $chunk.cases.Count - [int]$summary.executed
        if ($missing -gt 0) { $counts['NotRun'] = [int]$counts['NotRun'] + $missing; $notPassing.Add([ordered]@{ case = "$($chunk.name): $missing cases not run"; status = 'NotRun' }) }
    }
    ++$done
    # Logs: keep them only where something did not pass.
    $keep = -not $summary -or @($summary.notPassing).Count -gt 0
    foreach ($suffix in @('') + (0..($Jobs - 1) | ForEach-Object { "-j$_" }) + @('-serial')) {
        $dir = Join-Path $Root ('results\' + $chunk.name + $suffix)
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        $patterns = if ($keep -and $suffix -eq '-serial') { @('cases-*.txt') } elseif ($suffix -eq '') { @('cases-*.txt', 'results-*.qpa', 'shard-*.txt', 'chunk.txt') }
                    else { @('cases-*.txt', 'results-*.qpa', 'run-*.stdout.log') }
        foreach ($pattern in $patterns) { Get-ChildItem -LiteralPath $dir -File -Filter $pattern -ErrorAction SilentlyContinue | Remove-Item -Confirm:$false -ErrorAction SilentlyContinue }
    }
    Save-Summary $false
    $bad = if ($summary) { @($summary.notPassing).Count } else { $chunk.cases.Count }
    $rate = if ($ranSeconds -gt 0) { $ranCases / $ranSeconds } else { 0 }
    $left = $totalCases - $executed
    $eta = if ($rate -gt 0) { '{0:n1} h left' -f ($left / $rate / 3600) } else { '' }
    Write-Progress-Line ("{0}/{1} {2}: {3} cases{4}, not passing {5}; total {6}/{7}, not passing {8}, {9:n0} cases/s, {10}" -f
        $done, $chunks.Count, $chunk.name, $chunk.cases.Count, $(if ($resumed) { ' (done before)' } else { '' }), $bad, $executed, $totalCases, $notPassing.Count, $rate, $eta)
}
$finished = $done -eq $chunks.Count
Save-Summary $finished
[CtsFullAwake]::SetThreadExecutionState([uint32]'0x80000000') | Out-Null
$text = ($counts.Keys | Sort-Object | ForEach-Object { "$_ $($counts[$_])" }) -join ', '
Write-Progress-Line ("{0}: {1} of {2} cases in {3:n1} h; {4}; not passing {5}, flaky under parallel {6}" -f
    $(if ($finished) { 'finished' } else { 'stopped' }), $executed, $totalCases, $watch.Elapsed.TotalHours, $text, $notPassing.Count, $flaky.Count)
