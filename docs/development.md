# Development

Everything here is needed to work on the driver, not to build or use a
release. The scripts live in `tools\dev`; the build scripts they call are in
`tools`. Run them from the repository root in PowerShell
(`powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script>`).

## Toolchain

- `tools\bootstrap.ps1` installs MSVC and the Windows SDK into `.tools\vs`,
  Python 3.12.10 into `.tools\python`, and unpacks the official LLVM 23.1.6
  release archive into `.tools\llvm-23.1.6` (clang-cl, lld-link,
  llvm-profdata; no installer, no administrator rights). Downloads are pinned
  by SHA-256 in `tools\dependencies.json`, Python packages in
  `tools\requirements.txt`.
- `tools\toolchain-lock.json` pins MSVC 14.44.35207, the Windows SDK
  10.0.26100.0 and the hashes of the compilers and of the KMT headers; a build
  stops on a mismatch. The online Visual Studio installer can change what it
  installs, so keep the installed tools if you need this exact version again.
  `HASVK_UNLOCKED_TOOLCHAIN=1` accepts whatever MSVC and SDK are installed
  (the CI build uses it); clang stays pinned.
- Mesa is compiled by clang-cl with `-march=broadwell`: the driver only runs
  next to a Broadwell GPU, so the CPU is always a Broadwell. The native probes
  are compiled by MSVC.
- The changes to Mesa are made by `mesa\apply-compiler-port.py` on the pinned
  tree in `third_party`; every build regenerates
  `mesa\patches\compiler-port.patch` from them.
- If `PYTHONHOME` is set in your environment, remove it before building.

`tools\build-mesa-compiler.ps1` options:

| Option | Meaning |
|---|---|
| `-Jobs N` | parallel compile jobs (default 2) |
| `-DebugInfo` | debug information and a PDB next to the DLL, for `rip_sampler` and `debug-icd.ps1`; follow with `tools\build-icd.ps1 -SkipBuild` |
| `-Asserts` | keep Mesa's `assert()` checks |
| `-Pgo none` | build without the profile in `tools\pgo\hasvk.profdata` |
| `-Pgo generate` | instrumented build; `tools\dev\pgo-profile.ps1` runs the workloads (the command replay bench, vkcubepp, the Talos benchmark) and merges a new profile |

`tools\dev\debug-icd.ps1` runs a program under lldb. The LLVM archive has no
lldb: the script expects lldb 19.1.5 in `.tools\llvm` and Python 3.10 in
`.tools\python310-debug`, which `bootstrap.ps1` does not install.

## Tests

All tests run on the real GPU and need no interaction.

- `test-all.ps1` — the whole suite of hardware probes with byte-exact or
  pixel-exact comparison of results; `-SkipBuild` runs it on what is already
  built. Run it after every change.
- Single probes: `test-native.ps1` (WDDM transport), `test-elk.ps1`,
  `test-spirv.ps1`, `test-images.ps1`, `test-icd.ps1`, `test-graphics.ps1`,
  `test-wsi.ps1` (presentation), `test-oom.ps1`, `test-leaks.ps1`,
  `test-device-lost.ps1`, `test-kernel-limits.ps1`,
  `test-present-redirect.ps1`, `test-validation.ps1` (validation layers),
  `test-stability.ps1`.
- `test-flip.ps1` — presents by exclusive flip on the real screen (not part of
  `test-all.ps1`; needs `build-probes.ps1`): a Vulkan window covers the screen
  for about a minute in all, and what the display shows is read through
  Desktop Duplication and compared pixel by pixel (IMMEDIATE, FIFO, an sRGB
  swapchain), then the output is given back for a window shown above, and
  MAILBOX and a smaller window stay with the compositor. Run it after a
  change to presentation. `test-flip-exclusive.ps1` is the experiment behind
  it: the same kernel calls from the bare transport.
- `IrisIcdProbe.exe` (the probe of `test-icd.ps1` and of the installer) takes
  `ICD_PROBE_CYCLES=<n>` for more device creation cycles than its three, and
  `ICD_PROBE_FIRST_ONLY=1` to make a cycle nothing but the creation of the
  device and its first dispatch: a stress, not a test (its report says so).
  In that mode `ICD_PROBE_PRELUDE=empty` submits an empty command buffer first
  (a number sleeps that many milliseconds instead), and
  `ICD_PROBE_REGS=<hex offset>,...` reads those registers of the new context
  instead of dispatching (`ICD_PROBE_REGS_AFTER=1`: before and after the
  dispatch); the report counts every distinct set of values.
- `test-all.ps1` compares against control data captured from Intel's own driver
  by the C# prototype (`gpu-probe\artifacts`: saved shader code and allocation
  descriptors). That data is not in the repository, so the transport and
  compute controls need a local capture; `test-icd.ps1` and `test-graphics.ps1`
  are self-contained.
- Compact reports of the runs are in `native\artifacts`.

## Vulkan CTS

- `build-cts.ps1` builds `deqp-vk`; `run-cts.ps1 -Case 'dEQP-VK.…' -Name x
  -Jobs 2` runs a selection against the DLL in `build\icd`.
- Slices by group: `run-cts-sample.ps1`, `run-cts-groups.ps1`,
  `run-cts-vk13.ps1`, `run-cts-extensions.ps1`.
- `run-cts-full.ps1` runs the whole mustpass (2.89 million tests, 8–10 hours,
  resumable; progress in `build\cts-full\progress.log`).
- After a change to synchronization, depth, memory or pipelines, run the slices
  of the affected groups in addition to `test-all.ps1`.

## Benchmarks and profiling

- `run-dota.ps1` plays a Dota 2 replay as a timedemo under Vulkan or DX11
  (always with `-insecure`); `run-talos.ps1` runs the built-in benchmark of The
  Talos Principle. `-Sample` adds per-thread CPU time, CPU frequency and GPU
  load in the measured window, `-RipModule` adds sampled stacks.
- `build-probes.ps1` builds the micro-benchmarks, the `rip_sampler` profiler
  and a `d3d11.dll` proxy that breaks a DX11 frame down. With
  `D3D11_PROBE_DUMP=<prefix>` the D3D11 micro-benchmarks save the kernel that
  Intel's compiler produced; `IrisElkCompile.exe --disasm <file>` disassembles
  it (and any raw Gen8 code). `d3d11_flops_probe.exe` also saves the driver's
  command buffers that hold a dispatch (`<prefix>-batch-<n>.bin`); the depth
  fill and vertex probes of both drivers save the ones that hold a draw, and
  with `D3D11_PROBE_FIND=<hex dword>` every place where that dword occurs.
  `vs_fetch_probe.exe` takes a sixth argument that changes fixed-function
  state (cull mode, topology, primitive restart, ...); `depth_fill_probe.exe`
  one that chooses the depth clear (1.0 sampled, reverse Z 0.0, 1.0 not
  sampled) or adds an RGBA8 colour target (3, with `depth_fill_color.frag`;
  `d3d11_depth_fill_probe color` is its twin). `run-dota.ps1` and `run-talos.ps1` take `-D3D11Proxy`; for Dota 2
  the proxy needs `D3D11_PROXY_SEGMENTS=<n>` (no frame end is visible to it).
- Presentation: `test-flip-probe.ps1` runs a presenting program and reports
  the GPU load of every process meanwhile (what `dwm.exe` spends on composing
  it); `d3d11_flip_probe.exe` is a D3D11 window of the screen's size with a
  blt or flip swap chain or in DXGI's full-screen state; `screen_capture.exe`
  saves what the display shows (also while an output is owned exclusively,
  where GDI sees the desktop). `trace-d3d11-present.ps1 -Fullscreen` logs the
  kernel calls of DXGI's full-screen state (it cannot trace flip-model swap
  chains: its hook breaks `NtGdiDdDDIShareObjects`).
  `HASVK_WDDM_PRESENT_FLIP_DEBUG=1` makes the driver print when it takes the
  output and why it gives it back.
- `bench-frames.py` (frame tables), `talos-sections.py` (frame sections in
  milliseconds per API), `rip-compare.py` (code samples of two runs compared).
- Commands and measurement pitfalls are in [performance.md](performance.md).
- The driver's debug output (per-pass GPU times, frame timeline, frame capture)
  and its measurement switches are only in the `debug` branch.

Do not run the CTS, a game and a build at the same time: `build-icd.ps1`
overwrites the DLL in `build\icd`, and a build spoils any measurement.

## CI

`.github\workflows\build.yml` builds every push and pull request on a GitHub
hosted Windows runner: it checks that all scripts parse, runs `bootstrap.ps1`,
`build-icd.ps1` and `make-release.ps1`, uploads the package as an artifact and
checks that `THIRD-PARTY-NOTICES.md` matches the sources that were linked. The
runner has no Iris 6100, so nothing is executed on a GPU, and it uses its own
Visual Studio instead of the locked MSVC build.

## Releasing

1. `tools\build-icd.ps1`, then `tools\dev\test-all.ps1 -SkipBuild`.
2. If the Mesa version or the set of linked sources changed:
   `.tools\python\python.exe tools\gen-third-party-notices.py` and commit the
   result.
3. Commit, then `tools\make-release.ps1`: the package name carries the commit.
4. `tools\install-icd.ps1` from an administrator PowerShell installs it; the
   script ends with the `IrisIcdProbe` self-test through the system loader.
