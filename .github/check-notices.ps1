# Regenerates THIRD-PARTY-NOTICES.md from the finished build and fails when the
# committed file differs.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
& (Join-Path $root '.tools\python\python.exe') (Join-Path $root 'tools\gen-third-party-notices.py')
if ($LASTEXITCODE) { throw 'gen-third-party-notices.py failed' }
$difference = @(& git -C $root diff --unified=0 -- THIRD-PARTY-NOTICES.md)
if ($difference) {
    $difference | Select-Object -First 30
    throw 'THIRD-PARTY-NOTICES.md is out of date: run tools\gen-third-party-notices.py and commit the result'
}
'THIRD-PARTY-NOTICES.md is current'
