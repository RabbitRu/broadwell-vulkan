# Broadwell Vulkan: HasVK on WDDM for the Intel Iris 6100

An experimental Vulkan 1.3 driver (ICD) for Windows x64, built from Mesa HasVK
26.2.7. It runs on top of Intel's stock WDDM kernel driver `20.19.15.5171` on
the Intel Iris Graphics 6100 (Broadwell GT3, PCI `8086:162b`); neither Intel's
user-mode driver nor D3D is loaded into the process. On any other GPU or driver
version the ICD exposes no device.

![vkcube on the Iris 6100 through the HasVK/WDDM ICD](native/artifacts/vkcube-preview.png)

## Status

- Vulkan 1.3, 108 device extensions. The WDDM transport is the project's own;
  SPIR-V → NIR → ELK → Gen8, ISL, BLORP and the Vulkan runtime are stock Mesa.
- Presentation to a window without a copy through the CPU, in FIFO, MAILBOX and
  IMMEDIATE modes. A borderless window of the swapchain's size (a fullscreen
  game) is shown with no copy at all.
- Vulkan CTS 1.4.5.5: the full mustpass ran once (2,849,001 tests: 669,179
  Pass, 2,179,726 NotSupported, 90 not passed). After the fixes 89 of those 90
  pass and one failure is expected. The driver is not a conformant
  implementation. See [docs/cts-expected-failures.md](docs/cts-expected-failures.md).
- Applications that work: `vulkaninfo`, `vkcube`, `vkcubepp`, The Talos
  Principle, Dota 2.
- Performance against Intel's DX11 driver on the same machine
  ([docs/performance.md](docs/performance.md)):
  - The Talos Principle: 70–83 FPS against 79–87 (27.8 at the start of the
    work). The gap is the game's own Vulkan path; with shadow rendering off
    Vulkan is not slower.
  - Dota 2 (a replay as a timedemo): 28.1–28.4 FPS against 24.7–25.1 at
    2560×1600 (a window that covers the screen is shown without the desktop
    compositor), and 32.6–33.2 against 29.3–29.7 in full screen at 1280×800.
- A GPU hang on this version of Intel's kernel driver has twice ended in a
  Windows bug check. Both causes found were fixed and no CTS test is known to
  hang the GPU, but the risk of running an experimental driver is yours.

## Install

Download or build the release package and follow
[docs/install.md](docs/install.md) (it is the README of the package). To try
the driver with one application without installing it:

```powershell
$env:VK_DRIVER_FILES = "<package>\icd\intel_hasvk_wddm_icd.json"
& "<application>.exe"
```

## Build

About 25 GiB of free space is needed. From the repository root, in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\bootstrap.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-icd.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\make-release.ps1
```

`bootstrap.ps1` installs the MSVC build tools, the Windows SDK and Python and
downloads Mesa 26.2.7, clang 23.1.6 and zlib, all pinned by SHA-256.
`build-icd.ps1` builds the driver into `build\icd`; `make-release.ps1` packs it
into `build\release\HasVK-WDDM-<date>-<commit>.zip`.

Every push is also built by GitHub Actions
([.github/workflows/build.yml](.github/workflows/build.yml)); the package is
attached to the run as an artifact. That build uses the runner's Visual Studio
instead of the locked MSVC build and is not run on the GPU.

Toolchain details, tests and the rest of the development workflow are in
[docs/development.md](docs/development.md).

## Documentation

- [docs/install.md](docs/install.md) — installation, shader cache, environment
  variables, limitations.
- [docs/design.md](docs/design.md) — how the driver works: the WDDM transport,
  presentation, what was changed in Mesa, known risks.
- [docs/performance.md](docs/performance.md) — results in Talos and Dota 2,
  what made the difference, how to measure.
- [docs/cts-expected-failures.md](docs/cts-expected-failures.md) — CTS results
  and the bugs the CTS found.
- [docs/intel-interface.md](docs/intel-interface.md) — the undocumented Intel
  interface the port depends on and how to move to another driver version.
- [docs/development.md](docs/development.md) — toolchain, tests, CTS,
  benchmarks, profiling, CI.

## Repository layout

Needed to build the release:

| Path | Contents |
|---|---|
| `mesa/` | The Windows platform of HasVK (`anv_wddm_*`), the script that ports the pinned Mesa tree (`apply-compiler-port.py`) and the resulting patch |
| `native/src`, `native/include` | The WDDM transport |
| `tools/` | Bootstrap, build, packaging and installation scripts, the PGO profile |
| `.github/` | The CI build |

Needed only for development:

| Path | Contents |
|---|---|
| `tools/dev/` | Tests, CTS runners, game benchmarks, profiling and debugging scripts |
| `native/tests` | Hardware probes and micro-benchmarks (one of them, `IrisIcdProbe`, is the installer's self-test) |
| `native/artifacts` | Compact reports of test and CTS runs |
| `gpu-probe/` | The original C# prototype of direct GPU access |

`.tools`, `third_party` and `build` are local and not tracked; `bootstrap.ps1`
recreates the first two.

The driver in `main` has no measurement switches and no debug output. They are
kept in the `debug` branch together with the tooling that reads them (frame
timeline, per-pass GPU times, frame capture).

## License

The project's own code is under the MIT License ([LICENSE](LICENSE)). The parts
derived from Mesa stay under Mesa's license (also MIT) with their copyright
notices, see [NOTICE.md](NOTICE.md). Notices for the third-party code in the
binary package are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

This is an independent project: it is not an Intel product and is not certified
by Khronos. Vulkan is a registered trademark of the Khronos Group.
