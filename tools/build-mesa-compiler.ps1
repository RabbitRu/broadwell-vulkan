# -Asserts: keep Mesa's assert() checks (NDEBUG off) for debugging; the default
# build defines NDEBUG like Mesa release builds (asserts cost driver CPU time in
# every vkCmd* call: Talos' render thread spends ~20 % in the driver).
# -DebugInfo: emit debug information (/Z7, PDB next to the DLL) so rip_sampler,
# debug-icd.ps1 and llvm-symbolizer show function names and lines. Off by
# default: it does not change the generated code, but without /DEBUG the
# linker drops unreferenced functions and folds identical ones (/OPT:REF,ICF).
# -Pgo generate: instrumented build (clang -fprofile-instr-generate); every
# process using the ICD writes a .profraw at exit to LLVM_PROFILE_FILE. Merge
# them with llvm-profdata merge -o <file>.profdata (tools\dev\pgo-profile.ps1 does
# both). -Pgo <file.profdata>: optimize with that profile. Default: the
# profile in tools\pgo\hasvk.profdata if present, -Pgo none for neither.
# -Jobs: parallel compile jobs (2 keeps the laptop usable and cool).
param([switch]$ConfigureOnly, [switch]$Graphics = $true, [switch]$Asserts, [switch]$DebugInfo, [string]$Pgo = '', [int]$Jobs = 2)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'import-toolchain.ps1') -UseClang
$taskDependencies=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath (Join-Path $taskRoot '.tools\downloads\mesa-26.2.7.tar.xz') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskDependencies.mesa.sha256) {
    throw 'Pinned Mesa source archive checksum mismatch'
}
& $taskToolchain.python (Join-Path $taskRoot 'mesa\apply-compiler-port.py')
if ($LASTEXITCODE) { throw 'Mesa compiler patch failed' }
$taskMesa = Join-Path $taskRoot 'third_party\mesa-26.2.7'
$taskMesaBuild = Join-Path $taskRoot 'build\mesa-compiler-clang'
$taskArgs = @('setup', $taskMesaBuild, $taskMesa, '-Dplatforms=windows', '-Dvulkan-drivers=intel_hasvk',
    '-Dhasvk-wddm-compiler-only=true', '-Dgallium-drivers=', '-Dopengl=false', '-Degl=disabled',
    '-Dglx=disabled', '-Dgbm=disabled', '-Dllvm=disabled', '-Dzstd=disabled', '-Dxmlconfig=disabled',
    '-Dbuild-tests=false', '-Dprecomp-compiler=auto')
if (Test-Path -LiteralPath (Join-Path $taskMesaBuild 'build.ninja')) { $taskArgs += '--reconfigure' }
& $taskToolchain.python -m mesonbuild.mesonmain @taskArgs
if ($LASTEXITCODE) { throw "Mesa compiler configuration failed; see $taskMesaBuild\meson-logs\meson-log.txt" }
# The driver only runs next to a Broadwell GPU, so the CPU is always Broadwell
# (AVX2, BMI2, FMA): let clang use them. Setting the arguments here also drops
# anything left from experiments (PGO flags) in an existing build directory.
$taskCpuArgs = @('/clang:-march=broadwell')
$taskLinkArgs = @()
# Paths in compiler arguments: the 8.3 form, because meson's configuration
# checks break on non-ASCII paths (the project lives under a Cyrillic user name).
function Get-ShortPath([string]$path) { (New-Object -ComObject Scripting.FileSystemObject).GetFile($path).ShortPath }
if (-not $Pgo) {
    $taskDefaultProfile = Join-Path $PSScriptRoot 'pgo\hasvk.profdata'
    if (Test-Path -LiteralPath $taskDefaultProfile) { $Pgo = $taskDefaultProfile }
}
if ($Pgo -eq 'generate') {
    $taskCpuArgs += '/clang:-fprofile-instr-generate'
    $taskProfileRuntime = Join-Path $taskToolchain.llvmBin '..\lib\clang\23\lib\windows\clang_rt.profile-x86_64.lib'
    $taskLinkArgs += Get-ShortPath (Resolve-Path -LiteralPath $taskProfileRuntime).Path
} elseif ($Pgo -and $Pgo -ne 'none') {
    # Functions changed since the profile was taken are compiled without it.
    $taskCpuArgs += @("/clang:-fprofile-instr-use=$(Get-ShortPath (Resolve-Path -LiteralPath $Pgo).Path)",
                      '/clang:-Wno-profile-instr-unprofiled', '/clang:-Wno-profile-instr-out-of-date', '/clang:-Wno-profile-instr-missing')
}
"PGO: $(if ($Pgo) { $Pgo } else { 'none' })"
# meson reads the array as a Python literal: backslashes would be escapes.
function ConvertTo-MesonArray([string[]]$items) { '[' + (($items | ForEach-Object { "'" + ($_ -replace '\\', '/') + "'" }) -join ',') + ']' }
& $taskToolchain.python -m mesonbuild.mesonmain configure $taskMesaBuild "-Dhasvk-wddm-graphics=$($Graphics.IsPresent.ToString().ToLowerInvariant())" "-Db_ndebug=$(if ($Asserts) { 'false' } else { 'true' })" `
    "-Dc_args=$(ConvertTo-MesonArray $taskCpuArgs)" "-Dcpp_args=$(ConvertTo-MesonArray $taskCpuArgs)" `
    "-Dc_link_args=$(ConvertTo-MesonArray $taskLinkArgs)" "-Dcpp_link_args=$(ConvertTo-MesonArray $taskLinkArgs)" `
    "-Ddebug=$($DebugInfo.IsPresent.ToString().ToLowerInvariant())" '-Doptimization=2' '-Db_vscrt=md'
if ($LASTEXITCODE) { throw 'Mesa graphics option configuration failed' }
# A PDB left by a build with -DebugInfo would no longer match the DLL.
if (-not $DebugInfo) { Remove-Item -LiteralPath (Join-Path $taskMesaBuild 'src\intel\wddm_compiler\vulkan_hasvk_wddm.pdb') -ErrorAction SilentlyContinue }
if (-not $ConfigureOnly) {
    & $taskToolchain.python -m mesonbuild.mesonmain compile -C $taskMesaBuild -j $Jobs intel_compiler_elk intel_dev IrisElkCompile IrisImageProbe IrisNirSerializeTest vulkan_runtime vulkan_lite_runtime vulkan_instance vulkan_util vulkan_wsi vulkan_hasvk_wddm
    if ($LASTEXITCODE) { throw 'Mesa compiler build failed' }
    $taskProducts=@('src\intel\wddm_compiler\IrisElkCompile.exe','src\intel\wddm_compiler\IrisImageProbe.exe','src\intel\wddm_compiler\IrisNirSerializeTest.exe',
        'src\intel\wddm_compiler\libhasvk_wddm.a','src\intel\wddm_compiler\libwddm_blorp.a',
        'src\vulkan\runtime\libvulkan_runtime.a','src\vulkan\runtime\libvulkan_instance.a','src\vulkan\wsi\libvulkan_wsi.a',
        'src\intel\wddm_compiler\vulkan_hasvk_wddm.dll')
    $taskBuildManifest=[ordered]@{builtUtc=[DateTime]::UtcNow.ToString('o'); mesa='26.2.7'; toolchain=$taskToolchain;
        sourceArchiveSha256=$taskDependencies.mesa.sha256;
        patchSha256=(Get-FileHash -LiteralPath (Join-Path $taskRoot 'mesa\patches\compiler-port.patch') -Algorithm SHA256).Hash.ToLowerInvariant();
        products=@($taskProducts | ForEach-Object {[ordered]@{file=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $taskMesaBuild $_) -Algorithm SHA256).Hash.ToLowerInvariant()}});
        vulkanRuntimeAndWin32WsiCompiled=$true; vulkanControlPlaneIcdBuilt=$true; vulkanComputeIcdBuilt=$true;
        graphicsModulesBuilt=$Graphics.IsPresent; completeVulkanIcdImplemented=$false; win32PresentationValidated=$false}
    $taskBuildManifest | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $taskMesaBuild 'build.json') -Encoding UTF8
}
