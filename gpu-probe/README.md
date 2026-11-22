# gpu-probe: the C# prototype of direct GPU access through WDDM

The experiment the project started from. It proved that a process can drive the
Intel Iris Graphics 6100 (PCI `8086:162B`, Intel driver `20.19.15.5171`)
through the stock kernel driver alone: it builds Gen8 commands itself, submits
them with `D3DKMTSubmitCommand` and reads back what the GPU wrote, with neither
D3D12 nor `igd12umd64.dll` loaded (checked against the list of loaded modules).

The Vulkan driver later reimplemented all of this in C++ (`native/src`). The
prototype is kept because it is the tool that captures Intel's private
allocation descriptors and because `tools\dev\test-all.ps1` still uses its
captures as controls.

## What it does

- Its own WDDM device, context and paging queue; allocations through
  `D3DKMTCreateAllocation2`, CPU mappings through `D3DKMTLock2`, GPU mappings
  through `D3DKMTMapGpuVirtualAddress`.
- `-NativeOnly`: a 72-byte Gen8 batch of four `MI_STORE_DWORD_IMM` and
  `MI_BATCH_BUFFER_END`, real execution, a wait through a CPU event, and a
  check of all 1024 result words (4 written values and 1020 guard words).
- `-NativeCompute`: a 328-byte compute batch (`PIPELINE_SELECT`,
  `STATE_BASE_ADDRESS`, `MEDIA_VFE_STATE`, `MEDIA_INTERFACE_DESCRIPTOR_LOAD`,
  `GPGPU_WALKER`) that runs a shader over 1 to 16,381 elements. The 896 bytes
  of shader machine code come from Intel's compiler in a bootstrap run; the
  prototype allocates fresh memory and relocates base addresses, buffer
  pointers and CURBE parameters. The CPU computes the same function and checks
  every word.
- `-Raw` and `-Trace`: a tracer that hooks nine GPU system calls of
  `win32u.dll` inside its own process only, to record what Intel's user-mode
  driver passes to the kernel. This is the source of the descriptors in
  [docs/intel-interface.md](../docs/intel-interface.md). System files and other
  processes are not touched.
- A baseline through the stock D3D12 driver: a 65,536-byte copy, a `cs_5_1`
  compute shader and a 512×384 triangle compared with a CPU calculation.

It depends on three blocks of Intel parameters captured from the stock
user-mode driver (two 1096-byte allocation descriptors and the 80-byte
submission header) and, for compute, on the captured shader code and state.
They are pinned by SHA-256, checked against the driver version, and are not
distributed in this repository.

## Running

Requires Windows x64, this exact GPU and .NET Framework 4.x; it is compiled by
the `csc.exe` that ships with Windows. From the repository root:

```powershell
# Full bootstrap: D3D12 baseline, trace, direct commands, own allocator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -Raw -OutputDirectory .\gpu-probe\artifacts\native-bootstrap

# Standalone: WDDM only, the saved Intel parameters and our commands.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -NativeOnly -OutputDirectory .\gpu-probe\artifacts\native-only

# Capture the compute shader and decode the Gen8 packets.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\tools\fetch-genxml.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -CaptureCompute -OutputDirectory .\gpu-probe\artifacts\compute-bootstrap
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\tools\decode-compute.ps1

# Standalone compute with fresh data and relocated addresses.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -NativeCompute -ElementCount 16381 -OutputDirectory .\gpu-probe\artifacts\native-compute
```

`fetch-genxml.ps1` downloads Mesa's Gen8 command definitions, which
`decode-compute.ps1` uses to decode packets. Execution is limited to the one
PCI ID; there is no software fallback. GPU and paging waits time out after 15
seconds. Each run writes a `report.json` and the batches and readbacks it used
into its output directory under `gpu-probe\artifacts` (not tracked).

## Sources

- `src/NativeCompute.cs` — the compute batch, state relocation, result checks.
- `src/ComputeCapture.cs` — capture of buffers, machine code and compute
  arguments.
- `src/NativeMemory.cs` — the WDDM allocator, mappings, residency.
- `src/NativeOnly.cs` — the run without D3D12, adapter and module checks.
- `src/Wddm.cs` — context, direct submission, completion wait.
- `src/KernelTrace.cs` — the system call tracer.
- `src/Native.cs` — COM ABI of D3D12/DXGI and structure size checks.
- `src/Program.cs` — the D3D12 baseline and the test driver.
- `src/Imports.cs` — module list and import reading.
- `shaders/probe.hlsl` — shaders of the baseline.

## References

- Microsoft WDDM headers: https://github.com/microsoft/win32metadata/tree/main/generation/WinSDK/RecompiledIdlHeaders/shared
- Intel's WDDM implementation: https://github.com/intel/compute-runtime/tree/master/shared/source/os_interface/windows/wddm
- Intel command encodings in i915: https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_gpu_commands.h
- Mesa HasVK: https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/intel/vulkan_hasvk
