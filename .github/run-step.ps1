# Runs one command of the workflow and, when it fails, repeats the end of its
# output as an error annotation: annotations are readable without signing in,
# job logs are not. -Report: do that for the logs left behind by steps that
# were killed (a step timeout) and so could not report themselves.
param([string]$Name, [string]$Command, [switch]$Report)
function Write-ErrorAnnotation([string]$title, [object[]]$lines) {
    Write-Host ("::error title=$title::" + (($lines | ForEach-Object { "$_" -replace '%', '%25' -replace "`r", '' -replace "`n", '%0A' }) -join '%0A'))
}
if ($Report) {
    foreach ($left in Get-ChildItem -LiteralPath $env:RUNNER_TEMP -Filter 'step-*.log') {
        Write-ErrorAnnotation "$($left.BaseName.Substring(5)) was interrupted" (@(Get-Content -LiteralPath $left.FullName -Tail 40) + 'the step did not finish (timeout?)')
    }
    exit 0
}
$log = Join-Path $env:RUNNER_TEMP "step-$Name.log"
try {
    Invoke-Expression $Command | Tee-Object -FilePath $log
    if ($LASTEXITCODE) { throw "$Name exited with code $LASTEXITCODE" }
} catch {
    Write-ErrorAnnotation $Name (@(Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue) + @("$_", $_.ScriptStackTrace))
    [IO.File]::Delete($log)
    exit 1
}
[IO.File]::Delete($log)
exit 0
