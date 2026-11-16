param(
    [string]$CaptureDirectory = (Join-Path $PSScriptRoot '..\artifacts\compute-bootstrap\compute-capture'),
    [string]$XmlPath = (Join-Path $PSScriptRoot '..\reference\gen8.xml')
)
$ErrorActionPreference = 'Stop'
$capture = Get-Content -LiteralPath (Join-Path $CaptureDirectory 'capture.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$definitions = @{}
function Import-GenXml([string]$Path) {
    [xml]$xml = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    foreach ($import in $xml.genxml.import) {
        if ($import.name) { Import-GenXml (Join-Path (Split-Path $Path) $import.name) }
        foreach ($exclude in $import.exclude) { $definitions.Remove([string]$exclude.name) }
    }
    foreach ($instruction in $xml.genxml.instruction) {
        foreach($field in $instruction.field) {
            if($field.bits) { $parts=([string]$field.bits).Split(':'); $field.SetAttribute('start',([int]$field.dword*32+[int]$parts[-1]).ToString()); $field.SetAttribute('end',([int]$field.dword*32+[int]$parts[0]).ToString()) }
        }
        $definitions[[string]$instruction.name] = $instruction
    }
}
Import-GenXml $XmlPath
$instructions = @()
foreach ($instruction in $definitions.Values) {
    if ($instruction.engine -and $instruction.engine -notmatch 'render') { continue }
    [uint64]$mask = 0; [uint64]$value = 0
    foreach ($field in $instruction.field) {
        if ($null -eq $field.default -or [int]$field.start -ge 32) { continue }
        if ($field.name -notmatch 'Command (Type|Subtype|SubType|Opcode|Sub Opcode)|MI Command Opcode|^Pipeline$|Media Command|^SubOpcode$|^Opcode$') { continue }
        $width = [int]$field.end - [int]$field.start + 1
        [uint64]$part = [uint64]([Math]::Pow(2, $width) - 1)
        $mask = $mask -bor ($part -shl [int]$field.start)
        $value = $value -bor ([uint64]$field.default -shl [int]$field.start)
    }
    if ($mask -ne 0) { $instructions += [pscustomobject]@{Mask=$mask; Value=$value; Xml=$instruction} }
}
$memory = @{}
function Get-GpuBytes([uint64]$Address) {
    $Address = $Address -band 0xffffffffffffL
    foreach ($allocation in $capture.allocations) {
        $base = [Convert]::ToUInt64($allocation.address,16)
        if ($Address -ge $base -and $Address -lt ($base + $allocation.bytes)) {
            if (-not $memory.ContainsKey($allocation.address)) { $memory[$allocation.address] = [IO.File]::ReadAllBytes((Join-Path $CaptureDirectory $allocation.memoryFile)) }
            return [pscustomobject]@{Data=$memory[$allocation.address]; Offset=[int]($Address-$base); Base=$base}
        }
    }
    throw ('GPU address absent from capture: {0:X}' -f $Address)
}
$packets = New-Object 'System.Collections.Generic.List[object]'
$visited = @{}
function Decode-Batch([uint64]$Address,[int]$Depth) {
    $Address = $Address -band 0xffffffffffffL
    if ($Depth -gt 8 -or $visited.ContainsKey($Address)) { return }
    $visited[$Address] = $true
    $view = Get-GpuBytes $Address
    $offset = $view.Offset
    for($count=0;$count -lt 8192 -and $offset+4 -le $view.Data.Length;$count++) {
        [uint64]$header = [BitConverter]::ToUInt32($view.Data,$offset)
        if($header -eq 0) { $offset+=4; continue }
        $matches = @($instructions | Where-Object { ($header -band $_.Mask) -eq $_.Value })
        $instruction = if($matches.Count) { $matches[0].Xml } else { $null }
        $name = if($instruction) { [string]$instruction.name } else { 'UNKNOWN' }
        $lengthField = @($instruction.field | Where-Object name -EQ 'DWord Length') | Select-Object -First 1
        if ($lengthField) {
            $width = [int]$lengthField.end - [int]$lengthField.start + 1
            $length = [int](($header -shr [int]$lengthField.start) -band [uint64]([Math]::Pow(2,$width)-1)) + [int]$instruction.bias
        } elseif($instruction.length) { $length=[int]$instruction.length }
        else { $length = if($header -lt 0x20000000L) { 1 } else { [int]($header -band 255)+2 } }
        if($length -lt 1 -or $offset+$length*4 -gt $view.Data.Length) { throw "Invalid packet length at $offset : $name" }
        $words=@(); for($i=0;$i -lt $length;$i++) { $words += [BitConverter]::ToUInt32($view.Data,$offset+$i*4).ToString('X8') }
        if($name -ne 'MI_NOOP') {
            $packets.Add([pscustomobject]@{Address=($view.Base+[uint64]$offset).ToString('X'); Name=$name; Length=$length; Words=$words; Depth=$Depth})
        }
        if($name -eq 'MI_BATCH_BUFFER_END') { break }
        if($name -eq 'MI_BATCH_BUFFER_START') {
            [uint64]$target=[BitConverter]::ToUInt64($view.Data,$offset+4) -band 0xfffffffffffcL
            Decode-Batch $target ($Depth+1)
            if(($header -band 0x400000) -eq 0) { break }
        }
        $offset += $length*4
    }
}
Decode-Batch ([Convert]::ToUInt64($capture.batchAddress,16)) 0
$packets | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'decoded-packets.json') -Encoding UTF8
$packets | Select-Object Address,Name,Length,Depth,@{Name='Payload';Expression={($_.Words | Select-Object -Skip 1 -First 5) -join ' '}} | Format-Table -AutoSize
