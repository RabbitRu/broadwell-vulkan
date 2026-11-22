# CTS groups of the extensions Vulkan 1.3 makes core, and the 1.3 feature and
# limit checks. Each pattern: dEQP case pattern, every Nth case. Results in
# C:\VulkanCTS-local\results\vk13-*, summary in build\cts-vk13.json.
$ErrorActionPreference = 'Continue'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$groups = @(
    @('api.info.vulkan1p3*', 1), @('api.info.get_physical_device_properties2.*', 1), @('api.info.feature*', 1),
    @('api.object_management.private_data.*', 1), @('api.tooling_info.*', 1),
    @('subgroups.size_control.*', 1), @('pipeline.monolithic.creation_cache_control.*', 1),
    @('pipeline.monolithic.creation_feedback.*', 1), @('glsl.demote.*', 1),
    @('spirv_assembly.instruction.terminate_invocation.*', 1), @('spirv_assembly.instruction.compute.*dot*', 3),
    @('compute.pipeline.zero_initialize_workgroup_memory.*', 1), @('robustness.image_robustness.*', 10),
    @('binding_model.*inline_uniform*', 2))
$out = @()
foreach ($g in $groups) {
    $name = 'vk13-' + (($g[0] -replace '[^A-Za-z0-9]+', '-').Trim('-'))
    $summary = Join-Path 'C:\VulkanCTS-local\results' ($name + '\summary.json')
    if (Test-Path $summary) { Remove-Item $summary -Confirm:$false }
    try { & (Join-Path $PSScriptRoot 'run-cts.ps1') -Case ('dEQP-VK.' + $g[0]) -Every $g[1] -Name $name -CaseSeconds 60 *> (Join-Path $root "build\cts-$name.log") } catch { }
    if (Test-Path $summary) {
        $j = Get-Content $summary -Raw | ConvertFrom-Json
        $out += [ordered]@{group = $g[0]; every = $g[1]; counts = $j.counts; notPassing = $j.notPassing}
    } else { $out += [ordered]@{group = $g[0]; failed = $true} }
}
$out | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'build\cts-vk13.json') -Encoding UTF8
foreach ($o in $out) { "$($o.group) every $($o.every): " + ($o.counts | ConvertTo-Json -Compress) }
