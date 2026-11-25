# HasVK/WDDM: Vulkan 1.3 for the Intel Iris 6100 on Windows

An experimental Vulkan driver (ICD) built from Mesa HasVK. It runs on top of
Intel's stock WDDM kernel driver, without Intel's user-mode driver and without
D3D.

## Supported configuration

Exactly one:

| | |
|---|---|
| GPU | Intel Iris Graphics 6100 (Broadwell GT3, PCI `8086:162b`) |
| Intel driver | `20.19.15.5171` |
| OS | Windows 10/11, 64-bit |
| Applications | 64-bit |
| CPU | Broadwell (the one this GPU is part of): the driver is built with `-march=broadwell` and uses AVX2, BMI2 and FMA |

On another GPU or driver version the ICD finds no device and does nothing: the
undocumented interface of Intel's driver was taken from this version only.

## Features

- Vulkan 1.3, 108 device extensions (`vulkaninfo` lists them).
- Presentation to a window without a copy through the CPU; surface formats
  B8G8R8A8 UNORM and SRGB; FIFO, MAILBOX and IMMEDIATE present modes.
- Known CTS failures and their causes are in `cts-expected-failures.md`. The
  driver is not a conformant Vulkan implementation.

## Install

In PowerShell **as administrator**, in the unpacked package directory:

```powershell
.\tools\install-icd.ps1 -WhatIf     # show the steps, change nothing
.\tools\install-icd.ps1             # install
```

The script copies the driver to `%ProgramFiles%\HasVK-WDDM`, registers it in
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and checks the installed copy with the
`IrisIcdProbe` self-test through the system Vulkan loader. Drivers that are
already registered are left alone.

Without installing, the driver can be used for one application:

```powershell
$env:VK_DRIVER_FILES = "<package>\icd\intel_hasvk_wddm_icd.json"
& "<application>.exe"
```

## Uninstall

```powershell
.\tools\install-icd.ps1 -Uninstall
```

The shader cache (next section) is kept; delete its directory by hand if you
want.

## Shader cache

Compiled shaders are stored in `%LOCALAPPDATA%\HasVK-WDDM`:
`internal-cache.bin` holds the driver's own shaders (copies, clears) and
`default-cache.bin` holds all application pipelines, including those created
with the application's own `VkPipelineCache` (Mesa's disk cache does the same
on Linux; a game that never saves its cache, like Dota 2, would otherwise
compile everything on every start). The files are read when the device is
created and written when it is destroyed, at most 256 MiB each. After a driver
update the old contents are not used and the first run compiles shaders again;
the same happens with a different `INTEL_DEBUG` value. The directory can be
deleted at any time.

## Environment variables

| Variable | Effect |
|---|---|
| `HASVK_WDDM_INTERNAL_CACHE=0` | do not read or write the shader cache files |
| `HASVK_WDDM_PRESENT=gdi` | present through GDI with a copy through the CPU (a fallback) |
| `HASVK_WDDM_PRESENT_FLIP=0` | never take the output for a window that covers the screen: DWM composes it like any other window (see below) |
| `INTEL_PRECISE_TRIG=0` | turn off the `sin`/`cos` precision workaround (a multiplication by 0.99997 that keeps the result within [−1, 1]); on by default |

## A window that covers the screen

A game in "borderless window" mode at the screen's resolution (IMMEDIATE or
FIFO present mode) is shown without the desktop compositor: while its window
is in the foreground with nothing above it, the driver owns the display output
and flips the game's frames onto it, as an exclusive full-screen application
does. When another window is activated or appears above (Alt+Tab, a
notification, the volume indicator), the output goes back to the desktop
within a few hundredths of a second and the game is composed like any window
until it is alone again. What this changes:

- frames are not copied by DWM (in Dota 2 at 2560×1600 this is worth about a
  tenth of the frame rate);
- with IMMEDIATE the picture can tear, as that mode allows;
- screen capture through GDI (`BitBlt` of the screen) shows the desktop
  behind the game; Desktop Duplication and the game's own screenshots work.

`HASVK_WDDM_PRESENT_FLIP=0` turns this off.

## Limitations

- One graphics and compute queue; there is no separate transfer queue.
- Windows resets a GPU that works on one submission for more than 2 seconds
  (TDR); the application gets `VK_ERROR_DEVICE_LOST`.
- No sparse resources, no external memory and no semaphores shared between
  processes.
- Windows recovers from a GPU hang by itself, but on this version of Intel's
  kernel driver such a recovery has twice ended in a system bug check. Both
  causes of hangs that were found are fixed (`cts-expected-failures.md`).
  Packages up to `…-74e2287` hang the GPU on a fragment shader that writes
  depth with the `DepthUnchanged` execution mode; use a later one.

## License

The driver is distributed under the MIT License (`LICENSE`), without warranty
of any kind. It is built from Mesa 26.2.7 (also MIT) and contains zlib and
small third-party parts of Mesa under the Boost, BSD and Apache-2.0 licenses;
their copyright notices and license texts are in `THIRD-PARTY-NOTICES.md`. The
driver is not certified by Khronos and is not an Intel product.
