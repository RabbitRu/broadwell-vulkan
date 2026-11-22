$ErrorActionPreference='Continue'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
# Extension-specific CTS groups for the second extension batch (sampled).
$groups=@(@('renderpasses.renderpass2',200),@('renderpasses.dynamic_rendering',100000),@('draw.renderpass.multi_draw',20),@('pipeline.monolithic.extended_dynamic_state',60),
  @('ssbo.layout',120),@('ubo',120),@('spirv_assembly.instruction.compute.8bit_storage',10),@('spirv_assembly.instruction.graphics.16bit_storage',40),
  @('spirv_assembly.instruction.compute.float_controls',20),@('memory_model',300),@('api.copy_and_blit.copy_commands2',40),@('draw.renderpass.indirect_draw',4),
  @('api.image_clearing',150),@('renderpasses.renderpass2.depth_stencil_resolve',20),@('pipeline.monolithic.depth_stencil',30),@('image.format_list',5))
$out=@()
foreach ($g in $groups) {
  $name='ext-'+$g[0]
  $summary=Join-Path 'C:\VulkanCTS-local\results' ($name+'\summary.json'); if (Test-Path $summary) { Remove-Item $summary -Confirm:$false }
  try { & (Join-Path $PSScriptRoot 'run-cts.ps1') -Case ('dEQP-VK.'+$g[0]+'.*') -Every $g[1] -Name $name -CaseSeconds 60 *> (Join-Path $root "build\cts-$name.log") } catch { }
  if (Test-Path $summary) { $j=Get-Content $summary -Raw | ConvertFrom-Json; $out+=[ordered]@{group=$g[0]; matched=$j.matched; executed=$j.executed; counts=$j.counts} }
  else { $out+=[ordered]@{group=$g[0]; failed=$true} }
  $out | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'build\cts-ext.json') -Encoding UTF8
}
'DONE'