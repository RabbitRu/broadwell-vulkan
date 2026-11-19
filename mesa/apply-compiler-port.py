"""Apply the bounded Windows Mesa library port to pinned sources.

This enables ELK/ISL/BLORP, Vulkan runtime, Win32 WSI and an experimental HasVK
hardware compute/storage-image ICD. Graphics and presentation remain disabled.
It does not substitute fake DRM responses for a working WDDM implementation.
"""
import difflib
import pathlib
import tarfile
import re
from compute_sources import generate_compute_sources
from graphics_sources import generate_graphics_sources

ROOT = pathlib.Path(__file__).resolve().parent.parent
MESA = ROOT / "third_party/mesa-26.2.7"
if (MESA / "VERSION").read_text().strip() != "26.2.7":
    raise SystemExit("Wrong Mesa version")
patches = []
originals = ROOT / '.tools/mesa-upstream-26.2.7'
names = {
    'meson.options', 'meson.build', 'src/intel/meson.build',
    'src/intel/compiler/meson.build', 'src/intel/dev/meson.build',
    'src/intel/dev/intel_debug.c', 'src/intel/dev/intel_device_info.c',
    'src/util/macros.h', 'src/util/u_atomic.h', 'include/drm-uapi/drm_fourcc.h',
    'src/intel/compiler/elk/elk_eu.c',
    'src/intel/compiler/elk/elk_eu_validate.c',
    'src/intel/compiler/elk/elk_fs_bank_conflicts.cpp',
    'src/intel/compiler/elk/elk_fs.cpp',
    'src/intel/mda/debug_archiver.c',
    'src/intel/compiler/elk/elk_eu_defines.h',
    'src/intel/compiler/elk/elk_reg_type.h',
    'src/intel/compiler/elk/elk_compiler.h',
    'src/intel/common/intel_measure.h', 'src/intel/blorp/blorp_clear.c',
    'src/intel/blorp/blorp_blit.c',
    'src/intel/isl/isl_priv.h', 'src/intel/isl/isl_drm.c', 'src/intel/isl/isl_gfx8.c',
    'src/vulkan/wsi/wsi_common_win32.cpp', 'src/vulkan/wsi/meson.build',
    'src/vulkan/wsi/wsi_common.c', 'src/vulkan/wsi/wsi_common.h', 'src/vulkan/wsi/wsi_common_private.h',
    'src/intel/vulkan_hasvk/anv_private.h',
    'src/intel/vulkan_hasvk/anv_allocator.c',
    'src/intel/vulkan_hasvk/anv_descriptor_set.c',
    'src/intel/vulkan_hasvk/anv_pipeline.c', 'src/intel/vulkan_hasvk/anv_pipeline_cache.c',
    'src/intel/vulkan_hasvk/genX_pipeline.c',
    'src/intel/vulkan_hasvk/anv_formats.c',
    'src/intel/vulkan_hasvk/anv_cmd_buffer.c', 'src/intel/vulkan_hasvk/genX_cmd_buffer.c',
    'src/intel/vulkan_hasvk/anv_batch_chain.c', 'src/intel/vulkan_hasvk/genX_state.c',
    'src/intel/vulkan_hasvk/anv_image.c',
    'src/intel/vulkan_hasvk/anv_blorp.c', 'src/intel/vulkan_hasvk/genX_blorp_exec.c',
    'src/intel/vulkan_hasvk/gfx8_cmd_buffer.c',
    'src/intel/vulkan_hasvk/anv_device.c',
    'src/intel/vulkan_hasvk/genX_query.c', 'src/intel/vulkan_hasvk/genX_gpu_memcpy.c',
}
missing = {name for name in names if not (originals / name).exists()}
if missing:
    # One streaming decompression pass instead of seeking through xz for every
    # edited file. Original contents remain separate from the working tree.
    with tarfile.open(ROOT / '.tools/downloads/mesa-26.2.7.tar.xz', 'r|xz') as archive:
        for member in archive:
            name = member.name.removeprefix('mesa-26.2.7/')
            if name in missing:
                target = originals / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
                missing.remove(name)
                if not missing:
                    break
    if missing:
        raise ValueError(f'Missing upstream files: {missing}')


def edit(name, replacements):
    path = MESA / name
    # Always derive the complete patch from the pinned upstream archive. This
    # also makes repeated bootstrap/build calls idempotent.
    before = (originals / name).read_text(encoding='utf-8')
    after = before
    for old, new, *count in replacements:
        # An optional third item: how many identical occurrences to replace.
        expected = count[0] if count else 1
        if new in after:
            continue
        if after.count(old) != expected:
            raise ValueError(f"Unexpected source context: {name}: {old[:80]!r}")
        after = after.replace(old, new)
    if after != before:
        if path.read_text(encoding="utf-8") != after:
            path.write_text(after, encoding="utf-8", newline="\n")
        patches.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), f"a/{name}", f"b/{name}"))


edit("meson.options", [("option(\n  'split-debug',", "option('hasvk-wddm-compiler-only', type : 'boolean', value : false,\n       description : 'Windows WDDM libraries and experimental hardware ICD; incomplete Vulkan')\noption('hasvk-wddm-graphics', type : 'boolean', value : false,\n       description : 'Experimental Windows HasVK graphics modules')\n\noption(\n  'split-debug',")])
edit("meson.build", [
    ("with_gallium_iris, with_gallium_crocus, with_intel_vk, with_intel_hasvk,\n  with_gallium_asahi,", "with_gallium_iris, with_gallium_crocus, with_intel_vk,\n  with_intel_hasvk and not get_option('hasvk-wddm-compiler-only'),\n  with_gallium_asahi,"),
    ("elif with_intel_vk or with_intel_hasvk\n  error('Intel", "elif (with_intel_vk or with_intel_hasvk) and not get_option('hasvk-wddm-compiler-only')\n  error('Intel"),
])
edit("src/intel/meson.build", [
    ("subdir('isl')\nsubdir('common')\nsubdir('compiler')", "if get_option('hasvk-wddm-compiler-only')\n  if host_machine.system() != 'windows'\n    error('The isolated WDDM compiler build is Windows-only')\n  endif\n  subdir('isl')\n  subdir('compiler')\n  subdir('wddm_compiler')\nelse\nsubdir('isl')\nsubdir('common')\nsubdir('compiler')"),
    ("if with_intel_hasvk\n  subdir('vulkan_hasvk')\nendif", "if with_intel_hasvk\n  subdir('vulkan_hasvk')\nendif\nendif # not hasvk-wddm-compiler-only"),
])
edit("src/intel/compiler/meson.build", [
    ("subdir('gen')\nsubdir('brw')\nsubdir('jay')", "if not get_option('hasvk-wddm-compiler-only')\n  subdir('gen')\n  subdir('brw')\n  subdir('jay')\nendif"),
])
edit("src/intel/dev/meson.build", [
    ("inc_intel_dev_virtio = []", "if get_option('hasvk-wddm-compiler-only')\n  files_libintel_dev = files('intel_debug.c', 'intel_device_info.c')\nendif\n\ninc_intel_dev_virtio = []"),
])
edit("src/intel/dev/intel_debug.c", [
    ('#include "util/u_math.h"', '#include "util/u_math.h"\n#include "util/u_string.h"'),
])
edit('src/intel/common/intel_measure.h', [
    ('#include <pthread.h>', '#ifdef _WIN32\n#include "util/simple_mtx.h"\n#else\n#include <pthread.h>\n#endif'),
    ('   pthread_mutex_t mutex;', '#ifdef _WIN32\n   simple_mtx_t mutex;\n#else\n   pthread_mutex_t mutex;\n#endif'),
])
edit('src/intel/isl/isl_priv.h', [
    ('#include <strings.h>', '#ifndef _WIN32\n#include <strings.h>\n#endif'),
])
edit('src/intel/isl/isl_drm.c', [
    ('#include "drm-uapi/i915_drm.h"', '#ifndef _WIN32\n#include "drm-uapi/i915_drm.h"\n#else\n/* Tiling identifiers for pure layout conversion, no DRM calls. */\n#define I915_TILING_NONE 0\n#define I915_TILING_X 1\n#define I915_TILING_Y 2\n#endif'),
])
edit('src/vulkan/wsi/wsi_common_win32.cpp', [
    ('#include <directx/d3d12.h>\n#include <dxguids/dxguids.h>',
     '#ifdef HASVK_WDDM_GDI\n/* The isolated WDDM build uses the pinned Windows SDK interfaces. */\n#include <d3d12.h>\n#include <dwmapi.h>\n#else\n#include <directx/d3d12.h>\n#include <dxguids/dxguids.h>\n#endif'),
    ('   if (!wsi_device->sw) {\n      wsi->dxgi.factory = dxgi_get_factory',
     '   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {\n      wsi->dxgi.factory = dxgi_get_factory'),
    # The WDDM port's redirected present converts any of these into the
    # window's B8G8R8A8 surface (copy or channel-swapping blit, sRGB bytes kept).
    ('   { VK_FORMAT_R8G8B8A8_UNORM },\n   { VK_FORMAT_B8G8R8A8_SRGB },',
     '   { VK_FORMAT_R8G8B8A8_UNORM },\n   { VK_FORMAT_B8G8R8A8_SRGB },\n#ifdef HASVK_WDDM_GDI\n   { VK_FORMAT_R8G8B8A8_SRGB },\n#endif'),
    # FIFO is paced by DWM composition (DwmFlush after each present); MAILBOX
    # and IMMEDIATE are not: DWM shows the latest surface content.
    ('static const VkPresentModeKHR present_modes_gdi[] = {\n   VK_PRESENT_MODE_FIFO_KHR,\n};',
     'static const VkPresentModeKHR present_modes_gdi[] = {\n   VK_PRESENT_MODE_FIFO_KHR,\n#ifdef HASVK_WDDM_GDI\n'
     '   VK_PRESENT_MODE_MAILBOX_KHR,\n   VK_PRESENT_MODE_IMMEDIATE_KHR,\n#endif\n};'),
    # Redirected present (no CPU copy, no D3D): the driver copies the image into
    # a shared window surface and submits with the present token. GDI remains
    # the fallback when the driver hook fails.
    ('   HWND wnd;\n   HDC chain_dc;\n',
     '   HWND wnd;\n   HDC chain_dc;\n   void *redirect_state;\n   uint64_t redirected_presents, gdi_presents;\n'
     '   bool rgba_order;   /* R8G8B8A8 images: GDI needs R and B swapped */\n'),
    # VK_KHR_win32_surface: "minImageExtent, maxImageExtent, and currentExtent
    # must always equal the window size" (upstream advertises 1..16384, which
    # lets applications create swapchains the present then reports out of date).
    ('''   caps->surfaceCapabilities.minImageExtent = { 1u, 1u };
   caps->surfaceCapabilities.maxImageExtent = {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };
''', '''#ifdef HASVK_WDDM_GDI
   caps->surfaceCapabilities.minImageExtent = caps->surfaceCapabilities.currentExtent;
   caps->surfaceCapabilities.maxImageExtent = caps->surfaceCapabilities.currentExtent;
#else
   caps->surfaceCapabilities.minImageExtent = { 1u, 1u };
   caps->surfaceCapabilities.maxImageExtent = {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };
#endif
'''),
    # A zero extent (an application recreating while minimized; invalid usage)
    # would assert in image creation: report the surface as out of date.
    ('''   const unsigned num_images = create_info->minImageCount;
   struct wsi_win32_swapchain *chain;''', '''#ifdef HASVK_WDDM_GDI
   if (!create_info->imageExtent.width || !create_info->imageExtent.height)
      return VK_ERROR_OUT_OF_DATE_KHR;
#endif
   const unsigned num_images = create_info->minImageCount;
   struct wsi_win32_swapchain *chain;'''),
    ('   chain->extent = create_info->imageExtent;\n',
     '   chain->extent = create_info->imageExtent;\n'
     '   chain->rgba_order = create_info->imageFormat == VK_FORMAT_R8G8B8A8_UNORM ||\n'
     '                       create_info->imageFormat == VK_FORMAT_R8G8B8A8_SRGB;\n'
     '#ifdef HASVK_WDDM_GDI\n'
     '   chain->base.present_cmd = wsi_win32_present_cmd;\n'
     '#endif\n'),
    # A window whose client area no longer matches the swapchain: acquire
    # reports OUT_OF_DATE (as other Windows drivers do), present SUBOPTIMAL.
    ('''   /* acquire timeout has to be explicitly handled for sw wsi */
   if (!chain->dxgi)
      return wsi_win32_acquire_idle_cpu_image(chain, info, image_index);
''', '''   /* acquire timeout has to be explicitly handled for sw wsi */
   if (!chain->dxgi) {
#ifdef HASVK_WDDM_GDI
      if (!wsi_win32_extent_matches(chain))
         return VK_ERROR_OUT_OF_DATE_KHR;
#endif
      return wsi_win32_acquire_idle_cpu_image(chain, info, image_index);
   }
'''),
    ('''static bool
wsi_win32_find_idle_image(''', '''#ifdef HASVK_WDDM_GDI
/* The driver's window copy of image_index for this present, or VK_NULL_HANDLE
 * (the frame then goes through GDI). */
static VkCommandBuffer
wsi_win32_present_cmd(struct wsi_swapchain *drv_chain, VkQueue queue, uint32_t image_index)
{
   struct wsi_win32_swapchain *chain = (struct wsi_win32_swapchain *)drv_chain;
   if (!chain->wsi->wsi->win32.present_redirected_record)
      return VK_NULL_HANDLE;
   return chain->wsi->wsi->win32.present_redirected_record(chain->base.device, queue,
                                                          chain->images[image_index].base.image, image_index,
                                                          chain->surface->base.hwnd, chain->extent,
                                                          &chain->redirect_state);
}
/* Client area size equals the swapchain extent. A minimized window (0x0) is
 * not a mismatch: the swapchain stays usable until the window is restored, so
 * applications are not pushed into recreating it with a zero extent. */
static bool
wsi_win32_extent_matches(struct wsi_win32_swapchain *chain)
{
   RECT client;
   if (!GetClientRect(chain->surface->base.hwnd, &client))
      return false;
   const uint32_t width = client.right - client.left, height = client.bottom - client.top;
   const bool matches = (!width && !height) || (width == chain->extent.width && height == chain->extent.height);
   if (!matches && getenv("HASVK_WDDM_PRESENT_DEBUG"))
      fprintf(stderr, "HasVK/WDDM WSI: client %ux%u, swapchain %ux%u\\n", width, height, chain->extent.width, chain->extent.height);
   return matches;
}

/* FIFO pacing: sleep until the next vertical blank, from DWM's composition
 * timing (last vblank and refresh period). Unlike DwmFlush this does not
 * depend on DWM composing right now: with the display off DwmFlush stalled
 * for ~270 ms per frame. Without timing information, 60 Hz is assumed. */
static void
wsi_win32_wait_composition(void)
{
   typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO *);
   static PFN_DwmGetCompositionTimingInfo get_timing;
   static HANDLE timer;
   static bool loaded;
   if (!loaded) {
      HMODULE dwm = LoadLibraryA("dwmapi.dll");
      get_timing = dwm ? (PFN_DwmGetCompositionTimingInfo)(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo") : NULL;
      timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
      if (!timer)
         timer = CreateWaitableTimerW(NULL, TRUE, NULL);
      loaded = true;
   }
   LARGE_INTEGER frequency, now;
   QueryPerformanceFrequency(&frequency);
   QueryPerformanceCounter(&now);
   int64_t period = frequency.QuadPart / 60, next = now.QuadPart + period;
   DWM_TIMING_INFO info;
   memset(&info, 0, sizeof(info));
   info.cbSize = sizeof(info);
   if (get_timing && SUCCEEDED(get_timing(NULL, &info)) && info.qpcRefreshPeriod &&
       info.qpcRefreshPeriod < (UINT64)frequency.QuadPart / 20) {
      period = (int64_t)info.qpcRefreshPeriod;
      next = (int64_t)info.qpcVBlank;
      if (next <= now.QuadPart)
         next += ((now.QuadPart - next) / period + 1) * period;
   }
   const int64_t wait = next - now.QuadPart;
   if (wait <= 0 || wait > 2 * period)
      return;
   if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -(wait * 10000000 / frequency.QuadPart);   /* relative, 100 ns */
      if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
         WaitForSingleObject(timer, INFINITE);
         return;
      }
   }
   Sleep((DWORD)(wait * 1000 / frequency.QuadPart));
}
#endif

static bool
wsi_win32_find_idle_image('''),
    ('''   char *ptr = (char *)image->base.cpu_map;
   char *dptr = (char *)image->sw.ppvBits;
''', '''#ifdef HASVK_WDDM_GDI
   const bool extent_matches = wsi_win32_extent_matches(chain);
#endif
   if (image->base.redirected) {
      /* The window copy and the present went with WSI's present submission. */
      ++chain->redirected_presents;
      wsi_win32_set_image_idle(chain, image);
#ifdef HASVK_WDDM_GDI
      if (chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR)
         wsi_win32_wait_composition();
      if (chain->status == VK_SUCCESS && !extent_matches)
         return VK_SUBOPTIMAL_KHR;
#endif
      return chain->status;
   }
   ++chain->gdi_presents;

   char *ptr = (char *)image->base.cpu_map;
   char *dptr = (char *)image->sw.ppvBits;
'''),
    ('''   for (unsigned h = 0; h < chain->extent.height; h++) {
      memcpy(dptr, ptr, chain->extent.width * 4);
      dptr += image->sw.bmp_row_pitch;
      ptr += image->base.row_pitches[0];
   }''', '''   for (unsigned h = 0; h < chain->extent.height; h++) {
#ifdef HASVK_WDDM_GDI
      if (chain->rgba_order) {
         const uint32_t *in = (const uint32_t *)ptr;
         uint32_t *out = (uint32_t *)dptr;
         for (unsigned x = 0; x < chain->extent.width; x++)
            out[x] = (in[x] & 0xff00ff00u) | ((in[x] & 0xffu) << 16) | ((in[x] >> 16) & 0xffu);
      } else
#endif
      memcpy(dptr, ptr, chain->extent.width * 4);
      dptr += image->sw.bmp_row_pitch;
      ptr += image->base.row_pitches[0];
   }'''),
    ('''      chain->status = VK_ERROR_MEMORY_MAP_FAILED;

   wsi_win32_set_image_idle(chain, image);

   return chain->status;''', '''      chain->status = VK_ERROR_MEMORY_MAP_FAILED;

   wsi_win32_set_image_idle(chain, image);
#ifdef HASVK_WDDM_GDI
   /* The fallback (display off, minimized) paces FIFO the same way. */
   if (chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR)
      wsi_win32_wait_composition();
   if (chain->status == VK_SUCCESS && !extent_matches)
      return VK_SUBOPTIMAL_KHR;
#endif

   return chain->status;'''),
    ('''   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);

   DeleteDC(chain->chain_dc);
''', '''   /* First: the driver's last window copy still reads a swapchain image. */
   if (chain->redirect_state && chain->wsi->wsi->win32.present_redirected_finish)
      chain->wsi->wsi->win32.present_redirected_finish(chain->base.device, chain->redirect_state);

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_win32_image_finish(chain, allocator, &chain->images[i]);

   DeleteDC(chain->chain_dc);
'''),
    # The window DC comes from GetDC: one per swapchain (upstream took one per
    # image and kept the last) and released with ReleaseDC, not DeleteDC
    # (tools/test-leaks.ps1: 2 GDI objects lost per swapchain).
    ('   chain->chain_dc = GetDC(chain->wnd);\n',
     '   if (!chain->chain_dc)\n      chain->chain_dc = GetDC(chain->wnd);\n'),
    ('''   DeleteDC(chain->chain_dc);

   if (chain->surface->current_swapchain == chain)''', '''   if (chain->chain_dc)
      ReleaseDC(chain->wnd, chain->chain_dc);

   if (getenv("HASVK_WDDM_PRESENT_STATS"))
      fprintf(stderr, "HasVK/WDDM present stats: %llu redirected, %llu gdi\\n",
              (unsigned long long)chain->redirected_presents, (unsigned long long)chain->gdi_presents);

   if (chain->surface->current_swapchain == chain)'''),
])
edit('src/vulkan/wsi/wsi_common.h', [
    ('''      VkResult (*create_image_memory)(VkDevice device, void *resource,
                                      const VkAllocationCallbacks *alloc,
                                      VkDeviceMemory *out);
''', '''      VkResult (*create_image_memory)(VkDevice device, void *resource,
                                      const VkAllocationCallbacks *alloc,
                                      VkDeviceMemory *out);
      /* Records the copy of swapchain image index into window hwnd for a
       * present without a CPU copy; WSI submits it in its present submission
       * on queue. *state is per swapchain, owned by the driver and released by
       * present_redirected_finish. VK_NULL_HANDLE: this frame goes to GDI. */
      VkCommandBuffer (*present_redirected_record)(VkDevice device, VkQueue queue, VkImage image,
                                                   uint32_t index, void *hwnd, VkExtent2D extent,
                                                   void **state);
      void (*present_redirected_finish)(VkDevice device, void *state);
'''),
])
edit('src/vulkan/wsi/wsi_common_private.h', [
    ('   struct {\n      enum wsi_swapchain_blit_type type;\n',
     '   /* Set while presents need no CPU-visible copy (redirected present on\n'
     '    * the WDDM port); the buffer blit is then skipped. */\n'
     '   bool blit_unneeded;\n'
     '   /* WDDM port: the platform\'s window copy for image_index, submitted in\n'
     '    * the present submission; VK_NULL_HANDLE when the frame goes to GDI. */\n'
     '   VkCommandBuffer (*present_cmd)(struct wsi_swapchain *swap_chain, VkQueue queue,\n'
     '                                  uint32_t image_index);\n\n'
     '   struct {\n      enum wsi_swapchain_blit_type type;\n'),
    ('   bool acquired;\n   uint64_t present_serial;\n',
     '   bool acquired;\n   uint64_t present_serial;\n'
     '   /* WDDM port: this present\'s window copy went into the submission. */\n'
     '   bool redirected;\n'),
])
edit('src/vulkan/wsi/wsi_common.c', [
    ('''         if (swapchain->blit.type != WSI_SWAPCHAIN_NO_BLIT) {
            command_buffer_infos[command_buffer_count++] = (VkCommandBufferSubmitInfo) {''',
     '''         image->redirected = false;
         if (swapchain->present_cmd) {
            /* WDDM port: the window copy rides in this submission, after the
             * application's work on the in-order queue: no CPU wait and no
             * extra submission. Only a GDI frame needs the CPU copy. */
            VkCommandBuffer redirect =
               swapchain->present_cmd(swapchain, vk_queue_to_handle(queue), image_index);
            image->redirected = redirect != VK_NULL_HANDLE;
            swapchain->blit_unneeded = image->redirected;
            if (redirect) {
               command_buffer_infos[command_buffer_count++] = (VkCommandBufferSubmitInfo) {
                  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                  .commandBuffer = redirect,
               };
            }
         }

         if (swapchain->blit.type != WSI_SWAPCHAIN_NO_BLIT && !swapchain->blit_unneeded) {
            command_buffer_infos[command_buffer_count++] = (VkCommandBufferSubmitInfo) {'''),
    ('      if (wsi->sw) {\n         wsi->WaitForFences',
     '''#ifdef HASVK_WDDM_GDI
      /* A GDI frame shows the CPU copy: wait for it (redirected frames do not). */
      if (!image->redirected) {
#else
      if (wsi->sw) {
#endif
         wsi->WaitForFences'''),
])
edit('src/vulkan/wsi/meson.build', [
    ('libvulkan_wsi = static_library(',
     "wddm_gdi_args = []\nif get_option('hasvk-wddm-compiler-only')\n  wddm_gdi_args = ['-DHASVK_WDDM_GDI']\nendif\n\nlibvulkan_wsi = static_library("),
    ('  link_with: links_vulkan_wsi,', '  link_with: links_vulkan_wsi,\n  cpp_args : wddm_gdi_args,\n  c_args : wddm_gdi_args,'),
])
edit('src/intel/vulkan_hasvk/anv_private.h', [
    ('#include <pthread.h>', '#ifdef _WIN32\n#include "anv_wddm_compat.h"\n#else\n#include <pthread.h>\n#endif'),
    ('#include "drm-uapi/i915_drm.h"', '#ifndef _WIN32\n#include "drm-uapi/i915_drm.h"\n#endif'),
    ('#include "common/intel_gem.h"', '#ifndef _WIN32\n#include "common/intel_gem.h"\n#endif'),
    ('   return image->vk.drm_format_mod != DRM_FORMAT_MOD_INVALID ||\n          image->vk.external_handle_types != 0;', '#ifndef _WIN32\n   return image->vk.drm_format_mod != DRM_FORMAT_MOD_INVALID ||\n          image->vk.external_handle_types != 0;\n#else\n   return image->vk.external_handle_types != 0;\n#endif'),
    ('struct anv_device {\n    struct vk_device                            vk;', 'struct anv_device {\n    struct vk_device                            vk;\n#ifdef _WIN32\n    struct anv_wddm_device_state *wddm;\n#endif'),
    ('struct anv_bo {\n', 'struct anv_bo {\n#ifdef _WIN32\n   struct hasvk_wddm_bo *wddm_bo;\n#endif\n'),
    ('struct anv_cmd_buffer {\n   struct vk_command_buffer                     vk;', 'struct anv_cmd_buffer {\n   struct vk_command_buffer                     vk;\n#ifdef _WIN32\n   struct anv_wddm_cmd_state *wddm;\n#endif'),
    ('   enum isl_format isl_format:16;', '#if defined(_WIN32) && defined(__clang__)\n   unsigned isl_format:16;\n#else\n   enum isl_format isl_format:16;\n#endif'),
    ('struct anv_device_memory {\n   struct vk_object_base                        base;', 'struct anv_device_memory {\n   struct vk_object_base                        base;\n#ifdef _WIN32\n   uint64_t wddm_allocation_size;\n#endif'),
    ('#define anv_genX(devinfo, thing) ({', '#ifdef _WIN32\n/* The WDDM adapter gate accepts only Gen8. */\n#define anv_genX(devinfo, thing) (&gfx8_##thing)\n#else\n#define anv_genX(devinfo, thing) ({'),
    ('})\n\n/* Gen-specific function declarations */', '})\n#endif\n\n/* Gen-specific function declarations */'),
    # HASVK_WDDM_PROFILE_PASSES: pipe flush/stall requests counted by reason.
    ('''   cmd_buffer->state.pending_pipe_bits |= bits;
   if (INTEL_DEBUG(DEBUG_PIPE_CONTROL) && bits)''', '''   cmd_buffer->state.pending_pipe_bits |= bits;
#ifdef _WIN32
   if (anv_wddm_profile_pipe_bits_enabled && bits) anv_wddm_profile_pipe_bits(bits, reason);
#endif
   if (INTEL_DEBUG(DEBUG_PIPE_CONTROL) && bits)'''),
    ('   if (devinfo->ver != 8 && !devinfo->has_sample_with_hiz)\n      return false;\n',
     '   if (devinfo->ver != 8 && !devinfo->has_sample_with_hiz)\n      return false;\n'
     '#ifdef _WIN32\n   /* HASVK_WDDM_NO_HIZ_SAMPLING=1: depth is resolved before sampling instead\n'
     '    * of sampled with HiZ, which allows HiZ fast clears on Gen8\n'
     '    * (anv_can_hiz_clear_ds_view). Faster when alpha-tested geometry is drawn\n'
     '    * first after a clear (docs/optimizations-vs-dx11.md, 2.10); costs a\n'
     '    * resolve per sampled depth pass, so it is not the default. */\n'
     '   static int no_hiz_sampling = -1;\n'
     '   if (no_hiz_sampling < 0) no_hiz_sampling = getenv("HASVK_WDDM_NO_HIZ_SAMPLING") != NULL;\n'
     '   if (no_hiz_sampling) return false;\n#endif\n'),
    ('static inline void\nanv_add_pending_pipe_bits(', '''#ifdef _WIN32
extern bool anv_wddm_profile_pipe_bits_enabled;
void anv_wddm_profile_pipe_bits(uint32_t bits, const char *reason);
#endif
static inline void
anv_add_pending_pipe_bits('''),
])
# Keep Mesa's lock-free state/block allocators and free lists. Only the Linux
# mmap backing and GEM BO implementation are replaced on Windows.
allocator_name = 'src/intel/vulkan_hasvk/anv_allocator.c'
allocator_original = (originals / allocator_name).read_text(encoding='utf-8')
allocator_replacements = [
    ('p_atomic_cmpxchg(&pool->bos[scratch_size_log2][stage], NULL, bo)',
     'p_atomic_cmpxchg_ptr(&pool->bos[scratch_size_log2][stage], NULL, bo)'),
    # Block-pool maps take a BO-relative offset; state offsets include the
    # general/negative binding-table pool bias. Normalize at the boundary.
    ('                                        state_i->offset,',
     '#ifdef _WIN32\n                                        state_i->offset - pool->start_offset,\n#else\n                                        state_i->offset,\n#endif'),
    ('         int32_t chunk_offset = state->offset;',
     '         int32_t chunk_offset = state->offset;\n#ifdef _WIN32\n         chunk_offset -= pool->start_offset;\n#endif'),
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif'),
    ('#include <sys/mman.h>', '#ifndef _WIN32\n#include <sys/mman.h>\n#endif'),
    ('#include "util/anon_file.h"', '#ifndef _WIN32\n#include "util/anon_file.h"\n#endif'),
    ('#define ANV_STATE_ENTRY_SIZE (sizeof(struct anv_free_entry))',
     '#define ANV_STATE_ENTRY_SIZE (sizeof(struct anv_free_entry))\n#ifdef _WIN32\n#include "anv_wddm_allocator.h"\n#endif'),
]
for function in ['anv_state_table_init', 'anv_state_table_expand_range', 'anv_state_table_finish',
                 'anv_block_pool_init', 'anv_block_pool_expand_range', 'anv_block_pool_finish']:
    match = re.search(r'\n(?:static )?(?:VkResult|void)\n' + function + r'\([^;{]*\)\n\{', allocator_original)
    if not match:
        raise ValueError(f'Missing upstream allocator function: {function}')
    end = allocator_original.index('\n}\n', match.end()) + 3
    body = allocator_original[match.start() + 1:end]
    allocator_replacements.append((body, '#ifndef _WIN32\n' + body + '\n#endif'))
gem_allocator = allocator_original[allocator_original.index('#define ANV_BO_CACHE_SUPPORTED_FLAGS'):]
allocator_replacements.append((gem_allocator, '#ifndef _WIN32\n' + gem_allocator + '\n#endif\n'))
# A block pool that cannot grow (out of memory) is retried forever upstream:
# the application freezes inside vkCmd* when memory does not come back.
# Retrying does help with a transient failure (CTS simulate_oom fails single
# host allocations), so a failure is retried 64 times, 1 ms apart (os_time.h,
# included below); callers
# have no error path, so after that the process ends with the cause.
# Application memory no longer competes with these pools
# (anv_wddm_memory.c): this takes real exhaustion of the driver's own budget
# or of the system.
allocator_replacements.append(('''         do {
            new.end = anv_block_pool_grow(pool, pool_state, block_size);
         } while (new.end < new.next);''', '''#ifdef _WIN32
         unsigned grow_failures = 0;
#endif
         do {
            new.end = anv_block_pool_grow(pool, pool_state, block_size);
#ifdef _WIN32
            if (new.end == 0) {
               if (++grow_failures == 64) {
                  fprintf(stderr, "HasVK/WDDM: out of memory growing the %s pool (%u bytes); cannot continue\\n",
                          pool->name, pool->size);
                  abort();
               }
               if (grow_failures > 1) os_time_sleep(1000);
            }
#endif
         } while (new.end < new.next);'''))
allocator_replacements.append(('#include "util/futex.h"', '#include "util/futex.h"\n#include "util/os_time.h"'))
edit(allocator_name, allocator_replacements)
edit('src/intel/vulkan_hasvk/anv_descriptor_set.c', [
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif'),
])
pipeline_name = 'src/intel/vulkan_hasvk/anv_pipeline.c'
pipeline_original = (originals / pipeline_name).read_text(encoding='utf-8')
pipeline_replacements = [
    # The compute-only build keeps its verified 1D/64-invocation gate; the
    # graphics build accepts everything upstream does.
    ('   anv_genX(device->info, compute_pipeline_emit)(pipeline);', '''#if defined(_WIN32) && !defined(HASVK_WDDM_GRAPHICS)
   const struct elk_cs_prog_data *program = get_cs_prog_data(pipeline);
   if (program->local_size[0] > 64 || program->local_size[1] != 1 || program->local_size[2] != 1 ||
       program->base.total_scratch || program->base.total_shared) {
      anv_pipeline_finish(&pipeline->base, device, pAllocator);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      *pPipeline = VK_NULL_HANDLE;
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
   }
#endif
   anv_genX(device->info, compute_pipeline_emit)(pipeline);'''),
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif\n#include "util/memstream.h"'),
    ('      FILE *stream = open_memstream(&stream_data, &stream_size);',
     '      struct u_memstream memstream;\n      if (!u_memstream_open(&memstream, &stream_data, &stream_size)) return;\n      FILE *stream = u_memstream_get(&memstream);'),
    ('      fclose(stream);', '      u_memstream_close(&memstream);'),
]
for function in ['anv_graphics_pipeline_init', 'anv_graphics_pipeline_create', 'anv_CreateGraphicsPipelines']:
    match = re.search(r'\n(?:static\s+)?VkResult\s+' + function + r'\([^;{]*\)\n\{', pipeline_original)
    if not match:
        raise ValueError(f'Missing upstream graphics function: {function}')
    end = pipeline_original.index('\n}\n', match.end()) + 3
    body = pipeline_original[match.start() + 1:end]
    pipeline_replacements.append((body, '#if !defined(_WIN32) || defined(HASVK_WDDM_GRAPHICS)\n' + body + '\n#endif'))
edit(pipeline_name, pipeline_replacements)
# Upstream HasVK images almost always get HALIGN_16 because they may own an
# auxiliary surface. This port disables aux for every image, which selects
# HALIGN_4; on Broadwell the sampler then returns permuted texels for mip levels
# of 8/16-bit formats that do not start on a 16-byte column (CTS
# api.image_clearing 200x180). Keep the alignment upstream effectively uses.
edit('src/intel/isl/isl_gfx8.c', [
    ('   uint32_t halign = 4;\n', '#ifdef _WIN32\n   uint32_t halign = 16;\n#else\n   uint32_t halign = 4;\n#endif\n'),
])
edit('src/intel/vulkan_hasvk/anv_pipeline_cache.c', [
    ('   uint64_t shader_data_addr = INSTRUCTION_STATE_POOL_MIN_ADDRESS +',
     '#ifdef _WIN32\n   uint64_t shader_data_addr = device->instruction_state_pool.block_pool.start_address -\n                               device->instruction_state_pool.start_offset +\n#else\n   uint64_t shader_data_addr = INSTRUCTION_STATE_POOL_MIN_ADDRESS +\n#endif'),
])
image_name = 'src/intel/vulkan_hasvk/anv_image.c'
image_before = (originals / image_name).read_text(encoding='utf-8')
image_after = image_before.replace('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif')
image_after = image_after.replace('#include <sys/mman.h>', '#ifndef _WIN32\n#include <sys/mman.h>\n#endif')
image_after = image_after.replace('image->vk.drm_format_mod', 'anv_image_modifier(image)')
image_after = image_after.replace('      anv_image_modifier(image) = isl_mod_info->modifier;',
    '#ifndef _WIN32\n      image->vk.drm_format_mod = isl_mod_info->modifier;\n#endif')
image_after = image_after.replace('#define ANV_OFFSET_IMPLICIT UINT64_MAX', '''#define ANV_OFFSET_IMPLICIT UINT64_MAX
static inline uint64_t anv_image_modifier(const struct anv_image *image)
{
#ifdef _WIN32
   /* No external/modifier images are accepted by this Windows path. */
   return DRM_FORMAT_MOD_INVALID;
#else
   return image->vk.drm_format_mod;
#endif
}''')
# The compute-only build keeps its three-format gate. With graphics, image
# creation is upstream; only auxiliary surfaces (HiZ/CCS/MCS) stay disabled.
image_after = image_after.replace('   vk_image_init(&device->vk, &image->vk, pCreateInfo);', '''#ifdef _WIN32
#ifndef HASVK_WDDM_GRAPHICS
   if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D || pCreateInfo->format != VK_FORMAT_R8G8B8A8_UNORM ||
       (pCreateInfo->flags & ~VK_IMAGE_CREATE_ALIAS_BIT) || pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1 ||
       pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT || pCreateInfo->extent.depth != 1 ||
       !pCreateInfo->extent.width || !pCreateInfo->extent.height ||
       pCreateInfo->extent.width > 4096 || pCreateInfo->extent.height > 4096 ||
       (pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR && pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL) ||
       (pCreateInfo->usage & ~VK_IMAGE_USAGE_STORAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
#endif
#ifndef HASVK_WDDM_GRAPHICS
   create_info->isl_extra_usage_flags |= ISL_SURF_USAGE_DISABLE_AUX_BIT;
#else
   /* Graphics: upstream's HiZ and MCS (CCS is off, see anv_CreateInstance).
    * HASVK_WDDM_NO_AUX=1 disables every auxiliary surface (investigation). */
   if (getenv("HASVK_WDDM_NO_AUX")) create_info->isl_extra_usage_flags |= ISL_SURF_USAGE_DISABLE_AUX_BIT;
#endif
#endif
   vk_image_init(&device->vk, &image->vk, pCreateInfo);''')
# HiZ for depth attachments. Upstream hasvk computes the HiZ surface and then
# drops it: enabling it (aux_usage + the surface itself) went away together with
# HiZ+CCS (Gfx9+), while every HiZ path (WM_HZ_OP clears, depth/HiZ resolves at
# layout transitions, 3DSTATE_HIER_DEPTH_BUFFER, the Gfx8 PMA fix) stayed.
# Without it, depth-only passes run ~10x slower than Intel's own driver
# (shadow maps: The Talos Principle spent 14 of 20 ms per frame there).
hiz_old = '''      ok = isl_surf_get_hiz_surf(&device->isl_dev,
                                 &image->planes[plane].primary_surface.isl,
                                 &image->planes[plane].aux_surface.isl);
      if (!ok)
         return VK_SUCCESS;
'''
assert image_after.count(hiz_old) == 1
image_after = image_after.replace(hiz_old, hiz_old + '''#ifdef _WIN32
      image->planes[plane].aux_usage = ISL_AUX_USAGE_HIZ;
      return add_surface(device, image, &image->planes[plane].aux_surface,
                         ANV_IMAGE_MEMORY_BINDING_PLANE_0 + plane,
                         ANV_OFFSET_IMPLICIT);
#endif
''')
# ...and its layout states, as anv had them for ISL_AUX_USAGE_HIZ: valid HiZ
# (fast-clearable) where the layout allows it, RESOLVED in read-only layouts
# that must not use it (Gfx8 cannot sample with HiZ), otherwise invalid.
hiz_state_old = '''   switch (aux_usage) {
   case ISL_AUX_USAGE_CCS_D:
      /* We only support clear in exactly one state */'''
assert image_after.count(hiz_state_old) == 1
image_after = image_after.replace(hiz_state_old, '''   switch (aux_usage) {
#ifdef _WIN32
   case ISL_AUX_USAGE_HIZ:
      if (aux_supported) {
         assert(clear_supported);
         return ISL_AUX_STATE_COMPRESSED_CLEAR;
      } else if (read_only) {
         return ISL_AUX_STATE_RESOLVED;
      } else {
         return ISL_AUX_STATE_AUX_INVALID;
      }
#endif
   case ISL_AUX_USAGE_CCS_D:
      /* We only support clear in exactly one state */''')
edit(image_name, [(image_before, image_after)])
# A shader upload that fails (out of memory) is returned to the caller, which
# skips the operation; the driver marks the command buffer failed. Upstream
# asserts, which ended the process in CTS simulate_oom tests.
for name, uploads in [('src/intel/blorp/blorp_clear.c', 3), ('src/intel/blorp/blorp_blit.c', 2)]:
    edit(name, [('#include "blorp_shaders.h"', '#ifdef HASVK_WDDM_LEGACY_BLORP\n#include "blorp_bounds_nir.h"\n#else\n#include "blorp_shaders.h"\n#endif'),
                ('   assert(result);\n', '   (void)result;   /* failure is returned (out of memory) */\n', uploads)])
edit("src/intel/mda/debug_archiver.c", [
    ('#include "util/u_process.h"', '#include "util/u_process.h"\n#include "util/os_file.h"'),
    ('#include <unistd.h>', '#ifdef _WIN32\n#include <process.h>\n#define getpid _getpid\n#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)\n#else\n#include <unistd.h>\n#endif'),
    ('mkdir(dir, 0755)', 'os_mkdir(dir, 0755)'),
])
edit("src/intel/dev/intel_device_info.c", [
    ('#include <unistd.h>\n\n#include "util/libdrm.h"', '#ifndef _WIN32\n#include <unistd.h>\n#include "util/libdrm.h"\n#endif'),
    ('#include "i915/intel_device_info.h"\n#include "xe/intel_device_info.h"\n#include "virtio/intel_virtio.h"\n\n#include "common/intel_gem.h"', '#ifndef _WIN32\n#include "i915/intel_device_info.h"\n#include "xe/intel_device_info.h"\n#include "virtio/intel_virtio.h"\n#include "common/intel_gem.h"\n#endif'),
    ('#include "util/u_debug.h"', '#include "util/u_debug.h"\n#include "util/strndup.h"'),
    ('   intel_device_info_i915_update_from_masks(devinfo,\n', '''#ifdef _WIN32
   /* Static PCI-table topology for the offline compiler only. WDDM runtime
    * discovery must supply the real enabled slice/subslice/EU masks. */
   unsigned slices = devinfo->num_slices;
   unsigned subslices = devinfo->num_subslices[0];
   unsigned eus = devinfo->max_eus_per_subslice;
   intel_device_info_topology_reset_masks(devinfo);
   devinfo->max_slices = slices;
   devinfo->max_subslices_per_slice = subslices;
   devinfo->subslice_slice_stride = DIV_ROUND_UP(subslices, 8);
   devinfo->eu_subslice_stride = DIV_ROUND_UP(eus, 8);
   devinfo->eu_slice_stride = subslices * devinfo->eu_subslice_stride;
   devinfo->slice_masks = (1u << slices) - 1;
   for (unsigned s = 0; s < slices; ++s) {
      for (unsigned ss = 0; ss < subslices; ++ss) {
         devinfo->subslice_masks[s * devinfo->subslice_slice_stride + ss / 8] |= 1u << (ss % 8);
         for (unsigned eu = 0; eu < eus; ++eu)
            devinfo->eu_masks[s * devinfo->eu_slice_stride + ss * devinfo->eu_subslice_stride + eu / 8] |= 1u << (eu % 8);
      }
   }
   intel_device_info_topology_update_counts(devinfo);
   intel_device_info_update_pixel_pipes(devinfo, devinfo->subslice_masks);
   intel_device_info_update_l3_banks(devinfo);
#else
   intel_device_info_i915_update_from_masks(devinfo,
'''),
    ('                          devinfo->max_eus_per_subslice);\n}', '                          devinfo->max_eus_per_subslice);\n#endif\n}'),
    ("static void\nintel_device_info_adjust_memory", "#ifndef _WIN32\nstatic void\nintel_device_info_adjust_memory"),
    ("enum intel_wa_steppings\nintel_device_info_wa_stepping", "#endif /* Linux DRM device queries; the offline compiler uses PCI tables */\n\nenum intel_wa_steppings\nintel_device_info_wa_stepping"),
])
edit("src/util/u_atomic.h", [
    ('_interlockedexchange64', '_InterlockedExchange64'),
    ('_interlockedexchangeadd64', '_InterlockedExchangeAdd64'),
    ('_interlockedincrement64', '_InterlockedIncrement64'),
    ('_interlockeddecrement64', '_InterlockedDecrement64'),
    ('_interlockedadd  ((long', 'p_atomic_msvc_add32((long'),
    ('_interlockedadd64((__int64', 'p_atomic_msvc_add64((__int64'),
    ('/* MSVC supports decltype keyword,', '''static __forceinline long
p_atomic_msvc_add32(long volatile *address, long value)
{
   return (long)((unsigned long)_InterlockedExchangeAdd(address, value) + (unsigned long)value);
}

static __forceinline __int64
p_atomic_msvc_add64(__int64 volatile *address, __int64 value)
{
   return (__int64)((unsigned __int64)_InterlockedExchangeAdd64(address, value) + (unsigned __int64)value);
}

/* MSVC supports decltype keyword,'''),
])
edit("src/intel/compiler/elk/elk_eu.c", [
    ('#include <fcntl.h>', '#include <fcntl.h>\n#ifdef _WIN32\n#include <stdint.h>\n#include <io.h>\ntypedef intptr_t ssize_t;\n#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)\n#else\n#define O_BINARY 0\n#endif'),
    ('O_CREAT | O_WRONLY | O_TRUNC,', 'O_CREAT | O_WRONLY | O_TRUNC | O_BINARY,'),
    ('open(name, O_RDONLY)', 'open(name, O_RDONLY | O_BINARY)'),
])
edit("src/intel/compiler/elk/elk_eu_validate.c", [
    ('/* We\'re going to do lots of string concatenation, so this should help. */', '''#ifdef _WIN32
static const void *
elk_memmem(const void *haystack, size_t size, const void *needle, size_t length)
{
   if (!length) return haystack;
   if (length > size) return NULL;
   for (size_t i = 0; i <= size - length; ++i)
      if (!memcmp((const char *)haystack + i, needle, length))
         return (const char *)haystack + i;
   return NULL;
}
#define memmem elk_memmem
#endif

/* We're going to do lots of string concatenation, so this should help. */'''),
])
edit("src/intel/compiler/elk/elk_fs_bank_conflicts.cpp", [
    ('#include "util/macros.h"', '#include "util/macros.h"\n#include "util/u_memory.h"'),
    ('         free(v);', '         align_free(v);'),
    ('         void *p;\n         if (posix_memalign(&p, align, size))', '         void *p = align_malloc(size, align);\n         if (!p)'),
])
edit("src/intel/compiler/elk/elk_fs.cpp", [
    ('#include "util/u_math.h"', '#include "util/u_math.h"\n#include "util/u_string.h"'),
])
for name, enum_names in {
    'src/intel/compiler/elk/elk_eu_defines.h': ['elk_reg_file', 'elk_predicate'],
    'src/intel/compiler/elk/elk_reg_type.h': ['elk_reg_type'],
    'src/intel/compiler/elk/elk_compiler.h': ['elk_robustness_flags', 'elk_sf_primitive', 'elk_clip_fill_mode', 'elk_clip_mode', 'elk_sometimes'],
}.items():
    upstream = (originals / name).read_text(encoding='utf-8')
    first_include = re.search(r'^#include [^\n]+', upstream, re.M).group()
    replacements = [(first_include, '#include "elk_win32_compat.h"\n' + first_include)]
    for enum_name in enum_names:
        pattern = r'\benum(?:\s+(?:ENUM_PACKED|PACKED))?\s+' + enum_name + r'\s*\{'
        declaration = re.search(pattern, upstream)
        if declaration is None:
            raise ValueError(f'Missing enum declaration: {enum_name}')
        old = declaration.group()
        replacements.append((old, old[:-1].rstrip() + ' ELK_ENUM_BASE {'))
    if name.endswith('elk_reg_type.h'):
        replacements.append(('enum elk_reg_file;', 'enum elk_reg_file ELK_ENUM_BASE;'))
    if name.endswith('elk_compiler.h'):
        replacements.append(('alpha_test_func:3;', 'alpha_test_func:ELK_COMPARE_FUNC_BITS;'))
    edit(name, replacements)
edit("src/util/macros.h", [
    ("#define PRAGMA_POISON\n", "#define PRAGMA_POISON(X)\n"),
])
edit("include/drm-uapi/drm_fourcc.h", [
    ('#include "drm.h"', '#ifdef _WIN32\n/* Format/modifier constants only; no DRM transport on Windows. */\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef uint64_t __u64;\n#else\n#include "drm.h"\n#endif'),
])
owned_sources = {
    'src/intel/vulkan_hasvk/anv_wddm_compat.h': (ROOT / 'mesa/anv_wddm_compat.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_device.c': (ROOT / 'mesa/anv_wddm_device.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm.h': (ROOT / 'mesa/anv_wddm.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_memory.c': (ROOT / 'mesa/anv_wddm_memory.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_allocator.h': (ROOT / 'mesa/anv_wddm_allocator.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_pools.c': (ROOT / 'mesa/anv_wddm_pools.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_batch.c': (ROOT / 'mesa/anv_wddm_batch.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_command.c': (ROOT / 'mesa/anv_wddm_command.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_present.c': (ROOT / 'mesa/anv_wddm_present.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_profile.c': (ROOT / 'mesa/anv_wddm_profile.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/vulkan_hasvk_wddm.def': (ROOT / 'mesa/anv_wddm.def').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/elk_compile_probe.c': (ROOT / 'mesa/elk_compile_probe.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/elk_abi_probe.cpp': (ROOT / 'mesa/elk_abi_probe.cpp').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/spirv_probe.c': (ROOT / 'mesa/spirv_probe.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/spirv_probe.h': (ROOT / 'mesa/spirv_probe.h').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/image_probe.c': (ROOT / 'mesa/image_probe.c').read_text(encoding='utf-8'),
    'src/intel/compiler/elk/elk_win32_compat.h': (ROOT / 'mesa/elk_win32_compat.h').read_text(encoding='utf-8'),
    'src/intel/blorp/blorp_bounds_nir.h': '''/* Equivalent NIR builder for blorp_shaders.cl::blorp_check_in_bounds.
 * Only for the bounded Gen8 Windows bring-up. Indirect-copy shaders are not
 * compiled in this target and no corresponding Vulkan extension is exposed. */
#pragma once
#include "compiler/nir/nir_builder.h"
static inline nir_def *
blorp_check_in_bounds(nir_builder *b, nir_def *rect, nir_def *pos)
{
   nir_def *x = nir_channel(b, pos, 0), *y = nir_channel(b, pos, 1);
   return nir_iand(b,
      nir_iand(b, nir_uge(b, x, nir_channel(b, rect, 0)), nir_ult(b, x, nir_channel(b, rect, 1))),
      nir_iand(b, nir_uge(b, y, nir_channel(b, rect, 2)), nir_ult(b, y, nir_channel(b, rect, 3))));
}
''',
    'src/intel/wddm_compiler/meson.build': '''# Windows ELK/ISL/BLORP and initial, incomplete HasVK ICD.
executable('IrisElkCompile', ['elk_compile_probe.c', 'elk_abi_probe.cpp', 'spirv_probe.c'],
  include_directories : [inc_include, inc_src, inc_intel],
  dependencies : [idep_intel_compiler_elk, idep_intel_dev, idep_nir, idep_mesautil, idep_mda],
  c_args : [c_msvc_compat_args],
  win_subsystem : 'console',
)
libwddm_blorp = static_library('wddm_blorp',
  ['../blorp/blorp.c', '../blorp/blorp_blit.c', '../blorp/blorp_clear.c', '../blorp/blorp_elk.c',
   '../common/intel_compute_slm.c', '../common/intel_l3_config.c',
   '../common/intel_sample_positions.c', '../common/intel_urb_config.c'],
  include_directories : [inc_include, inc_src, inc_intel],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_intel_dev],
  c_args : [c_msvc_compat_args, '-DHASVK_WDDM_LEGACY_BLORP'],
  build_by_default : false,
)
''',
}
# Gen8's actual compute emitter and its two helpers, verbatim from the pinned
# source. Graphics emission is added when graphics command buffers are enabled.
gen_pipeline = (originals / 'src/intel/vulkan_hasvk/genX_pipeline.c').read_text(encoding='utf-8')
compute_emitter = gen_pipeline[:gen_pipeline.index('\nstatic ')]
for signature in ['static uint32_t\nget_sampler_count(', 'static UNUSED struct anv_address\nget_scratch_address(',
                  'void\ngenX(compute_pipeline_emit)(']:
    start = gen_pipeline.index(signature)
    end = gen_pipeline.index('\n}\n', start) + 3
    compute_emitter += '\n' + gen_pipeline[start:end] + '\n'
owned_sources['src/intel/wddm_compiler/anv_wddm_compute_emit.c'] = compute_emitter
formats = (originals / 'src/intel/vulkan_hasvk/anv_formats.c').read_text(encoding='utf-8')
format_split = formats.index('// Format capabilities')
# The capability queries (format features, image format properties) are the
# upstream ones whenever graphics is built.
format_caps = formats[format_split:]
# No external memory handle type exists on this port (no fd, dma-buf, AHB).
old = '   switch (pExternalBufferInfo->handleType) {'
assert format_caps.count(old) == 1
format_caps = format_caps.replace(old, '#ifdef _WIN32\n   (void)physical_device;\n   goto unsupported;\n#endif\n' + old)
old = '      switch (external_info->handleType) {\n      case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:'
assert format_caps.count(old) == 1
format_caps = format_caps.replace(old, '#ifdef _WIN32\n      result = vk_errorf(physical_device, VK_ERROR_FORMAT_NOT_SUPPORTED,\n                         "no external memory handle types on Windows");\n      goto fail;\n#endif\n' + old)
owned_sources['src/intel/wddm_compiler/anv_wddm_format_table.c'] = (
    formats[:format_split] + '#ifdef HASVK_WDDM_GRAPHICS\n' + format_caps + '\n#endif\n')
owned_sources.update(generate_compute_sources(originals))
owned_sources.update(generate_graphics_sources(originals))
for name in ['hasvk_wddm.h', 'hasvk_wddm.hpp']:
    owned_sources['src/intel/wddm/' + name] = (ROOT / 'native/include' / name).read_text(encoding='utf-8')
for name in ['hasvk_wddm.cpp', 'hasvk_wddm_c.cpp', 'intel_2019_profile.cpp']:
    owned_sources['src/intel/wddm/' + name] = (ROOT / 'native/src' / name).read_text(encoding='utf-8')
owned_sources['src/intel/wddm_compiler/meson.build'] += '''
libhasvk_wddm = static_library('hasvk_wddm',
  ['../wddm/hasvk_wddm.cpp', '../wddm/hasvk_wddm_c.cpp', '../wddm/intel_2019_profile.cpp'],
  include_directories : [include_directories('../wddm')],
  cpp_args : ['/EHsc', '/U_HAS_EXCEPTIONS', '/D_HAS_EXCEPTIONS=1'],
  dependencies : [cpp.find_library('gdi32'), cpp.find_library('dxgi')],
)
executable('IrisImageProbe', 'image_probe.c',
  include_directories : [inc_include, inc_src, inc_intel],
  link_with : [libwddm_blorp, libisl, libhasvk_wddm],
  dependencies : [idep_nir, idep_intel_dev, idep_intel_compiler_elk, idep_genxml, idep_mesautil,
    cc.find_library('gdi32'), cc.find_library('dxgi')],
  c_args : [c_msvc_compat_args, '-DGFX_VERx10=80'],
  win_subsystem : 'console',
)
anv_wddm_entrypoints = custom_target('anv_wddm_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml], output : ['anv_entrypoints.h', 'anv_entrypoints.c'],
  command : [prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'anv',
    '--beta', with_vulkan_beta.to_string(), '--device-prefix', 'gfx8'],
  depend_files : vk_entrypoints_gen_depend_files,
)
anv_wddm_drirc = custom_target('anv_wddm_drirc',
  input : ['../vulkan_hasvk/anv_drirc_gen.py', drirc_gen, '../vulkan_hasvk/00-hasvk-defaults.conf'],
  output : ['hasvk_drirc.c', 'hasvk_drirc.h'],
  command : [prog_python, '@INPUT0@', '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@', '--drirc-hdr', '@OUTPUT1@', '--validate', '@INPUT2@'],
)
graphics_args = []
common_commands = ['anv_wddm_command_common.c']
gen8_commands = ['anv_wddm_compute_emit.c', 'anv_wddm_command_gen8.c']
if get_option('hasvk-wddm-graphics')
  graphics_args = ['-DHASVK_WDDM_GRAPHICS']
  common_commands = ['anv_wddm_graphics_common.c', 'anv_wddm_graphics_blorp.c', 'anv_wddm_capabilities.c']
  gen8_commands = ['anv_wddm_graphics_gen8.c', 'anv_wddm_graphics_pipeline.c',
    'anv_wddm_graphics_dynamic.c', 'anv_wddm_graphics_state.c', 'anv_wddm_graphics_blorp_exec.c',
    'anv_wddm_graphics_query.c', 'anv_wddm_graphics_memcpy.c']
endif
libanv_wddm_objects = static_library('anv_wddm_objects',
  ['../vulkan_hasvk/anv_wddm_device.c', '../vulkan_hasvk/anv_wddm_memory.c', '../vulkan_hasvk/anv_wddm_pools.c', '../vulkan_hasvk/anv_allocator.c',
   '../vulkan_hasvk/anv_wddm_present.c', '../vulkan_hasvk/anv_wddm_profile.c',
   '../vulkan_hasvk/anv_descriptor_set.c',
   '../vulkan_hasvk/anv_image.c', '../vulkan_hasvk/anv_android_stubs.c',
   '../vulkan_hasvk/anv_pipeline.c', '../vulkan_hasvk/anv_pipeline_cache.c', '../vulkan_hasvk/anv_wddm_batch.c',
   '../vulkan_hasvk/anv_nir_apply_pipeline_layout.c', '../vulkan_hasvk/anv_nir_compute_push_layout.c',
   '../vulkan_hasvk/anv_nir_lower_multiview.c', '../vulkan_hasvk/anv_nir_lower_ubo_loads.c',
   '../vulkan_hasvk/anv_nir_lower_ycbcr_textures.c', '../vulkan_hasvk/anv_util.c',
   'anv_wddm_format_table.c',
   anv_wddm_entrypoints[0], anv_wddm_drirc[1]] + common_commands,
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_vulkan_runtime_headers,
    idep_vulkan_wsi_headers, idep_vulkan_util_headers],
  c_args : [c_msvc_compat_args, graphics_args],
)
libanv_wddm_gen8 = static_library('anv_wddm_gen8',
  ['../vulkan_hasvk/anv_wddm_command.c',
   anv_wddm_entrypoints[0], anv_wddm_drirc[1]] + gen8_commands,
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_vulkan_runtime_headers,
    idep_vulkan_wsi_headers, idep_vulkan_util_headers],
  c_args : [c_msvc_compat_args, graphics_args, '-DGFX_VERx10=80'],
)
shared_library('vulkan_hasvk_wddm', anv_wddm_entrypoints[1],
  vs_module_defs : 'vulkan_hasvk_wddm.def',
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  link_whole : [libanv_wddm_objects, libanv_wddm_gen8],
  link_with : [libhasvk_wddm, libisl, libwddm_blorp],
  dependencies : [idep_nir, idep_intel_dev, idep_intel_compiler_elk, idep_mesautil,
    idep_vulkan_runtime, idep_vulkan_util, idep_vulkan_wsi,
    cc.find_library('gdi32'), cc.find_library('dxgi'), cc.find_library('user32'), cc.find_library('uuid')],
  c_args : c_msvc_compat_args,
  install : false,
)
'''
for name, contents in owned_sources.items():
    target = MESA / name
    target.parent.mkdir(exist_ok=True)
    if not target.exists() or target.read_text(encoding='utf-8') != contents:
        target.write_text(contents, encoding='utf-8', newline='\n')
    patches.extend(difflib.unified_diff([], contents.splitlines(True), '/dev/null', f'b/{name}'))
if patches:
    patch = ROOT / "mesa/patches/compiler-port.patch"
    patch.parent.mkdir(exist_ok=True)
    patch.write_text("".join(patches), encoding="utf-8", newline="\n")
print("Prepared Windows Mesa libraries and HasVK control-plane ICD (no shader command buffers)")
