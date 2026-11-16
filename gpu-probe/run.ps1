param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts'),
    [switch]$BuildOnly,
    [switch]$Trace,
    [switch]$Raw,
    [switch]$NativeOnly,
    [switch]$CaptureCompute,
    [switch]$NativeCompute,
    [int]$ElementCount = 16381,
    [string]$CaptureDirectory = (Join-Path $PSScriptRoot 'artifacts\compute-bootstrap\compute-capture'),
    [string]$BootstrapDirectory = (Join-Path $PSScriptRoot 'artifacts\native-bootstrap\bootstrap')
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "C# compiler not found: $compiler" }
$bin = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Path $bin, $OutputDirectory -Force | Out-Null
$executable = Join-Path $bin 'IrisProbe.exe'
$sources = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -Filter '*.cs' | Sort-Object Name | ForEach-Object FullName)
& $compiler /nologo /target:exe /platform:x64 /optimize+ /warn:4 /warnaserror+ "/out:$executable" /reference:System.Drawing.dll /reference:System.Web.Extensions.dll $sources
if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
$sourceHashes = @($sources | ForEach-Object {
    [ordered]@{ file = [IO.Path]::GetFileName($_); sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{
    builtUtc = [DateTime]::UtcNow.ToString('o')
    executable = $executable
    executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    sources = $sourceHashes
    shaderSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'shaders\probe.hlsl') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $resolvedOutput 'build.json') -Encoding UTF8
if ($BuildOnly) { Write-Output "Built: $executable"; return }
try {
    if ($NativeCompute) { & $executable $resolvedOutput '--native-compute' ([IO.Path]::GetFullPath($CaptureDirectory)) ([IO.Path]::GetFullPath($BootstrapDirectory)) $ElementCount }
    elseif ($NativeOnly) { & $executable $resolvedOutput '--native' ([IO.Path]::GetFullPath($BootstrapDirectory)) }
    elseif ($CaptureCompute) { & $executable $resolvedOutput '--capture-compute' }
    elseif ($Raw) { & $executable $resolvedOutput '--raw' }
    elseif ($Trace) { & $executable $resolvedOutput '--trace' }
    else { & $executable $resolvedOutput }
}
catch {
    [ordered]@{ attemptedUtc = [DateTime]::UtcNow.ToString('o'); executableSha256 = $manifest.executableSha256; launchFailed = $true; error = $_.Exception.Message } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resolvedOutput 'launch-status.json') -Encoding UTF8
    throw
}
if ($LASTEXITCODE -ne 0) { throw "GPU probe failed: $LASTEXITCODE; see $OutputDirectory\report.json" }
