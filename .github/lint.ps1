# Syntax check of every PowerShell and Python script in the repository: most
# of them need the GPU and are never run by the workflow.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$failed = 0
$scripts = @(& git -C $root ls-files '*.ps1')
foreach ($script in $scripts) {
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $root $script), [ref]$null, [ref]$errors)
    foreach ($problem in $errors) { "${script}:$($problem.Extent.StartLineNumber): $($problem.Message)"; $failed++ }
}
$python = @(& git -C $root ls-files '*.py')
foreach ($script in $python) {
    & python -c "import ast, sys; ast.parse(open(sys.argv[1], encoding='utf-8').read(), sys.argv[1])" (Join-Path $root $script)
    if ($LASTEXITCODE) { "${script}: does not parse"; $failed++ }
}
# Scripts refer to each other by path: every referenced tools\... file must exist.
foreach ($script in $scripts + $python) {
    $text = Get-Content -LiteralPath (Join-Path $root $script) -Raw -Encoding UTF8
    foreach ($match in [regex]::Matches($text, 'tools[\\/](?:dev[\\/])?[a-z0-9-]+\.(?:ps1|py|json|txt)')) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $match.Value))) { "${script}: refers to missing $($match.Value)"; $failed++ }
    }
}
if ($failed) { throw "$failed problem(s) in $($scripts.Count) PowerShell and $($python.Count) Python scripts" }
"$($scripts.Count) PowerShell and $($python.Count) Python scripts parse"
