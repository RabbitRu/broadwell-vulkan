# Performance: Vulkan against DX11 on the Iris 6100

The goal of the project was Dota 2 on Vulkan faster than on Intel's DX11
driver, on a 2015 MacBook Pro (2 cores, 28 W shared between CPU and GPU). The
Talos Principle was the first test bench. Only general driver fixes were made:
there are no per-game profiles.

## Results

**The Talos Principle** (built-in benchmark, 640×480): Vulkan 70–83 FPS against
79–87 on DX11; 27.8 FPS at the start.

- The frame is set by the game's main thread (87–90 % busy) under both APIs.
  The driver costs it less than Intel's driver with the D3D11 runtime (2.35
  against 3.37 ms per frame), but the game's own code takes 2.27 ms longer
  under Vulkan.
- The reason is the game's Vulkan path: every frame it clears and rebuilds its
  alpha-tested shadow maps (3.7 million fragment shader invocations per map
  against 7 thousand under DX11). The GPU is 81–91 % busy against 73–77 %, and
  because of the shared power budget the CPU runs 4–19 % slower.
- With shadow rendering off (`+shb_bUpdateShadows 0`) Vulkan gives 114–124 FPS
  against 114–117: the whole gap is shadows, not the driver.

**Dota 2** (a match replay as a timedemo, 2,999 frames, window 2560×1600
borderless, render scale 0.4, lowest settings): Vulkan 28.1–28.4 FPS against
24.7–25.1 on DX11 (26.11.2026: 28.09, 28.19, 28.36 against 25.07, 24.76,
24.65 after a warm-up run). The lead comes from showing the window without
the compositor (see "Presents without the compositor"); composed like the
DX11 path, Vulkan was level with it: 25.72, 24.87, 25.12 against 25.40,
24.76, 25.39 on 25.11.2026 (the same build differs more from day to day than
the two APIs did). Before presentation without a copy it was 23.2–24.7
against 26.0–26.9.

- The frame is bound by the CPU time of the game's own code. The game submits
  the whole frame with one `vkQueueSubmit`; the GPU needs about 22–25 ms for it
  and idles for about 14 ms per frame. GPU time per frame is the same under
  both APIs.
- The CPU is not power limited here: during the replay it runs at about
  3.0 GHz (111 % of its base clock) with no performance limit in any of the 30
  samples (`-Sample`, 24.11.2026, fans at full speed); in Talos, with the GPU
  86–91 % busy, it runs at 2.56–2.68 GHz and is limited in most samples. The
  GPU costs frames another way: what it writes into window-size images slows
  the game's code through the shared memory (see "Window-size passes slow the
  CPU down").
- The driver is almost absent from the critical path: 0.3 % of the main
  thread's samples. Command recording takes about 2.8 ms per frame on the
  game's render thread, which mostly waits. Under DX11 Intel's driver thread
  spends 16 ms per frame.
- A lower display resolution gives little: full screen at 1280×800 with the
  same scene runs at 32.6–33.2 FPS against 29.4–30.1 at 2560×1600 on the same
  day, and DX11 at 29.3–29.7 (see "Lower display resolutions"; the 37–41 FPS
  that earlier versions of this file gave for a 1280×800 window were a
  measurement error).
- What remains at 2560×1600 is in the game: its worker thread takes 22.6 ms per
  frame under Vulkan against 19.6, across almost all modules.

## What made the difference

| Change | Effect |
|---|---|
| HiZ restored for depth (upstream HasVK dropped the HiZ surface) | a depth pass was 10 times slower without it |
| Asynchronous queue: one kernel submission per `vkQueueSubmit`, completion on its own thread | no waiting for the GPU on the application's threads |
| Paging waited for on a kernel event instead of polling with `Sleep(1)` | removed stalls of a 15.6 ms timer tick |
| Allocation without shared mutexes around kernel calls | removed main thread stalls of up to 40 ms |
| No CPU cache flushes around submissions (shared LLC) | less CPU time per submission |
| Presentation by a GPU copy inside the WSI submission, then with no copy for borderless windows | Dota 2 +4–6 % |
| A window that covers the screen is shown by exclusive flips, without DWM's copy | Dota 2 +11 % |
| clang 23 with `-march=broadwell` | draw recording −3 % |
| PGO | draw recording −10 % more |
| Dynamic buffer surface state from a template | draw recording −5 % on the replay bench |
| All pipelines in the driver's cache file | Dota 2 no longer compiles every pipeline on every start |
| Command buffer memory allocated when recording begins, small first blocks | Dota 2's thousands of small command buffers fit the driver's memory |
| Colour fast clears (CCS_D) for 2D images with one level and layer; their resolve recorded once per command buffer | Dota 2 +2–4 % (an image of the window's size that the game clears and does not use) |
| ELK: constants of a loop loaded before the loop, not in every iteration | FMA loop +8 % |
| ELK: bank conflicts of three-source instructions modelled as measured (below) | FMA loop +3 % |
| ELK: two constants of one instruction never share a register | FMA loop +11 % |
| ELK: vectors in the upper half of the register file, scalars of three-source instructions in the lower | fragment multiply-add chains +2 % |
| ELK: one compare for a `discard` on a float comparison | fragment multiply-add chains +1 % |

The ELK changes (`acfef37`, `5bdf740`) do not show in the games: interleaved
against the build before them (`0c68e55`) on 24.11.2026, after a warm-up run
of each, Talos 85.8 against 86.0 FPS (six runs each, 84.0–87.0) and Dota 2
25.87 against 25.96 (three runs each, 25.72–26.42). Nor does the GPU time
per frame (the game's GPU 3D load times the frame time, `-Sample`): Talos
10.67 against 10.67 ms (GPU 85–90 % busy), Dota 26.3 against 26.9 ms (the
old build alone spread 24.6–27.6). What these changes speed up (three-source
arithmetic, the compare of a `discard`) is a small part of either frame.

Many other ideas were measured and rejected (PMA workaround variants, MOCS and
L3 cache settings, 2 MiB alignment, compiler flags, game console variables
and more). The registry of those experiments is in the
history of this file and in the `debug` branch, which also keeps the
measurement switches they used.

## Measurements

Talos, main thread, ms per frame (DX11 at 84.3 FPS, Vulkan at 73.5):

| | DX11 | Vulkan |
|---|---|---|
| Game code | 6.19 | 8.46 |
| Driver (`igd10iumd64` + `d3d11` / this ICD) | 2.75 | 2.15 |
| Kernel calls (`win32u`) | 0.62 | 0.20 |
| `ntdll` (waits) | 2.10 | 2.48 |

Micro-benchmarks, this ICD relative to Intel's D3D11 driver (1.00 is equal,
lower is slower):

| Test | This ICD | Intel |
|---|---|---|
| `flops.comp`, FMA loop, ms per dispatch | 3.88 | 4.09 (1.05) |
| `sample.comp`, 16 bilinear samples per invocation, ms | 0.905 | 0.83–0.88 (0.92–0.97) |
| `vs_fetch.vert`, `texelFetch` in the vertex shader, ms | 6.46 | 6.46 (1.00) |
| `vs_fetch.vert`, no loop iterations, 3 million vertices, ms | 3.32 | 2.95 (0.89) |
| depth fill, no fragment shader, 8 layers of 1536², ms | 0.405 (reverse Z 0.359) | 0.362 (0.89; 1.01) |
| depth fill, empty fragment shader, ms | 0.460 | 0.457 (0.99) |
| depth fill, `depth_fill_kill.frag` (one compare and `discard`), ms | 4.6–4.9 (reverse Z 4.33) | 4.17 (0.87–0.90; 0.96) |
| depth fill, `depth_fill_discard.frag` (sample and `discard`), ms | 5.55–5.90 (reverse Z 5.31) | 5.09–5.23 (0.91–0.93; 0.96–0.98) |
| depth fill, `depth_fill_light.frag`, ms | 4.74–4.82 | 4.19–4.25 (0.88) |
| depth fill, `depth_fill_mad.frag` (64 multiply-adds), ms | 5.80 | 5.54–5.61 (0.96; 5.98 before) |

## Where the GPU time goes

GPU time per render pass, every pass bracketed by timestamps (which
serialize the GPU, so the sum is a little more than the frame's real GPU
time). Dota 2 renders with `vkCmdBeginRendering`; a profiler that only sees
`vkCmdBeginRenderPass` (the debug branch's) sees none of its passes.

Dota 2, replay window, 25.3 FPS, 25.1 ms of passes per frame:

| Pass | ms/frame | Note |
|---|---|---|
| 1024×640 RGBA8 sRGB + D24S8, ×3.6 per frame, ~117 draws | 10.4 | the scene; ~7.6 M fragment shader invocations per frame, spread over many pipelines (the largest 1.9 ms: VS ~990, FS ~470 instructions, 17 sends) |
| 1024×640 RGBA16F + D24S8, ×2.6, ~76 draws | 4.3 | |
| 2560×1600 BGRA8 sRGB + D24S8, ×20.6, ~13 draws | 2.7 | interface |
| 2560×1600 BGRA8 sRGB, ×2 (one draw of a 4-instruction shader: 1.31 ms) | 1.5 | upscale to the window |
| 2560×1600 RGBA8 sRGB, clear only | 1.0 | 16 MB at 16 GB/s, the GPU's write speed; a fast clear since CCS_D is on (below) |
| 2560×1600 depth/stencil clears, ×2 | 1.1 | sampled depth cleared to 1.0: not fast (see below) |
| copies, small passes | ~4 | |

The full-window passes run at memory speed (a 2560×1600 clear at 16 GB/s,
`bandwidth_probe`: buffer fill 7–9 GB/s, copy 12 GB/s read + write), and the
scene has no single expensive pipeline. Under DX11 the GPU time per frame is
the same (see Results), and Intel's driver splits its submissions, so the
DX11 proxy cannot give comparable times per pass (`D3D11_PROXY_SEGMENTS`
counts the GPU's waits as well; shares only).

The Talos Principle, 80 FPS, 9.7 ms of passes per frame: the two shadow maps
(1536², D16, alpha test, 3.7 M fragment shader invocations each) 4.9 ms, the
main pass 2.5, the depth prepass 1.0, the rest under 0.4 each. The shadow
maps are reverse Z and get HiZ fast clears; on the same draw Intel's driver
is not faster (registry of the debug branch, C6): the gap to DX11 is the
game's own work.

## Window-size passes slow the CPU down

Dota 2's frame is bound by the CPU, the CPU is not power limited there, and
still the GPU's work costs frames: what the GPU writes into window-size
images slows the game's own code through the shared memory. Measured on
25.11.2026 with a switch that shrinks render areas to 16×16 pixels (the
picture is wrong, the replay and the game's CPU work are the same):

| Dota 2 replay, 2560×1600 | FPS | Frame, ms | Client Simulation, ms | Client Rendering, ms |
|---|---|---|---|---|
| as it is | 25.73, 24.74 | 40.3 | 12.9 | 23.6 |
| passes at least 2048 wide shrunk (6.6 ms of GPU work) | 30.18, 29.60 | 34.2 | 10.5 | 20.3 |
| every pass shrunk (the GPU does almost nothing) | 32.62, 31.30 | 31.9 | 9.7 | 18.9 |

(Sections from the game's own `timedemo_profile.csv`; physics and animation
get 20 % shorter as well.) The six milliseconds of window-size passes cost
almost six milliseconds of frame; the 15 ms of scene passes, which compute
and sample but write little, cost two. So bytes written to memory matter
here, not GPU time.

- **Not the cache policy.** Render targets and depth of 8 MiB and more
  without LLC caching (MOCS 0x38): 25.46, 25.59. Intel's value for everything
  (0x79, found in its surface states; Mesa uses 0x78): 25.31, 24.99.
- **Not the write path.** One sample written to an RGBA8 target of 1536²,
  8 layers (`depth_fill_probe` depth mode 3, `d3d11_depth_fill_probe color`):
  4.23–4.35 ms here, 4.24–4.27 ms on Intel's driver.
- **What Dota 2 does with its window-size images** (a log of one frame):
  - an RGBA8 image of 2560×1600 is cleared to (0, 0, 0, 1) every frame and
    never drawn into, sampled or copied: 16 MB written for nothing;
  - the depth/stencil image is cleared twice, for the scene (which then uses
    1024×640 of it) and for the interface; each clear takes 0.52 ms, half of
    a colour clear of that size, and fast clears of depth to 1.0 (wrong
    picture, upper bound) gave 25.45, 24.16 against 23.88–24.71: nothing;
  - the scene is stretched to the window by one draw (16 MB), and the
    interface is about four million blended pixels in 20 passes;
  - DWM composes the window under both APIs (5 % of the GPU and 12–13 % of a
    logical CPU in `dwm.exe`, with Vulkan and with DX11 alike).
- **Colour fast clears (CCS_D)** take the unused clear away: the time of the
  window-size passes goes from 6.65 to 5.43 ms per frame. An image that is
  sampled after a fast clear is resolved first, which writes what the clear
  did not (time between passes 0.75 → 1.41–1.48 ms), so the gain is the
  unused image. Two interleaved series: 25.31, 26.05, 25.97, 26.22 against
  23.88, 24.71, 24.06, and 25.22, 24.08, 25.25, 26.17, 25.27 against 24.84,
  24.26, 24.14, 25.05, 25.29 (25.50 against 24.53 on average). The Talos
  Principle does not change: 78.3, 79.2, 78.9, 76.8 against 77.4, 78.6, 80.2,
  79.1.
- **Resolves that are known to do nothing are not recorded.** Every layout
  transition of a CCS image out of the attachment layout records a resolve
  that the GPU skips unless the image is fast-cleared; skipped, it still
  costs two end-of-pipe flushes, and Dota 2 makes about 40 such transitions
  per frame. After the first one in a command buffer the answer is known
  until that command buffer fast-clears the image again: 1.41, 1.48 ms
  between passes against 1.60, 1.61.

The largest of these writes was not in the game's frame at all: DWM's copy of
the window, next section.

The switches of these measurements (shrunk render areas, the frame log, MOCS,
pass times with `vkCmdBeginRendering`) are a patch in the `debug` branch,
`tools/measure-2026-11-25`.

## Presents without the compositor

DWM composes a window that covers the screen like any other: it copies the
frame. What a D3D11 window of 2560×1600 at 60 FPS costs it on this system
(`d3d11_flip_probe` with `tools\dev\test-flip-probe.ps1`, GPU 3D load):

| Swap chain | dwm.exe | the program |
|---|---|---|
| blt model, borderless window | 14.6 % | 21.3 % |
| flip model (discard or sequential), borderless, in the foreground | 12.9–13.6 % | 6.9 % |
| either, in DXGI's full-screen state | 0 | 6.4 % |

So Windows does not flip a windowed swap chain directly here, flip model or
not; only DXGI's full-screen state shows the application's buffer. A trace of
that state (`tools\dev\trace-d3d11-present.ps1 -Fullscreen`) is three kernel
calls around ordinary rendering: exclusive ownership of the output, a mode
set with a primary, and a present with the `Flip` flag per frame. The same
calls from this project's own device (`flip_exclusive_probe`, no D3D): 600
flips at 60.3 per second with DWM idle, and 5,900 per second without waiting
for the vertical blank; a screen capture through Desktop Duplication shows
the primaries (`screen_capture`).

The driver now does this for a window that covers the primary screen
([design.md](design.md), presentation). A Vulkan window of 2560×1600 that
only clears its frame (`flip_wsi_probe`): 1,008 FPS against 232 through DWM
with IMMEDIATE, 59.4 with FIFO, DWM idle, the screen's pixels exact
(`tools\dev\test-flip.ps1`).

Dota 2, the flips on and off in turn (`HASVK_WDDM_PRESENT_FLIP=0`),
25.11.2026: 28.27, 28.13, 26.48, 27.68 FPS against 25.29, 23.75, 24.98, 26.06,
24.44 (27.6 against 24.9, +11 %). DWM's share of the GPU goes from 5.2 % to
nothing, its CPU time from 12.6 % to 9.5 % of a logical CPU. The game's DX11
path is composed by DWM as well (5.1 % of the GPU).

## Lower display resolutions

In the game's exclusive full-screen mode the display runs at the game's
resolution and the panel stretches the picture, which costs nothing. The
driver's flips work in every mode: the swapchain's images are primaries of
the mode's size. Measured on 26.11.2026 with the mouse pointer in the middle
of the screen (below); the render scale keeps the scene at 1024×640 unless
noted, so only the passes of the window's size shrink:

| Display mode (render scale) | Vulkan, FPS | DX11, FPS |
|---|---|---|
| 2560×1600 (0.4), borderless window | 30.07, 29.40, 29.90 | 25.94 |
| 1920×1200 (0.533) | 31.14 | |
| 1440×900 (0.711) | 32.10 | |
| 1280×800 (0.8) | 33.22, 32.55 | 29.32, 29.65 |
| 1280×800 (0.6): scene 768×480 | 34.44 | |
| 1280×800 (0.4): scene 512×320 | 35.20 | |

A quarter of the window's pixels is worth 3 FPS (the frame goes from 33.6 to
30.4 ms) and a quarter of the scene's pixels 2 more: the frame is the game's
CPU work, and neither size takes much of it away. Under DX11 the game's
full-screen mode is DXGI's, which DWM does not compose either, and Vulkan is
11 % ahead there as well.

**The mouse pointer is part of this benchmark.** At a screen edge the pointer
scrolls the replay's camera, which ends in a corner of the map under the fog
of war (seen in screenshots of both runs). There the two scene passes hold
about 130 draws per frame instead of about 245, and the same replay gives
38.95 FPS instead of 29.15 at 2560×1600 (41.28 against 28.52 in a 1280×800
window, where the game keeps the pointer inside the window, at its corner).
The game's own profile tells the two apart: Frame Boundary takes 3.3–3.5 ms
with the camera in the corner and 8–9 ms on the action. The measurements of a
1280×800 window of 21.11.2026 (37–41 FPS, Vulkan 1–3 % faster than DX11 at
every render scale) have the profile of the scrolled camera under both APIs
(Frame Boundary 3.7–3.9 ms) and are withdrawn. The series at the full window
size in this file have the profile of the action (8.5–11 ms).
`run-dota.ps1` now puts the pointer in the middle of the screen 20 s after
the start and rejects a run during which it is found near an edge.

## ELK against Intel's compiler

`flops.comp` (a loop of eight FMA chains) ran at 0.84 of Intel's driver: 4.86
ms per dispatch against 4.09. It now takes 3.88 ms. The machine code of both
compilers was compared (see "How to measure"); what Intel's code did
differently was the placement of registers, not the instructions:

- **Constants in a loop.** ELK loads a constant that an instruction cannot
  take as an immediate (every `MAD` source) right before its first use. Inside
  a loop that is two `MOV`s per iteration; they are now placed before the
  outermost loop (at most two registers' worth).
- **GRF bank conflicts.** The register file has four banks (g0–g63 and
  g64–g127, even and odd). Measured with hand-placed registers: a three-source
  instruction loses time when its **second** source is in the same bank as the
  first or as the third; the first and the third do not disturb each other.
  Mesa's pass knew only the second and third, and only looked at the first
  register of each. A scalar against a SIMD16 vector therefore always
  conflicted in one half, and two constants in one register always did. The
  pass now counts both pairs for every SIMD8 half and can move a value into
  unused registers of another bank (it could only exchange values of equal
  size); constants read by one instruction get separate registers.

| `flops.comp`, ms per dispatch | |
|---|---|
| Intel's D3D11 driver | 4.09 |
| before | 4.86 |
| constants before the loop | 4.49 |
| vectors and scalars in different banks | 4.36 |
| the two constants in different registers | 3.88 |

Measured and not adopted:

- Compacting three-source instructions with a scalar source (Intel's code
  does; ELK's swizzle for a replicated source is not in the compaction table):
  5 % less code, no time gained, and the ALU vertex shader test lost 3 % from
  the changed layout alone.
- Scheduling the instructions that feed a branch first after register
  allocation: 2 % on the FMA loop, nothing measurable elsewhere.
- Keeping 16-byte instructions on 16-byte boundaries: differences within 1 %,
  both ways.
- Operand order of `MAD`, a `BREAK` in place of a predicated `WHILE`: nothing.

The other tests gave no lead: for the sampler and vertex shader tests Intel's
compiler emits the same instructions as ELK (the D3D11 "ALU" vertex shader has
two branches where the GLSL one has four, so that pair does not compare the
compilers).

## Fragment and vertex stages against Intel's driver

The fragment tests are depth-only passes whose shader ends in a `discard` that
never happens (`depth_fill_probe` with `depth_fill_kill.frag`,
`depth_fill_light.frag`, `depth_fill_mad.frag`; the HLSL twins are in
`d3d11_depth_fill_probe`).

- **Register placement.** In `depth_fill_mad.frag` ELK and Intel's compiler
  emit the same instructions. Intel keeps every vector in g64–g127 and the
  constants below; ELK's allocator (round robin over the whole file) spread
  the vectors over g7–g113, so half of the multiply-adds read a SIMD16 vector
  and a scalar from one bank in one of their halves. The allocator now looks
  in the upper half first, and in the lower half first for the scalars that
  three-source instructions read (5.98 → 5.85 ms).
- **`discard` on a comparison.** `if (a < b) discard;` took two compares (the
  comparison into a register, then a test of the register for zero): the
  negated comparison in one instruction discards a NaN that the original
  keeps. Vulkan allows NaNs to be ignored unless the module asks for them
  (`SignedZeroInfNanPreserve`, `FPFastMathDefault` or `FPFastMathMode`
  without `NotNaN`); without such a request the discard is one compare, as in
  Intel's kernels (5.85 → 5.80 ms).
- **What remained was the depth clear, not the shader.** A pixel that goes
  through a shader with a `discard` cost 4.6–4.8 ms per pass here and 4.17 ms
  on Intel's driver with five instructions on each side. The probe clears its
  D16 buffer to 1.0, and this ICD fast-clears (HiZ) depth only to 0.0
  (`anv_image_hiz_clear_value`); Intel's driver fast-clears to 1.0. With a
  full clear the pass after it costs 12 % more. With reverse Z (cleared to
  0.0, `GREATER_OR_EQUAL`: `depth_fill_probe` argument 6 = 1) the probe is
  where Intel's driver is: no fragment shader 0.359 ms against 0.362, the
  `discard` test 4.33 against 4.17, sample and `discard` 5.31 against
  5.09–5.23. Before that was found, these were set to Intel's values one at a
  time with no effect: "pixel shader does not write to a render target"
  (3DSTATE_PS_EXTRA bit 30, not in Mesa's Gen8 description), forced thread
  dispatch off, double-sided stencil off, single program flow, the vector
  mask, the denormal mode, depth MOCS 0x69/0x79/0x68, CACHE_MODE_1 bit 14,
  the default L3 configuration (Intel's driver never writes L3CNTLREG in 3D
  mode), the DX10 rasterization mode, statistics counters.
- **Fast clears to 1.0** (measured, not adopted):
  - The Gen8 sampler returns 0.0 for fast-cleared HiZ blocks also with Red
    Clear Color = 1 in the sampling surface state (`depth_sample_probe`: the
    four cases cleared to 1.0 read 0.0). A depth buffer that is sampled can
    therefore be fast-cleared only to 0.0, or to 1.0 with a depth resolve
    before every sampling. Dota 2 clears its depth buffers of 2560×1600 and
    draws mostly into 1024×640 of them (render scale 0.4), so the resolve
    would write more than the clear costs now.
  - Images that are never sampled (no SAMPLED, TRANSFER_SRC or
    INPUT_ATTACHMENT usage) could use 1.0: the probe without SAMPLED went
    from 0.405/4.88/5.93 ms to 0.359/4.29/5.41. Neither game gains: all of
    Dota's depth buffers are sampled, and the one depth buffer of Talos that
    is not sampled is reverse Z and would lose its fast clear (it would need
    the value chosen per image at its first clear).
- **Vertices.** With no work in the shader this ICD passes exactly one vertex
  per GPU clock (950 MHz) whatever the shader and the topology. Intel's driver
  does the same for points and strips, and nine vertices in eight clocks for
  a non-indexed triangle list. Not the cause: the vertex buffer read for
  `gl_VertexIndex`, cull mode and winding, primitive restart, rasterizer
  discard, depth clamp, the "vertex URB entry output" fields of 3DSTATE_VS
  (upstream leaves them 0, Intel sends offset 1 and length 1), the statistics
  of VF, VS and the clipper, the topology field of 3DPRIMITIVE.
- **`pow` with a constant exponent** is four multiplications in Intel's
  kernel (the HLSL compiler may do that) and one POW here. Replacing it did
  not change the time of `depth_fill_light.frag`, which is bound by the fixed
  cost above; not adopted without a test that shows it.

Dota 2, threads in % of a logical CPU: Vulkan — main 88, worker 55, render 18,
driver queue about 3; DX11 — main 89, worker 51, render with Intel's driver 42.

## How to measure

Build the driver (`tools\build-icd.ps1`) and the probes
(`tools\dev\build-probes.ps1`), then from the repository root:

```powershell
# Dota 2 (always with -insecure; results in build\dota\<Tag>.*)
.\tools\dev\run-dota.ps1 -Api Vulkan -Tag v1
.\tools\dev\run-dota.ps1 -Api Direct3D11 -Tag d1
.\tools\dev\run-dota.ps1 -Tag s1 -Sample                               # threads, CPU frequency, GPU load
.\tools\dev\run-dota.ps1 -Tag r1 -RipModule vulkan_hasvk_wddm.dll      # sampled stacks
# DX11 GPU shares per render target (Dota 2 needs pseudo-frames of N segments)
.\tools\dev\run-dota.ps1 -Api Direct3D11 -Tag p1 -Env D3D11_PROXY_SEGMENTS=600 -D3D11Proxy build\probes\d3d11-proxy\d3d11.dll
# The Talos Principle (Steam must be signed in to the account that owns the game)
.\tools\dev\run-talos.ps1 -Api Vulkan -Tag v1    # -Sample, -RipModule, -D3D11Proxy build\probes\d3d11-proxy\d3d11.dll
# what DWM spends on a window of the screen's size (D3D11), and the driver's exclusive flips
.\tools\dev\test-flip-probe.ps1 -Exe build\probes\d3d11_flip_probe.exe -Arguments '10','flip_discard','1'
.\tools\dev\test-flip.ps1
.\tools\dev\run-dota.ps1 -Tag noflip -Env HASVK_WDDM_PRESENT_FLIP=0
# the game's full-screen mode at a lower display resolution, the scene's size kept
.\tools\dev\run-dota.ps1 -Tag s1280 -Screen 1280x800 -Extra '+mat_viewportscale 0.8'
# analysis
.\.tools\python\python.exe -I tools\dev\rip-compare.py <dx.rip.txt> <FPS> <vk.rip.txt> <FPS>
.\.tools\python\python.exe -I tools\dev\talos-sections.py --since 2026-11-19T20:00
```

Shader code of both compilers:

```powershell
# ELK: assembly of every shader the process compiles (stderr), cache off
$env:INTEL_DEBUG = 'cs,vs,fs'; $env:HASVK_WDDM_INTERNAL_CACHE = '0'
.\tools\dev\run-icd.ps1 -Executable (Resolve-Path build\probes\flops_probe.exe) -Arguments (Resolve-Path build\probes\flops.spv), 2
# Intel: the D3D11 probes find their compiled kernel in the process's memory
$env:D3D11_PROBE_DUMP = "$PWD\build\intel-flops"; .\build\probes\d3d11_flops_probe.exe 3
.\build\mesa-compiler-clang\src\intel\wddm_compiler\IrisElkCompile.exe --disasm build\intel-flops-0.bin
```

The probes of both drivers also save the command buffers that hold a draw
(`-batch` files: the driver's 3DSTATE commands are in front of the
3DPRIMITIVE, 0x7b000005), and with `D3D11_PROBE_FIND=11000001` every place
with an MI_LOAD_REGISTER_IMM (`-find` files), which is how the register
writes of Intel's driver were listed.

The dump is every place in memory where the shader's constants occur close
together (`native\tests\kernel_dump.h`), so several numbered files come out;
the kernel is the one that disassembles into a program (`z-1.dll` must be on
`PATH` for `IrisElkCompile.exe`). `flops_probe` also checks its results against
the CPU.

`run-dota.ps1` plays ticks 30000–33000 of a replay you have downloaded, saves
the player's video settings before the run and restores them after it. For
function names in the samples build with `tools\build-mesa-compiler.ps1
-DebugInfo` and then `tools\build-icd.ps1 -SkipBuild`.

Pitfalls:

- **Spread and warm-up.** Consecutive runs get slower (Dota 2 on Vulkan 24.7 →
  23.2 FPS): compare only interleaved runs within one series, at least three
  pairs. The first run after a new build is cold (pipelines are compiled) and
  does not count.
- **Not right after a build.** Six minutes of compiling heat the laptop: the
  first Talos block after it ran at 78.5 FPS and the same binary at 83.3 FPS
  twenty minutes later. One binary spreads from 75 to 85 FPS between Talos
  runs. Two builds share one shader cache file, so alternate them in blocks of
  four runs, drop the first run of each block, start with the old build, and
  believe only a difference that every block shows.
- Measure thread CPU time with `QueryThreadCycleTime`; `GetThreadTimes` counts
  timer ticks and understated the main thread three times.
- PDH counters are not found by their English names in a process with a
  non-English UI language without `SetThreadUILanguage(0x409)`.
- `rip_sampler` with stack walking slows the game by 8–12 % (shares stay
  comparable, FPS does not); with `RIP_NOWALK=1` it barely does.
- Dota 2 stores console variables given on the command line in its configs;
  `run-dota.ps1` restores them.
- GPU micro-benchmarks shorter than about a second run at a cold GPU frequency:
  take the best of 250–400 repeats.
- Do not run the CTS, games and a build at the same time.
