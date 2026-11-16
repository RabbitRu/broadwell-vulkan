param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\reference'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$baseUri = 'https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/main/src/intel/genxml/'
$pending = New-Object 'System.Collections.Generic.Queue[string]'
$pending.Enqueue('gen80.xml')
$seen = @{}
$sources = @()
while ($pending.Count -gt 0) {
    $name = $pending.Dequeue()
    if ($seen.ContainsKey($name)) { continue }
    if ($name -notmatch '^gen[0-9]+\.xml$') { throw "Unexpected genxml import: $name" }
    $seen[$name] = $true
    $uri = $baseUri + $name
    $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -ErrorAction Stop
    [xml]$xml = $response.Content
    if ($null -eq $xml.genxml) { throw "The response is not Mesa genxml: $name" }
    $localName = if ($name -eq 'gen80.xml') { 'gen8.xml' } else { $name }
    $path = Join-Path $OutputDirectory $localName
    [IO.File]::WriteAllText($path, $response.Content)
    $sources += [ordered]@{ file = $localName; url = $uri; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    foreach ($import in $xml.genxml.import) { if ($import.name) { $pending.Enqueue([string]$import.name) } }
}
$sources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'genxml-sources.json') -Encoding UTF8
Write-Output ('Fetched Mesa command definitions: ' + $sources.Count)
