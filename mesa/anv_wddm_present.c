/* Windowed present without a CPU copy and without D3D.
 *
 * As the Intel OpenGL ICD of the same driver package does (traced with
 * tools/dev/trace-gl-present.ps1, reproduced by tools/dev/test-present-redirect.ps1):
 * the frame is copied by the GPU into a shared window surface and the
 * submission carries PresentRedirected with the token
 * (0x80000000 | HWND) << 32 | share handle; DWM composes that surface.
 *
 * DWM places the surface at the top-left corner of the whole window rectangle
 * (frame included), so the surface has the window's size and the image is
 * copied at the client area's offset. Per swapchain one such surface:
 * B8G8R8A8, Y-tiled, no aux, wrapped in an anv_image so the copy is an
 * ordinary BLORP copy (or a channel-swapping blit); recreated when the window
 * size changes.
 *
 * The copy is recorded at vkQueuePresentKHR and goes into WSI's own present
 * submission, behind the application's work on the same in-order queue: no
 * CPU wait for the frame and no extra kernel submission. The command buffer
 * carries the present token (anv_wddm_cmd_state.present_token); the queue
 * passes it to the kernel with that submission. If the kernel refuses the
 * present (STATUS_GRAPHICS_PRESENT_OCCLUDED while the display sleeps), the
 * same commands run without it and the next frame goes through GDI. */
#include "anv_wddm.h"

#ifdef HASVK_WDDM_GRAPHICS
#define ANV_WDDM_PRESENT_CMDS 16u   /* one per swapchain image index */

struct anv_wddm_window {
   HWND hwnd;
   VkExtent2D extent;
   VkFormat format;        /* B8G8R8A8_UNORM or _SRGB, as the swapchain's encoding */
   struct anv_bo *bo;
   uint32_t share;
   VkImage image;
   VkQueue queue;          /* where the copies were submitted */
   VkCommandPool pool;
   /* Copy of image index i. WSI waits for the previous present of the same
    * index (its throttle fence) before recording it again. */
   VkCommandBuffer cmds[ANV_WDDM_PRESENT_CMDS];
   int refused;            /* set by the queue when the kernel refused a present */
};

/* Swapchain images of the window surface layout get window surfaces as their
 * memory (anv_AllocateMemory), and a present of such an image to a window of
 * exactly its size (no frame, e.g. a borderless full-screen game) hands DWM
 * the image itself: no copy, 2 x 4 bytes per pixel less memory traffic per
 * frame (Dota 2 at 2560x1600: 25.5-25.7 FPS against 23.4-24.7 with the copy).
 * Every swapchain image is then a surface of its own; DWM opens the one named
 * by each present's token. DWM reads them as B8G8R8A8, so R8G8B8A8 surface
 * formats are not reported (wsi_win32_format_hidden). Windows with a frame,
 * child windows and size mismatches still copy. */
bool anv_wddm_window_image(struct anv_device *device, const struct anv_image *image, uint64_t size)
{
   (void)device; (void)size;
   if (!image->from_wsi) return false;
   if (image->n_planes != 1 || image->vk.samples != VK_SAMPLE_COUNT_1_BIT ||
       image->vk.mip_levels != 1 || image->vk.array_layers != 1 || image->vk.extent.depth != 1 ||
       (image->vk.format != VK_FORMAT_B8G8R8A8_UNORM && image->vk.format != VK_FORMAT_B8G8R8A8_SRGB))
      return false;
   const struct anv_image_plane *plane = &image->planes[0];
   return plane->primary_surface.isl.tiling == ISL_TILING_Y0 && plane->aux_usage == ISL_AUX_USAGE_NONE &&
          plane->primary_surface.memory_range.binding == ANV_IMAGE_MEMORY_BINDING_MAIN &&
          plane->primary_surface.memory_range.offset == 0 &&
          image->bindings[ANV_IMAGE_MEMORY_BINDING_PRIVATE].memory_range.size == 0;
}

/* ---- Presents by exclusive flip --------------------------------------------
 *
 * DWM composes a borderless window of the screen's size like any other: it
 * copies the frame (Dota 2 at 2560x1600: 5 % of the GPU and 12 % of a logical
 * CPU in dwm.exe, and the copy's memory traffic slows the game's own code).
 * On this system it does so for a flip-model DXGI swap chain too; only DXGI's
 * full-screen state shows the application's buffer directly. This is that
 * state taken by the driver, as other vendors' Vulkan drivers take it for a
 * window that covers the screen:
 *
 * - a swapchain image of the screen's size is a primary of the output
 *   (anv_wddm_flip_candidate, anv_wddm_alloc_primary_bo);
 * - at a present, if its window covers the primary screen, is in the
 *   foreground and has nothing above it, the device owns the output
 *   exclusively (hasvk_wddm_output_acquire) and the frame is shown by a flip
 *   right after WSI's present submission (anv_wddm_flip_after_submit);
 * - a watcher thread gives the output back as soon as that is no longer so
 *   (another window activated or shown above, a refused flip), and the
 *   presents go through the window copy again until it is.
 *
 * VK_PRESENT_MODE_IMMEDIATE flips at once (tearing, as the mode allows), FIFO
 * at the vertical blank. MAILBOX stays with the compositor: a flip either
 * tears or waits. HASVK_WDDM_PRESENT_FLIP=0 turns all of this off. */
struct anv_wddm_flip {
   mtx_t mutex;
   HWND hwnd;                  /* window whose frames are flipped; NULL: the output is not ours */
   uint64_t retry_ms;          /* after a refusal: no new attempt before this time */
   thrd_t watcher;
   bool watcher_started, exit;
   struct anv_device *device;
};
static bool flip_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) { const char *v = getenv("HASVK_WDDM_PRESENT_FLIP"); enabled = !v || strcmp(v, "0"); }
   return enabled;
}
static bool flip_debug(void)
{
   static int debug = -1;
   if (debug < 0) debug = getenv("HASVK_WDDM_PRESENT_FLIP_DEBUG") != NULL;
   return debug;
}
static bool flip_mode(uint32_t present_mode)
{
   return present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR || present_mode == VK_PRESENT_MODE_FIFO_KHR ||
          present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}
/* The present mode of the swapchain whose images this thread is creating
 * (WSI calls anv_wddm_present_images_mode around that). */
static __declspec(thread) uint32_t images_present_mode = UINT32_MAX;
void anv_wddm_present_images_mode(VkDevice device, uint32_t present_mode)
{
   (void)device; images_present_mode = present_mode;
}
bool anv_wddm_flip_candidate(struct anv_device *device, const VkImageCreateInfo *info)
{
   if (!flip_enabled() || !flip_mode(images_present_mode) || !device->wddm || !device->wddm->transport) return false;
   if (info->imageType != VK_IMAGE_TYPE_2D || info->mipLevels != 1 || info->arrayLayers != 1 ||
       info->samples != VK_SAMPLE_COUNT_1_BIT || info->extent.depth != 1 ||
       (info->format != VK_FORMAT_B8G8R8A8_UNORM && info->format != VK_FORMAT_B8G8R8A8_SRGB))
      return false;
   uint32_t width = 0, height = 0;
   if (hasvk_wddm_output_screen(device->wddm->transport, &width, &height)) return false;
   return info->extent.width == width && info->extent.height == height;
}
bool anv_wddm_flip_image(const struct anv_image *image)
{
   if (!image->from_wsi || image->n_planes != 1) return false;
   const struct anv_image_plane *plane = &image->planes[0];
   return plane->primary_surface.isl.tiling == ISL_TILING_X && plane->aux_usage == ISL_AUX_USAGE_NONE &&
          plane->primary_surface.memory_range.binding == ANV_IMAGE_MEMORY_BINDING_MAIN &&
          plane->primary_surface.memory_range.offset == 0 &&
          image->bindings[ANV_IMAGE_MEMORY_BINDING_PRIVATE].memory_range.size == 0;
}
/* The window is the whole primary screen, in the foreground, with no visible
 * window above it: what the compositor would show is exactly this window. */
static bool flip_window_alone(HWND hwnd)
{
   if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetForegroundWindow() != hwnd) return false;
   RECT window, client; POINT origin = {0, 0};
   MONITORINFO monitor = {.cbSize = sizeof(monitor)};
   HMONITOR handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
   if (!handle || !GetMonitorInfoW(handle, &monitor) || !(monitor.dwFlags & MONITORINFOF_PRIMARY)) return false;
   if (!GetWindowRect(hwnd, &window) || !EqualRect(&window, &monitor.rcMonitor)) return false;
   if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return false;
   if (origin.x != window.left || origin.y != window.top || client.right != window.right - window.left ||
       client.bottom != window.bottom - window.top)
      return false;
   typedef HRESULT (WINAPI *PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
   static PFN_DwmGetWindowAttribute get_attribute;
   static bool loaded;
   if (!loaded) {
      HMODULE dwm = LoadLibraryA("dwmapi.dll");
      get_attribute = dwm ? (PFN_DwmGetWindowAttribute)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
      loaded = true;
   }
   for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
      RECT r, shown; int cloaked = 0;
      if (!IsWindowVisible(above) || IsIconic(above) || !GetWindowRect(above, &r) || !IntersectRect(&shown, &r, &monitor.rcMonitor)) continue;
      if (get_attribute && SUCCEEDED(get_attribute(above, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) continue;
      if (flip_debug()) {
         static HWND reported; char name[64] = "";
         if (reported != above) { reported = above; GetClassNameA(above, name, sizeof(name));
            fprintf(stderr, "HasVK/WDDM flip: a window is above (%s, %ldx%ld): composed\n", name, shown.right - shown.left, shown.bottom - shown.top); }
      }
      return false;
   }
   return true;
}
static struct anv_wddm_flip *flip_state(struct anv_device *device)
{
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->alloc_mutex);
   if (!w->flip) {
      struct anv_wddm_flip *flip = calloc(1, sizeof(*flip));
      if (flip && mtx_init(&flip->mutex, mtx_plain) == thrd_success) { flip->device = device; w->flip = flip; }
      else free(flip);
   }
   mtx_unlock(&w->alloc_mutex);
   return w->flip;
}
/* flip->mutex held. */
static void flip_release_locked(struct anv_wddm_flip *flip, const char *reason)
{
   if (!flip->hwnd) return;
   hasvk_wddm_output_release(flip->device->wddm->transport);
   flip->hwnd = NULL;
   if (flip_debug()) fprintf(stderr, "HasVK/WDDM flip: the output is the desktop's again (%s)\n", reason);
}
void anv_wddm_flip_release(struct anv_device *device)
{
   struct anv_wddm_flip *flip = device->wddm ? device->wddm->flip : NULL;
   if (!flip) return;
   mtx_lock(&flip->mutex); flip_release_locked(flip, "released"); mtx_unlock(&flip->mutex);
}
/* While the output is ours nothing else is visible: give it back as soon as
 * the window is not alone on the screen any more, also when the application
 * stops presenting (a paused game after Alt+Tab would keep its last frame on
 * the screen). */
static int flip_watcher(void *data)
{
   struct anv_wddm_flip *flip = data;
   for (;;) {
      Sleep(30);
      mtx_lock(&flip->mutex);
      const bool exit = flip->exit;
      if (!exit && flip->hwnd && !flip_window_alone(flip->hwnd)) {
         flip_release_locked(flip, "the window is not alone on the screen");
         flip->retry_ms = GetTickCount64() + 300;
      }
      mtx_unlock(&flip->mutex);
      if (exit) return 0;
   }
}
void anv_wddm_flip_finish(struct anv_device *device)
{
   struct anv_wddm_flip *flip = device->wddm ? device->wddm->flip : NULL;
   if (!flip) return;
   mtx_lock(&flip->mutex); flip_release_locked(flip, "device destroyed"); flip->exit = true; mtx_unlock(&flip->mutex);
   if (flip->watcher_started) thrd_join(flip->watcher, NULL);
   mtx_destroy(&flip->mutex); free(flip); device->wddm->flip = NULL;
}
/* At a present of a primary: true if this frame is to be flipped (the output
 * is ours, taken now if the window just became alone on the screen). */
static bool flip_begin(struct anv_device *device, HWND hwnd, struct anv_bo *bo, uint32_t present_mode)
{
   if (!flip_enabled() || !flip_mode(present_mode)) return false;
   struct anv_wddm_flip *flip = flip_state(device);
   if (!flip) return false;
   bool flipped = false;
   mtx_lock(&flip->mutex);
   const bool alone = flip_window_alone(hwnd);
   if (flip->hwnd && (flip->hwnd != hwnd || !alone)) flip_release_locked(flip, "another window");
   if (alone && !flip->hwnd && GetTickCount64() >= flip->retry_ms) {
      if (!hasvk_wddm_output_acquire(device->wddm->transport, bo->wddm_bo)) {
         flip->hwnd = hwnd;
         if (flip_debug()) fprintf(stderr, "HasVK/WDDM flip: the output is ours (present mode %u)\n", present_mode);
         if (!flip->watcher_started) flip->watcher_started = thrd_create(&flip->watcher, flip_watcher, flip) == thrd_success;
         if (!flip->watcher_started) flip_release_locked(flip, "no watcher thread");
      } else {
         flip->retry_ms = GetTickCount64() + 1000;
         if (flip_debug()) fprintf(stderr, "HasVK/WDDM flip: %s\n", hasvk_wddm_error());
      }
   }
   flipped = flip->hwnd == hwnd;
   mtx_unlock(&flip->mutex);
   return flipped;
}
/* submit_mutex held, before the kernel submission(s) of these command
 * buffers: while the output is ours, the primaries they render into. */
void anv_wddm_flip_before_submit(struct anv_device *device, struct vk_command_buffer *const *commands, uint32_t count)
{
   struct anv_wddm_device_state *w = device->wddm;
   w->flip_written_count = 0;
   if (!w->flip || !w->flip->hwnd || !w->flip_bo_count) return;
   mtx_lock(&w->alloc_mutex);
   for (uint32_t b = 0; b < w->flip_bo_count && w->flip_written_count < ARRAY_SIZE(w->flip_written); ++b) {
      const uint32_t id = w->flip_bos[b]->gem_handle;
      for (uint32_t i = 0; i < count; ++i) {
         const struct anv_cmd_buffer *cmd = container_of(commands[i], struct anv_cmd_buffer, vk);
         const struct anv_reloc_list *list = cmd->batch.relocs;
         if (list && id < list->dep_words * BITSET_WORDBITS && BITSET_TEST(list->deps, id)) {
            w->flip_written[w->flip_written_count++] = w->flip_bos[b]->wddm_bo;
            break;
         }
      }
   }
   mtx_unlock(&w->alloc_mutex);
}
/* submit_mutex held, right after the kernel submission that ends with cmd:
 * the flip is queued behind it. */
void anv_wddm_flip_after_submit(struct anv_device *device, struct anv_cmd_buffer *cmd)
{
   struct anv_wddm_cmd_state *s = cmd->wddm;
   struct anv_wddm_flip *flip = device->wddm->flip;
   if (!s->present_flip_bo || !flip) return;
   const int status = hasvk_wddm_present_flip(device->wddm->transport, s->present_flip_bo->wddm_bo, s->present_flip_hwnd,
                                              s->present_flip_width, s->present_flip_height, s->present_flip_interval);
   if (status) {
      /* Refused: the output is no longer ours (the transport gave it back).
       * This frame is not shown; the next ones go through the window copy. */
      mtx_lock(&flip->mutex);
      flip->hwnd = NULL; flip->retry_ms = GetTickCount64() + 1000;
      mtx_unlock(&flip->mutex);
      if (flip_debug()) fprintf(stderr, "HasVK/WDDM flip: %s\n", hasvk_wddm_error());
   }
   s->present_flip_bo = NULL;
}

/* Waits until every copy submitted for this window has executed. */
static void window_idle(struct anv_device *device, struct anv_wddm_window *w)
{
   if (w->queue) device->vk.dispatch_table.QueueWaitIdle(w->queue);
}
static void surface_release(struct anv_device *device, struct anv_wddm_window *w)
{
   VkDevice handle = anv_device_to_handle(device);
   if (w->image) anv_DestroyImage(handle, w->image, NULL);
   if (w->bo) anv_device_release_bo(device, w->bo);
   w->image = VK_NULL_HANDLE; w->bo = NULL; w->share = 0;
}

/* Window surface and its image for this HWND and extent. */
static VkResult surface_create(struct anv_device *device, struct anv_wddm_window *w, HWND hwnd, VkExtent2D extent, VkFormat format)
{
   uint64_t pitch = 0;
   VkResult result = anv_wddm_alloc_window_surface_bo(device, extent.width, extent.height, &w->bo, &w->share, &pitch);
   if (result != VK_SUCCESS) return result;
   w->hwnd = hwnd; w->extent = extent; w->format = format;

   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = format, .extent = {extent.width, extent.height, 1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   struct anv_image *image = vk_object_zalloc(&device->vk, NULL, sizeof(*image), VK_OBJECT_TYPE_IMAGE);
   if (!image) return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct anv_image_create_info create = {
      .vk_info = &info, .isl_tiling_flags = ISL_TILING_Y0_BIT, .isl_extra_usage_flags = ISL_SURF_USAGE_DISABLE_AUX_BIT,
   };
   result = anv_image_init(device, image, &create);
   if (result != VK_SUCCESS) { vk_object_free(&device->vk, NULL, image); return result; }
   w->image = anv_image_to_handle(image);
   const struct isl_surf *surf = &image->planes[0].primary_surface.isl;
   if (surf->tiling != ISL_TILING_Y0 || surf->row_pitch_B != pitch || image->planes[0].aux_usage != ISL_AUX_USAGE_NONE ||
       image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].memory_range.size > w->bo->size)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT, "window surface layout differs from ISL (pitch %llu vs %u)",
                       (unsigned long long)pitch, surf->row_pitch_B);
   image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].address = (struct anv_address){w->bo, 0};
   return VK_SUCCESS;
}

static VkCommandBuffer present_record(VkDevice handle, VkQueue queue, VkImage src, uint32_t index,
                                      void *hwnd, VkExtent2D extent, uint32_t present_mode, void **state, bool *flipped)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   const struct vk_device_dispatch_table *d = &device->vk.dispatch_table;
   struct anv_wddm_window *w = *state;
   if (!w) {
      w = vk_zalloc(&device->vk.alloc, sizeof(*w), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!w) return VK_NULL_HANDLE;
      *state = w;
   }
   if (index >= ANV_WDDM_PRESENT_CMDS) return VK_NULL_HANDLE;
   /* The kernel refused the last present: this frame goes through GDI, the
    * next one tries again. */
   if (p_atomic_xchg(&w->refused, 0)) return VK_NULL_HANDLE;
   if (w->queue && w->queue != queue) window_idle(device, w);
   w->queue = queue;

   /* DWM redirects top-level windows only: a child window's content is part of
    * its parent's surface. Its frames go through GDI, which draws into the
    * child's own DC (the redirected token for a child HWND is unverified). */
   if (GetAncestor(hwnd, GA_ROOT) != hwnd) return VK_NULL_HANDLE;
   /* Window size and the client area's offset inside it, in the window's own
    * (possibly DPI-virtualized) coordinates. */
   RECT outer, client;
   POINT origin = {0, 0};
   if (!GetWindowRect(hwnd, &outer) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
      return VK_NULL_HANDLE;
   const VkExtent2D window = {(uint32_t)MAX2(outer.right - outer.left, 1), (uint32_t)MAX2(outer.bottom - outer.top, 1)};
   const VkOffset3D offset = {origin.x - outer.left, origin.y - outer.top, 0};
   if (offset.x < 0 || offset.y < 0 || (uint32_t)offset.x >= window.width || (uint32_t)offset.y >= window.height)
      return VK_NULL_HANDLE;   /* minimized or unusual geometry: GDI takes the frame */
   const VkExtent3D copied = {MIN2(extent.width, window.width - offset.x), MIN2(extent.height, window.height - offset.y), 1};
   /* DWM reads the surface as B8G8R8A8 bytes in the sRGB encoding. An sRGB
    * swapchain already holds encoded bytes: the surface takes the _SRGB view so
    * nothing is decoded on the way. R8G8B8A8 needs its channels swapped (blit). */
   const VkFormat source = anv_image_from_handle(src)->vk.format;
   const bool srgb = source == VK_FORMAT_B8G8R8A8_SRGB || source == VK_FORMAT_R8G8B8A8_SRGB;
   const bool swap = source == VK_FORMAT_R8G8B8A8_UNORM || source == VK_FORMAT_R8G8B8A8_SRGB;
   if (!swap && source != VK_FORMAT_B8G8R8A8_UNORM && source != VK_FORMAT_B8G8R8A8_SRGB) return VK_NULL_HANDLE;
   const VkFormat format = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;

   /* The image is a window surface of the window's size: present it as it is.
    * The application's own command buffer already flushed the render caches
    * (anv_cmd_buffer_end_batch_buffer), so the present's command buffer is
    * empty; it only carries the token, behind the frame on the same queue. */
   const struct anv_image *image = anv_image_from_handle(src);
   const struct anv_address own = image->bindings[ANV_IMAGE_MEMORY_BINDING_MAIN].address;
   uint32_t own_share = 0; uint64_t own_pitch = 0;
   const bool direct = own.bo && own.bo->name && !strcmp(own.bo->name, "window image") && own.offset == 0 && !swap &&
      offset.x == 0 && offset.y == 0 &&
      image->vk.extent.width == window.width && image->vk.extent.height == window.height &&
      !hasvk_wddm_bo_window_surface_info(own.bo->wddm_bo, &own_share, &own_pitch);
   /* The image is a primary and its window is alone on the screen: the frame
    * is flipped onto the output (no copy, no token, no compositor). A primary
    * whose window is not alone is copied like any other image. */
   const bool flip = !direct && own.bo && own.bo->name && !strcmp(own.bo->name, "flip image") && own.offset == 0 && !swap &&
      offset.x == 0 && offset.y == 0 &&
      image->vk.extent.width == window.width && image->vk.extent.height == window.height &&
      flip_begin(device, hwnd, own.bo, present_mode);

   if (!direct && !flip && w->bo && (w->hwnd != hwnd || w->extent.width != window.width || w->extent.height != window.height || w->format != format)) {
      window_idle(device, w);   /* earlier copies still target the old surface */
      surface_release(device, w);
   }
   if (!direct && !flip && !w->bo && surface_create(device, w, hwnd, window, format) != VK_SUCCESS) { surface_release(device, w); return VK_NULL_HANDLE; }

   if (!w->pool) {
      const VkCommandPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      };
      if (d->CreateCommandPool(handle, &pool_info, NULL, &w->pool) != VK_SUCCESS) return VK_NULL_HANDLE;
   }
   if (!w->cmds[index]) {
      const VkCommandBufferAllocateInfo alloc = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = w->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
      };
      if (d->AllocateCommandBuffers(handle, &alloc, &w->cmds[index]) != VK_SUCCESS) return VK_NULL_HANDLE;
   }
   VkCommandBuffer cmd = w->cmds[index];
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (d->ResetCommandBuffer(cmd, 0) != VK_SUCCESS || d->BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return VK_NULL_HANDLE;
   const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
   if (direct || flip) {
      /* nothing to copy */
   } else if (swap) {
      /* 1:1 nearest blit: per-texel format conversion, values unchanged (the
       * sRGB decode/encode round trip of 8-bit values is exact). */
      const VkImageBlit2 region = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .srcSubresource = layers, .dstSubresource = layers,
         .srcOffsets = {{0, 0, 0}, {(int32_t)copied.width, (int32_t)copied.height, 1}},
         .dstOffsets = {offset, {offset.x + (int32_t)copied.width, offset.y + (int32_t)copied.height, 1}},
      };
      const VkBlitImageInfo2 blit = {
         .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .srcImage = src, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .dstImage = w->image, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL, .regionCount = 1, .pRegions = &region,
         .filter = VK_FILTER_NEAREST,
      };
      anv_CmdBlitImage2(cmd, &blit);
   } else {
      const VkImageCopy2 region = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2, .srcSubresource = layers, .dstSubresource = layers,
         .dstOffset = offset, .extent = copied,
      };
      const VkCopyImageInfo2 copy = {
         .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, .srcImage = src, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
         .dstImage = w->image, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL, .regionCount = 1, .pRegions = &region,
      };
      anv_CmdCopyImage2(cmd, &copy);
   }
   if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return VK_NULL_HANDLE;

   struct anv_cmd_buffer *recorded = anv_cmd_buffer_from_handle(cmd);
   if (flip) {
      recorded->wddm->present_token = 0; recorded->wddm->present_refused = NULL;
      recorded->wddm->present_flip_bo = own.bo; recorded->wddm->present_flip_hwnd = hwnd;
      recorded->wddm->present_flip_interval = present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 0 : 1;
      recorded->wddm->present_flip_width = image->vk.extent.width; recorded->wddm->present_flip_height = image->vk.extent.height;
      *flipped = true;
      return cmd;
   }
   recorded->wddm->present_flip_bo = NULL;
   recorded->wddm->present_token = (uint64_t)(0x80000000u | (uint32_t)(uintptr_t)hwnd) << 32 | (direct ? own_share : w->share);
   recorded->wddm->present_refused = &w->refused;
   return cmd;
}
VkCommandBuffer anv_wddm_present_redirected_record(VkDevice handle, VkQueue queue, VkImage src, uint32_t index,
                                                   void *hwnd, VkExtent2D extent, uint32_t present_mode, void **state)
{
   bool flipped = false;
   const VkCommandBuffer cmd = present_record(handle, queue, src, index, hwnd, extent, present_mode, state, &flipped);
   /* A frame of this window that is not flipped (copied, or left to GDI) is
    * shown by the compositor: the output must not stay ours. */
   if (!flipped) {
      ANV_FROM_HANDLE(anv_device, device, handle);
      struct anv_wddm_flip *flip = device->wddm ? device->wddm->flip : NULL;
      if (flip) {
         mtx_lock(&flip->mutex);
         if (flip->hwnd == (HWND)hwnd) { flip_release_locked(flip, "a frame that is not flipped"); flip->retry_ms = GetTickCount64() + 300; }
         mtx_unlock(&flip->mutex);
      }
   }
   return cmd;
}

void anv_wddm_present_redirected_finish(VkDevice handle, void *state)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   struct anv_wddm_window *w = state;
   if (!w) return;
   /* The swapchain's images may be the primaries on the screen. */
   anv_wddm_flip_release(device);
   window_idle(device, w);
   if (w->pool) device->vk.dispatch_table.DestroyCommandPool(handle, w->pool, NULL);
   surface_release(device, w);
   vk_free(&device->vk.alloc, w);
}
#endif
