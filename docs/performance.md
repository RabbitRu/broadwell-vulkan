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
borderless, render scale 0.4, lowest settings): Vulkan 25.5–26.3 FPS against
25.4–26.2 on DX11 (24.11.2026, fans at full speed: 26.04, 26.31, 25.77 against
25.97, 26.18, 26.06). Before presentation without a copy it was 23.2–24.7
against 26.0–26.9.

- The frame is bound by the CPU time of the game's own code. The game submits
  the whole frame with one `vkQueueSubmit`; the GPU needs about 22–25 ms for it
  and idles for about 14 ms per frame. GPU time per frame is the same under
  both APIs.
- Less GPU work would not speed the CPU up here: during the replay it runs at
  about 3.0 GHz (111 % of its base clock) with no performance limit in any of
  the 30 samples (`-Sample`, 24.11.2026, fans at full speed). In Talos, with
  the GPU 86–91 % busy, it runs at 2.56–2.68 GHz and is limited in most
  samples.
- The driver is almost absent from the critical path: 0.3 % of the main
  thread's samples. Command recording takes about 2.8 ms per frame on the
  game's render thread, which mostly waits. Under DX11 Intel's driver thread
  spends 16 ms per frame.
- The gap existed only at the full window size. In a 1280×800 window Vulkan is
  1–3 % faster than DX11 at every render scale (41.2 against 40.3 FPS at 0.4,
  37.2 against 36.8 at 1.0).
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
| clang 23 with `-march=broadwell` | draw recording −3 % |
| PGO | draw recording −10 % more |
| Dynamic buffer surface state from a template | draw recording −5 % on the replay bench |
| All pipelines in the driver's cache file | Dota 2 no longer compiles every pipeline on every start |
| Command buffer memory allocated when recording begins, small first blocks | Dota 2's thousands of small command buffers fit the driver's memory |
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
L3 cache settings, CCS_D fast clears, 2 MiB alignment, compiler flags, game
console variables and more). The registry of those experiments is in the
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
| 2560×1600 RGBA8 sRGB, clear only | 1.0 | 16 MB at 16 GB/s, the GPU's write speed |
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
