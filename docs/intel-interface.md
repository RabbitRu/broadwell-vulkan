# The undocumented Intel interface the port depends on

Everything the ICD passes to Intel's kernel driver `20.19.15.5171` beyond the
documented D3DKMT structures. Only the calls and their wrappers are documented
(`d3dkmthk.h`); Intel does not describe the private data inside them. Every
value below was taken from traces of Intel's own drivers of the same package
and verified by a probe on the hardware.

## Protection against another driver

`hasvk::wddm::Device` creates a device only for the adapter `8086:162b` whose
user-mode driver version is exactly `20.19.15.5171` (`supported_driver`,
`KMTQAITYPE_UMD_DRIVER_VERSION`). Otherwise `enumerate()` in
`anv_wddm_device.c` returns no physical device: the Vulkan loader skips the
ICD, the application sees its other GPUs, and nothing is sent to the kernel
with unverified data.

## What is supplied

| Data | In the code | Source | Verified by |
|---|---|---|---|
| Linear allocation descriptor, 1096 bytes | `linear_metadata()`, `native/src/intel_2019_profile.cpp` | byte for byte from state allocations of Intel's UMD (`gpu-probe`, `KernelTrace.cs`) | `tools\dev\test-native.ps1`, every ICD probe |
| Window surface descriptor, 1096 bytes | `window_surface_metadata()` | the surface `dwmapi` creates through D3D11 for the OpenGL driver (`tools\dev\trace-gl-present.ps1`) | `tools\dev\test-present-redirect.ps1`, `vkcube` |
| Window surface runtime data, 104 bytes | `window_surface_runtime_data()` | the same trace (the D3D runtime's format, `DXGI_FORMAT_B8G8R8A8_UNORM`) | the same |
| Submission header, 80 bytes | `command_header_8192()` | a trace of Intel UMD submissions; field 20 is verified only for 8192-byte command buffers | every submission |
| Context private data, 32 bytes | `Device::Device` (`presentable_context`) | a trace of the OpenGL driver: a pointer to a `0x2cdc`-byte buffer, its size, the flag `0x100`; the kernel does not write to the buffer | `test-present-redirect.ps1` (without it DWM shows black), regression tests, CTS |
| Redirected present token | `anv_wddm_present.c` | a trace: `(0x80000000 \| HWND) << 32 \| handle`; equals `updateId` from `dwmapi!DwmpDxGetWindowSharedSurface` | `test-present-redirect.ps1 -Surface dwmapi` |
| Context `ClientHint` | `Device::Device` | `D3DKMT_CLIENTHINT_VULKAN` (documented) | — |

### Allocation descriptor fields (what is known)

Byte offsets, 32-bit values. The meaning comes from comparing traces; this is
not a description of Intel's GMM format.

| Offset | Linear | Window surface | Meaning (a guess) |
|---|---|---|---|
| 0x00 | 3 | 3 | descriptor version or class |
| 0x08 | 6 | 2 | resource type: buffer / 2D |
| 0x0c | 0x155 | 0x17 | format |
| 0x10 | 8 | 0x20 | bits per element |
| 0x14, 0x18, 0x1c | flags | flags | usage, tiling (the kernel changes 0x18–0x1f on open) |
| 0x28 | size (8 bytes) | width | |
| 0x30 | — | height | |
| 0xc0, 0xd0 | — | filled by the kernel: pitch and size | |
| 0x3ac | 0x14330000 | 0x14330000 | signature |

The kernel chooses the window surface layout itself: Y tiling, a pitch of
`align(width × 4, 128)`, a height that is a multiple of 32. The transport
checks this on every creation and refuses to work with another layout.

## Moving to another driver version

1. Capture again: `tools\dev\trace-gl-present.ps1` (the OpenGL driver: window
   surface, context private data, header, token) and a `gpu-probe` trace
   (linear allocations).
2. Compare with the tables above; if they match, add the version to the
   `supported_driver` check.
3. Run `tools\dev\test-all.ps1`, `test-present-redirect.ps1`,
   `test-kernel-limits.ps1` and a CTS sample.

How stable these values are between driver versions cannot be checked without
another version, which is why the port is tied to one.
