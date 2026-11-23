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
borderless, render scale 0.4, lowest settings): Vulkan 25.5–25.7 FPS against
25.4–26.1 on DX11. Before presentation without a copy it was 23.2–24.7 against
26.0–26.9.

- The frame is bound by the CPU time of the game's own code. The game submits
  the whole frame with one `vkQueueSubmit`; the GPU needs about 22–25 ms for it
  and idles for about 14 ms per frame. GPU time per frame is the same under
  both APIs.
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
lower is slower): vertex shader with `texelFetch` 1.00; empty vertex shader
0.89–0.94; sampler 0.95–0.98; FMA 1.05 (0.84 before the ELK changes); depth
fill with `discard` 0.96–1.0.

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
