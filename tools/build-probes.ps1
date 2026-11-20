<#
Builds the performance probes and profiling tools of native\tests into
build\probes (they are not part of test-all):

  Vulkan, through the system loader and VK_DRIVER_FILES:
    replay_pass_probe  replay of a captured render pass (HASVK_WDDM_CAPTURE_PASS);
                       REPLAY_RECORD_ONLY=<n> times the driver's recording CPU
    replay_probe       replay of one captured draw (HASVK_WDDM_CAPTURE_DIR)
    depth_fill_probe, flops_probe, sample_probe, vs_fetch_probe, bandwidth_probe
  The same loads on Intel's own D3D11 driver:
    d3d11_depth_fill_probe, d3d11_flops_probe, d3d11_sample_probe,
    d3d11_vs_fetch_probe, d3d11_replay_probe
  Process tools:
    rip_sampler        sampling profiler with stacks (<pid> <seconds> [symbols] [module])
    stack_dump         stacks of every thread of a hung process
    d3d11-proxy\d3d11.dll  timing proxy of Intel's D3D11 driver for run-talos.ps1 -D3D11Proxy
  Shaders (SPIR-V for the Vulkan probes), with glslangValidator from the local
  Vulkan SDK (-Glslang to point elsewhere).

  .\tools\build-probes.ps1
#>
param([string]$Glslang = 'C:\VulkanSDK-local\Bin\glslangValidator.exe')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'import-toolchain.ps1')
# Compilers write file names and warnings to stderr; Windows PowerShell turns
# that into errors under 'Stop'. Every tool's exit code is checked instead.
$ErrorActionPreference = 'Continue'
$out = Join-Path $taskRoot 'build\probes'
New-Item -ItemType Directory -Force $out | Out-Null
$tests = Join-Path $taskRoot 'native\tests'
$vulkanInclude = Join-Path $taskRoot 'third_party\mesa-26.2.7\include'
$failed = @()
function Build([string]$name, [string[]]$libs, [string[]]$extra = @()) {
   $source = Join-Path $tests "$name.cpp"
   $obj = Join-Path $out "$name.obj"
   $arguments = @('/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', '/utf-8', '/W3', '/D_WIN32_WINNT=0x0A00', "/I$vulkanInclude", "/I$tests") + $extra +
                @($source, "/Fo$obj", "/Fe$(Join-Path $out "$name.exe")", '/link') + $libs
   $log = & cl.exe @arguments 2>&1
   if ($LASTEXITCODE) { $script:failed += $name; $log | Select-String 'error' | Select-Object -First 5 | ForEach-Object { "  ${name}: $($_.Line.Trim())" } }
   else { "  built $name.exe" }
   Remove-Item -LiteralPath $obj -ErrorAction SilentlyContinue
}
foreach ($name in 'replay_pass_probe', 'replay_probe', 'depth_fill_probe', 'flops_probe', 'sample_probe', 'vs_fetch_probe', 'bandwidth_probe') {
   Build $name @('user32.lib')
}
foreach ($name in 'd3d11_depth_fill_probe', 'd3d11_flops_probe', 'd3d11_sample_probe', 'd3d11_vs_fetch_probe', 'd3d11_replay_probe') {
   Build $name @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
}
foreach ($name in 'rip_sampler', 'stack_dump') {
   Build $name @('dbghelp.lib', 'psapi.lib', 'winmm.lib')
}
# The D3D11 timing proxy (run-talos.ps1 -D3D11Proxy build\probes\d3d11-proxy\d3d11.dll).
$proxyOut = Join-Path $out 'd3d11-proxy'
New-Item -ItemType Directory -Force $proxyOut | Out-Null
$clang = Join-Path $taskToolchain.llvmBin 'clang.exe'
$proxyLog = & $clang -shared -O2 -D_CRT_SECURE_NO_WARNINGS -o (Join-Path $proxyOut 'd3d11.dll') (Join-Path $tests 'd3d11_proxy\proxy.c') (Join-Path $tests 'd3d11_proxy\stubs.c') -ldxguid -luser32 2>&1
if ($LASTEXITCODE) { $failed += 'd3d11 proxy'; $proxyLog | Select-String 'error' | Select-Object -First 5 | ForEach-Object { "  d3d11 proxy: $($_.Line.Trim())" } }
else { "  built d3d11-proxy\d3d11.dll" }
if (Test-Path -LiteralPath $Glslang) {
   $shaders = [ordered]@{ 'flops.comp' = 'flops.spv'; 'sample.comp' = 'sample.spv'; 'depth_fill.vert' = 'depth_fill.vert.spv';
      'depth_fill_discard.frag' = 'depth_fill_discard.frag.spv'; 'depth_fill_empty.frag' = 'depth_fill_empty.frag.spv';
      'vs_fetch.vert' = 'vs_fetch.vert.spv'; 'replay_pos.vert' = 'replay_pos.spv'; 'replay_t4.vert' = 'replay_t4.spv';
      'replay_trivial.vert' = 'replay_trivial.spv'; 'replay_v4.vert' = 'replay_v4.spv' }
   foreach ($shader in $shaders.Keys) {
      & $Glslang -V -o (Join-Path $out $shaders[$shader]) (Join-Path $tests $shader) *> $null
      if ($LASTEXITCODE) { $failed += $shader; "  $shader failed" } else { "  compiled $($shaders[$shader])" }
   }
} else { "  glslangValidator not found ($Glslang): SPIR-V shaders skipped" }
if ($failed) { throw "Failed: $($failed -join ', ')" }
"Probes in $out"
