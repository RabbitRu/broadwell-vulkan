# Runs one command of the workflow and, when it fails, repeats the end of its
# output as an error annotation: annotations are readable without signing in,
# job logs are not.
param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][string]$Command)
$log = Join-Path $env:RUNNER_TEMP "$Name.log"
try {
    Invoke-Expression $Command | Tee-Object -FilePath $log
    if ($LASTEXITCODE) { throw "$Name exited with code $LASTEXITCODE" }
} catch {
    $tail = @(Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue) + @("$_", $_.ScriptStackTrace)
    Write-Host ("::error title=$Name::" + (($tail | ForEach-Object { "$_" -replace '%', '%25' -replace "`r", '' -replace "`n", '%0A' }) -join '%0A'))
    exit 1
}
exit 0
