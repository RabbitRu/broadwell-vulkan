$ErrorActionPreference='Continue'
$root=Split-Path $PSScriptRoot -Parent
$groups=@(@('api',1500),@('pipeline.monolithic',3000),@('renderpasses.renderpass1',200),@('draw.renderpass',150),@('compute.pipeline',300),@('image',1000),
  @('binding_model',1000),@('texture',120),@('glsl',160),@('ubo',70),@('ssbo',90),@('query_pool',130),@('memory',50),@('synchronization',450),
  @('rasterization',110),@('dynamic_state',6),@('spirv_assembly',420),@('clipping',3),@('fragment_operations',2),@('geometry',2),@('tessellation',8),@('wsi.win32',250))
$out=@()
foreach ($g in $groups) {
  $name='sample-'+$g[0]
  if (Test-Path (Join-Path 'C:\VulkanCTS-local\results' ($name+'\summary.json'))) { Remove-Item (Join-Path 'C:\VulkanCTS-local\results' ($name+'\summary.json')) -Confirm:$false }
  try { & (Join-Path $PSScriptRoot 'run-cts.ps1') -Case ('dEQP-VK.'+$g[0]+'.*') -Every $g[1] -Name $name -CaseSeconds 60 *> (Join-Path $root "build\cts-$name.log") } catch { $_ | Out-File (Join-Path $root "build\cts-$name.log") -Append }
  $s=Join-Path 'C:\VulkanCTS-local\results' ($name+'\summary.json')
  if (Test-Path $s) { $j=Get-Content $s -Raw | ConvertFrom-Json; $out+=[ordered]@{group=$g[0]; matched=$j.matched; selected=$j.selected; executed=$j.executed; seconds=$j.seconds; counts=$j.counts} }
  else { $out+=[ordered]@{group=$g[0]; failed=$true} }
  $out | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'build\cts-sample.json') -Encoding UTF8
}
'DONE'
