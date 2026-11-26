# How the driver works

The ICD is Mesa HasVK 26.2.7 with its Linux platform (i915, DRM, GEM) replaced
by a Windows one that submits Gen8 command buffers through the documented
`D3DKMT*` kernel calls of WDDM. It supports only the Iris 6100 (`8086:162b`)
with Intel's kernel driver `20.19.15.5171`; the private data that driver
expects inside the documented calls is described in
[intel-interface.md](intel-interface.md).

## 1. WDDM transport (`native/src`, `mesa/anv_wddm_*.c`)

- A device, a context, a paging queue, allocations at fixed GPU addresses (a
  16 GiB window, state pools at fixed addresses), submission and confirmed
  completion. The application heap is 3 GiB, one allocation is at most 1 GiB,
  there are at most 65,536 allocations; the driver's own memory has a separate
  1 GiB limit (`ANV_WDDM_INTERNAL_BYTES`). `HASVK_WDDM_HEAP_MB` shrinks the heap
  for the out-of-memory test.
- The paging queue is waited for on a kernel event, not by polling. Allocation
  does not wait for paging on the CPU: the next submission waits for it on the
  GPU, as the D3D runtimes do. Kernel calls during allocation take no shared
  mutex (with one, the game's main thread stalled for up to 40 ms).
- No CPU cache flushes around submissions: the GPU is coherent with the CPU
  caches (shared LLC), verified by `IrisCoherenceProbe`. CPU mappings are
  cached and as fast as D3D11's (about 15 GB/s write, 14 GB/s read).
- All command buffers of one `vkQueueSubmit` go to the kernel as one
  submission (the tail of a buffer jumps to the next one). The queue is
  non-blocking: a queue thread submits, a completion thread waits for the GPU,
  checks markers and guard areas and signals the sync objects.
- `MI_SEMAPHORE_WAIT` never completes in this context, so `vkCmdWaitEvents`
  outside a render pass splits the batch and the queue waits for the event on
  the CPU. The other 46 register and MI command checks HasVK relies on pass
  (`tools\dev\test-kernel-limits.ps1`).
- Command buffers are chains of 8 KiB and then 64 KiB buffers, allocated when
  recording begins; spare ones are shared by the device. Thousands of small
  command buffers (Dota 2 has about 4,400) would otherwise exhaust the driver's
  memory.

## 2. What differs from upstream HasVK

- Vulkan 1.3 with 108 device extensions, enabled from a verified list
  (`verified_device_extensions` in `anv_wddm_device.c`). Only the Linux-only
  extensions are off (dma-buf, DRM, fd handles, `pci_bus_info`,
  `queue_family_foreign`). There are no sparse resources, no external memory
  and no transfer queue; `descriptorIndexing` is not advertised, as upstream
  for Gen8.
- HiZ for depth is restored: upstream HasVK computed the HiZ surface and
  dropped it, and without it a depth pass is 10 times slower than Intel's
  driver. MCS is on. CCS_D (colour fast clears) is on for 2D images with one
  level and one layer that are not swapchain images: with more levels or
  layers multisample resolves into mip level 3 and array layer 1 were wrong,
  and DWM shows swapchain images without a resolve. A layout transition
  records its fast-clear resolve only once per command buffer, until that
  command buffer fast-clears the image again.
- Gen8 only: other generations are removed from the build and generation
  checks fold to constants (`gen8_fold()`).
- The driver is built by clang-cl with `-march=broadwell` and a PGO profile,
  without asserts.
- A descriptor set whose layout has no descriptor buffer, bound where the
  shader reads one, gets a null surface. Upstream would dereference NULL; here
  the batch was rejected and Dota 2 got `VK_ERROR_DEVICE_LOST`.
- After `PIPELINE_SELECT` to GPGPU the driver sends a `MEDIA_VFE_STATE` with
  one URB entry (Intel's WaSendDummyVFEafterPipelineSelect) and then the bound
  pipeline's own again. Intel's Windows driver sends such a packet on this
  hardware in every batch that switches to GPGPU; upstream has no such
  workaround for Broadwell. Without it the first compute dispatch of a device
  hung the GPU about once in 500 device creations, but only while another
  process was drawing through the desktop compositor: the thread was never
  dispatched. On an idle desktop 35,000 device creations passed, and 15,000
  next to a D3D11 compute load. Later dispatches of the same device never
  hung. With the workaround 28,000 creations passed (6,000 of them on the
  final build) where the driver without it hung six times in about 4,000.
  The dummy packet replaces the pipeline's state: a dispatch, a draw and
  another dispatch of the same pipeline in one command buffer hung Dota 2 in
  every run until the pipeline's packet was sent again (`IrisIcdProbe` has the
  case now; the CTS groups that were run have none).

Classes of porting bugs that were hit, worth checking first when something new
breaks:

- State that only Mesa's Linux code fills in: `max_scratch_ids` (the register
  spill buffer; its absence hung the GPU), driconf values that are never read
  (`lower_depth_range_rate`), `robust_buffer_access`.
- The Microsoft ABI: a bit-field of an enum type is signed
  (`FRAG_DEPTH_LAYOUT_UNCHANGED` read as −4 and hung the GPU). Check new build
  directories with `-Wbitfield-enum-conversion`.
- An upstream bug: `continue` in serialized NIR was stored in 2 bits and came
  back as `return`. The field is widened; `IrisNirSerializeTest` covers it.
- `u_memstream` left a temporary file per shader compilation.
- The generated files `anv_wddm_graphics_*.c` and `anv_wddm_command_*.c` are
  built from the **original** upstream files (`graphics_sources.py`,
  `compute_sources.py`), so an edit made to an upstream file through
  `apply-compiler-port.py` does not reach them. In `apply-compiler-port.py`
  each file has exactly one `edit()` call.

The CTS-found bugs and their fixes are listed in
[cts-expected-failures.md](cts-expected-failures.md).

## 3. Presentation (`anv_wddm_present.c`)

- The same way Intel's OpenGL driver does it: the GPU copies the frame into the
  window's shared surface, and the submission carries the `PresentRedirected`
  flag and a token; DWM shows it. The copy is recorded at `vkQueuePresentKHR`
  and goes in the WSI submission of the same queue, so nothing waits for the
  frame on the CPU.
- Without the copy: B8G8R8A8 swapchain images are themselves allocated as
  window surfaces, and presenting to a borderless window of exactly their size
  hands the image itself to DWM (an empty command buffer with that image's
  token). DWM reads such a surface only as B8G8R8A8, so R8G8B8A8 formats are
  not reported for a window. Framed windows, child windows and size mismatches
  go through the copy.
- Without the compositor: a window that covers the primary screen. DWM
  composes such a window like any other (on this system also for a flip-model
  DXGI swap chain; only DXGI's full-screen state shows the application's
  buffer directly), which costs a copy of the screen per frame. The driver
  takes that state itself, as DXGI does it, with its own device:
  - the images of a swapchain of the screen's size (the display's mode when
    the swapchain is created, so also a lower mode that a game has set for its
    full-screen window; IMMEDIATE or FIFO,
    B8G8R8A8) are primaries of the output: X-tiled, no aux, allocated with the
    `Primary` flag and Intel's description of a primary;
  - at a present, if the window has no frame, covers the primary screen, is in
    the foreground and no visible window is above it, the device becomes the
    exclusive owner of the output (`D3DKMTSetVidPnSourceOwner`,
    `D3DKMTSetDisplayMode`), and the frame is shown by `D3DKMTPresent` with
    the `Flip` flag right after the WSI submission; IMMEDIATE flips at once,
    FIFO at the vertical blank;
  - every kernel submission names the primaries its command buffers render
    into (`WrittenPrimaries`), so the kernel holds rendering back while that
    primary is still on the screen;
  - a watcher thread gives the output back within 30 ms when the window stops
    being alone on the screen (Alt+Tab, a notification, a window shown above),
    also when the application has stopped presenting; a refused flip does the
    same. Until the window is alone again its frames are copied into the
    window surface as for any other window.
  MAILBOX stays with the compositor (a flip either tears or waits).
  `HASVK_WDDM_PRESENT_FLIP=0` turns this off.
- If the kernel refuses the present (the display is asleep), the submission is
  repeated without the token and the frame goes through GDI; child windows
  always use GDI. FIFO sleeps until vblank using
  `DwmGetCompositionTimingInfo`. Images are handed out round-robin.
- Cost: the kernel call with a present takes about 1 ms of the queue thread's
  CPU; the copy of a 2560×1600 frame takes about 2.4 ms of GPU time.

## 4. Shader cache

`%LOCALAPPDATA%\HasVK-WDDM\internal-cache.bin` (BLORP shaders) and
`default-cache.bin` (all pipelines, including those created with the
application's `VkPipelineCache`, up to 256 MiB). The cache UUID changes with
every build and with `INTEL_DEBUG`, so the first run after a rebuild is cold.

## 5. Risks

- **A GPU hang can take the system down.** Windows resets a hung engine by
  itself (LiveKernelEvent 141; the application gets `VK_ERROR_DEVICE_LOST`),
  but Intel's kernel driver ended in a bug check after such a reset twice
  (0x119 and 0xD1) out of about 50 hangs during development. Both causes found
  (the missing register spill buffer and `DepthUnchanged`) are fixed. Reports
  are counted by the `Kernel_141_*` directories under
  `C:\ProgramData\Microsoft\Windows\WER`.
- Precautions when changing the driver: do not change GPU state beyond what the
  PRM requires; a wrong address in a GPU command (a NULL buffer, for example)
  is a path to a hang, so the driver rejects such batches instead of submitting
  them; after a system failure run `test-all` a few times before any load.
- To find where a hang happens: emit a `PIPE_CONTROL` with a stall and a write
  of a step number into command buffer memory before every draw and BLORP
  operation; after the device is lost the last number names the step.
- A rare hang needs a fast reproduction and a control under the same
  conditions. `IrisIcdProbe.exe` with `ICD_PROBE_FIRST_ONLY=1` and
  `ICD_PROBE_CYCLES=500` creates a device and makes its first dispatch about
  100 times a second. The hang of the first dispatch (section 2) depended on
  what else was drawing: with a window animating it came within seconds, on an
  idle desktop never. A first "fix" (the L3 configuration programmed once per
  device) passed 21,000 device creations only because those series happened to
  run on an idle desktop, and the unchanged driver passed 35,000 there too.
  Alternate the old and the new build within one run (new, old until it
  fails, new again) before believing a fix.
- What Intel's driver does in the same situation is the best lead for a
  hardware hang. Its command buffers stay mapped in the process:
  `d3d11_flops_probe.exe` with `D3D11_PROBE_DUMP=<prefix>` writes the ones with
  a `GPGPU_WALKER` to `<prefix>-batch-<n>.bin` (raw dwords). The dummy
  `MEDIA_VFE_STATE` was found there, after registers and counters of new
  contexts had shown nothing (`ICD_PROBE_REGS`).
- The driver prints the WDDM execution state of a lost device to stderr (3:
  this context hung, 2: another one did); `gpu_reg_monitor` reads GPU registers
  from a context of its own, `hasvk_wddm_icd_read_register` from the device's.
- The Intel interface was taken from one driver version; on another the ICD
  does not enable itself.
- Vulkan 1.4 is out of reach for HasVK without new features
  (`load_store_op_none`, `map_memory2`, `host_image_copy` and others).
- The CPU has 2 cores and shares a 28 W power budget with the GPU: more GPU
  work means a slower CPU, which shapes every performance result.
