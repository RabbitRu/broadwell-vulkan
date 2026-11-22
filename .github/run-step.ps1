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
# A child process with stderr merged by cmd.exe: the messages of failing tools
# (a Python traceback, for example) go to stderr, and merging it inside
# PowerShell would turn every such line into an error record.
# The command goes through a script file: with -EncodedCommand PowerShell
# writes its errors to a redirected stderr as CLIXML.
$script = Join-Path $env:RUNNER_TEMP "step-$Name.ps1"
[IO.File]::WriteAllText($script, "`$ProgressPreference = 'SilentlyContinue'`r`n$Command`r`nexit `$LASTEXITCODE`r`n", [Text.UTF8Encoding]::new($true))
& cmd.exe /d /c "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$script`" 2>&1" | Tee-Object -FilePath $log
$code = $LASTEXITCODE
[IO.File]::Delete($script)
if ($code) { Write-ErrorAnnotation $Name (@(Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue) + "$Name exited with code $code") }
[IO.File]::Delete($log)
exit $code
