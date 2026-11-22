# Vulkan CTS results

Configuration: Iris 6100 (`8086:162b`), Intel driver `20.19.15.5171`, Windows,
CTS `vulkan-cts-1.4.5.5`.

## Full run, 2026-11-20

The whole `vk-default` mustpass (`tools\dev\run-cts-full.ps1`, 8.5 hours, two
processes): 2,849,001 of 2,888,682 tests were executed — 669,179 Pass,
2,179,726 NotSupported, 6 QualityWarning, 85 Fail, 5 ResourceError. Summary:
`native/artifacts/cts-full-20261120.json`.

- 19,681 tests of the list do not exist in this `deqp-vk` build
  (`renderpass.custom_resolve.*`, `image.astc_sample.*`).
- 20,000 tests of the first `draw` chunk were not counted because of a runner
  bug; rerun on the fixed build they give 14,047 Pass and 5,953 NotSupported.
- 2 tests were excluded in advance (the first two rows of the next table).

The run was made before the fixes below. Of the 90 tests that did not pass, 89
pass after the fixes and one failure is expected
(`native/artifacts/cts-rerun-failed-20261120.json`). The full run was not
repeated on the fixed build: only the failed tests, the `draw` chunk and the
groups next to each fix were rerun.

## Expected failures

| Test | Result | Cause |
|---|---|---|
| `dEQP-VK.tessellation.misc_draw.tess_factor_barrier_bug` | `VK_ERROR_DEVICE_LOST` after about 4 s (sometimes Pass) | The workload is longer than the 2-second Windows TDR timeout; it is not a hang. The vertex shader does about 6.0·10⁸ `atomicAdd` operations on one cell, and this GPU does 287 million of them per second. i915 on Linux has no such hard limit. Changing `TdrDelay` is a system setting the ICD does not touch. |
| `dEQP-VK.wsi.win32.swapchain.simulate_oom.min_image_count` | Timeout | A long test, not a bug: about 2,300 swapchain creation attempts at about 0.5 s each. All 16 configurations complete without errors. |
| `dEQP-VK.api.driver_properties.conformance_version` | Fail | The driver is not conformant and reports `conformanceVersion` 0.0.0.0. |

No unfixed driver bugs are known.

## Fixed

| What | Tests | Cause and fix |
|---|---|---|
| Signed enum bit-fields in `shader_info` | `spirv_assembly.instruction.graphics.execution_mode.depthunchanged_0`, `_2` — **GPU hang** | Under the Microsoft ABI an enum without a fixed type is `int`, and a bit-field of it is signed. `depth_layout` has 3 bits, so `FRAG_DEPTH_LAYOUT_UNCHANGED` (4) read as −4 and ELK decided the shader does not write depth. The shader did write it: `3DSTATE_PS_EXTRA` said `PSCDEPTH_OFF` while the render target write carried depth, a mismatch that hangs the hardware. The enums that `shader_info` stores in narrow fields now have an unsigned underlying type. |
| The shader register spill buffer did not exist | `graphicsfuzz` (42), `subgroups` (13), `spirv_assembly…opphi.wide` | `max_scratch_ids` is filled only by Mesa's Linux code and stayed zero, and the port rejected the "address below 4 GiB" flag. Shaders that spill registers wrote to unmapped GPU addresses: wrong results and GPU hangs. Spill buffers now live in their own range of the first 4 GiB. |
| A temporary file per shader compilation | the whole run: after 65,535 `MEM*.tmp` files every compilation took seconds | Mesa's `u_memstream` on Windows deleted the file while it was open, which left it behind. It is now opened delete-on-close. |
| `vkCreateBuffer` ignored `VkBufferUsageFlags2CreateInfo` | the first `draw` chunk (`VK_KHR_maintenance5`) | The legacy `usage` field was checked. |
| `VK_ACCESS_2_SHADER_STORAGE_READ_BIT` did not flush caches | `synchronization2…write_draw*_read_image_vertex.*_specialized_access_flag` (20) | Upstream HasVK does nothing for this flag; it is now handled like `SHADER_READ`. |
| 10,000 command buffers at once | `api.command_buffers.allocate_many_*`, `api.object_management.max_concurrent.command_buffer_*` | A command buffer got its 64 KiB buffer and binding table block when created; now when recording begins. |
| 10,000 events at once | `api.command_buffers.execute_large_primary` | 65,536 event slots instead of 4,096. |
| Graphics pipeline flags read from the legacy field | `pipeline.monolithic.misc.identically_defined_layout` | Upstream HasVK ignores `VkPipelineCreateFlags2CreateInfo` and returned `VK_PIPELINE_COMPILE_REQUIRED`. |
| A 64-bit vertex attribute wider than the shader reads | `pipeline.monolithic.vertex_input.component_mismatch.r64g64b64*_to_double`, `…_to_dvec2` | The vertex element was 256 bits while the shader read 128; such an attribute is now fetched as R64G64. |
| `continue` in a loop became `return` after the NIR cache (found by the bit-field audit, not by the CTS) | covered by `IrisNirSerializeTest` | An upstream bug in Mesa 26.2.7: `nir_jump_continue` became 4 while `nir_serialize.c` stores the jump type in two bits. The field is now 3 bits. |
| `sin`/`cos` slightly above 1 | `glsl.builtin.precision.{sin,cos}.*` (8) | Broadwell's math unit returns values like `0x1.0001a8p0`. The `precise_trig` compiler workaround is on (as in brw before Gfx10); `INTEL_PRECISE_TRIG=0` turns it off. |

## Legitimately unsupported

| Group | Reason |
|---|---|
| `api.copy_and_blit.multiplanar_xfer.*` | Needs a separate transfer queue; HasVK has one graphics and compute queue. |
| `image.texel_view_compatible.*.3d_image.*` for compressed formats | Gen8 can describe an uncompressed view of a compressed image for one layer only, and a 3D image view always covers all depth slices. Upstream HasVK advertised support and crashed; the port returns `VK_ERROR_FORMAT_NOT_SUPPORTED`. |
| Linux-only extensions | dma-buf, DRM modifiers, fd handles for memory and semaphores, `pci_bus_info`, `queue_family_foreign`. |

## GPU hangs during CTS work

Windows resets a hung engine by itself (TDR, LiveKernelEvent 141), but on this
machine such a reset twice ended in a system bug check (0x119 and 0xD1). Both
causes were the first two rows of the "Fixed" table. No test is known to hang
the GPU now. Right after a series of resets even the transport's own probe,
which uses neither the ICD nor the shader compiler, hung once; a repeated
`test-all` passed.
