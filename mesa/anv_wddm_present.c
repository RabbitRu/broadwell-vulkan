/* Windowed present without a CPU copy and without D3D.
 *
 * As the Intel OpenGL ICD of the same driver package does (traced with
 * tools/trace-gl-present.ps1, reproduced by tools/test-present-redirect.ps1):
 * the frame is copied by the GPU into a shared window surface and the
 * submission carries PresentRedirected with the token
 * (0x80000000 | HWND) << 32 | share handle; DWM composes that surface.
 *
 * DWM places the surface at the top-left corner of the whole window rectangle
 * (frame included), so the surface has the window's size and the image is
 * copied at the client area's offset. Per swapchain one such surface:
 * B8G8R8A8, Y-tiled, no aux, wrapped in an anv_image so the copy is an
 * ordinary BLORP vkCmdCopyImage2; recreated when the window size changes. Called from WSI after the image's rendering has completed
 * (wsi_common waits for it before platform present on this port). */
#include "anv_wddm.h"

#ifdef HASVK_WDDM_GRAPHICS
struct anv_wddm_window {
   HWND hwnd;
   VkExtent2D extent;
   struct anv_bo *bo;
   uint32_t share;
   VkImage image;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   uint64_t pending_seq;   /* transport number of the copy not yet waited for */
};

/* The previous frame's copy is submitted without waiting: wait for it and
 * check its completion marker before the command buffer or surface is reused. */
static int finish_pending(struct anv_device *device, struct anv_wddm_window *w)
{
   if (!w->pending_seq) return 0;
   const uint64_t seq = w->pending_seq;
   w->pending_seq = 0;
   if (hasvk_wddm_wait_submission(device->wddm->transport, seq)) return -1;
   return anv_wddm_check_completed(anv_cmd_buffer_from_handle(w->cmd));
}
static void window_release(struct anv_device *device, struct anv_wddm_window *w)
{
   VkDevice handle = anv_device_to_handle(device);
   if (finish_pending(device, w)) fprintf(stderr, "HasVK/WDDM: last window copy failed: %s\n", hasvk_wddm_error());
   if (w->pool) device->vk.dispatch_table.DestroyCommandPool(handle, w->pool, NULL);
   if (w->image) anv_DestroyImage(handle, w->image, NULL);
   if (w->bo) anv_device_release_bo(device, w->bo);
   memset(w, 0, sizeof(*w));
}

/* Window surface and its image for this HWND and extent. */
static VkResult window_create(struct anv_device *device, struct anv_wddm_window *w, HWND hwnd, VkExtent2D extent)
{
   uint64_t pitch = 0;
   VkResult result = anv_wddm_alloc_window_surface_bo(device, extent.width, extent.height, &w->bo, &w->share, &pitch);
   if (result != VK_SUCCESS) return result;
   w->hwnd = hwnd; w->extent = extent;

   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {extent.width, extent.height, 1},
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

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkDevice handle = anv_device_to_handle(device);
   result = device->vk.dispatch_table.CreateCommandPool(handle, &pool_info, NULL, &w->pool);
   if (result != VK_SUCCESS) return result;
   const VkCommandBufferAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = w->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   return device->vk.dispatch_table.AllocateCommandBuffers(handle, &alloc, &w->cmd);
}

VkResult anv_wddm_present_redirected(VkDevice handle, VkImage src, void *hwnd, VkExtent2D extent, void **state)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   struct anv_wddm_window *w = *state;
   if (!w) {
      w = vk_zalloc(&device->vk.alloc, sizeof(*w), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
      *state = w;
   }
   /* Window size and the client area's offset inside it, in the window's own
    * (possibly DPI-virtualized) coordinates. */
   RECT outer, client;
   POINT origin = {0, 0};
   if (!GetWindowRect(hwnd, &outer) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
      return VK_ERROR_SURFACE_LOST_KHR;
   const VkExtent2D window = {(uint32_t)MAX2(outer.right - outer.left, 1), (uint32_t)MAX2(outer.bottom - outer.top, 1)};
   const VkOffset3D offset = {origin.x - outer.left, origin.y - outer.top, 0};
   if (offset.x < 0 || offset.y < 0 || (uint32_t)offset.x >= window.width || (uint32_t)offset.y >= window.height)
      return VK_ERROR_OUT_OF_DATE_KHR;   /* minimized or unusual geometry: GDI takes the frame */
   const VkExtent3D copied = {MIN2(extent.width, window.width - offset.x), MIN2(extent.height, window.height - offset.y), 1};
   if (w->bo && (w->hwnd != hwnd || w->extent.width != window.width || w->extent.height != window.height))
      window_release(device, w);
   if (!w->bo) {
      VkResult result = window_create(device, w, hwnd, window);
      if (result != VK_SUCCESS) { window_release(device, w); return result; }
   }

   const struct vk_device_dispatch_table *d = &device->vk.dispatch_table;
   if (finish_pending(device, w)) return vk_errorf(device, VK_ERROR_DEVICE_LOST, "window copy: %s", hasvk_wddm_error());
   VkResult result = d->ResetCommandBuffer(w->cmd, 0);
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (result == VK_SUCCESS) result = d->BeginCommandBuffer(w->cmd, &begin);
   if (result != VK_SUCCESS) return result;
   const VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffset = offset, .extent = copied,
   };
   const VkCopyImageInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, .srcImage = src, .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .dstImage = w->image, .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL, .regionCount = 1, .pRegions = &region,
   };
   anv_CmdCopyImage2(w->cmd, &copy);
   result = d->EndCommandBuffer(w->cmd);
   if (result != VK_SUCCESS) return result;

   const uint64_t token = (uint64_t)(0x80000000u | (uint32_t)(uintptr_t)hwnd) << 32 | w->share;
   static int debug = -1;
   if (debug < 0) debug = getenv("HASVK_WDDM_PRESENT_DEBUG") != NULL;
   if (debug > 0 && debug++ < 4) {
      DWORD pid = 0; const DWORD owner = GetWindowThreadProcessId(hwnd, &pid);
      fprintf(stderr, "HasVK/WDDM present: hwnd=%p token=%#llx extent=%ux%u client=%ldx%ld window=%ldx%ld visible=%d iconic=%d "
              "owner thread %lu, this thread %lu, surface VA %#llx size %llu\n", hwnd, (unsigned long long)token, extent.width, extent.height,
              client.right, client.bottom, outer.right - outer.left, outer.bottom - outer.top, IsWindowVisible(hwnd), IsIconic(hwnd),
              owner, GetCurrentThreadId(), (unsigned long long)w->bo->offset, (unsigned long long)w->bo->size);
   }
   struct anv_wddm_device_state *ws = device->wddm;
   /* Not waited for here: the next present (or the release) waits for it. */
   struct anv_cmd_buffer *cmd = anv_cmd_buffer_from_handle(w->cmd);
   struct vk_command_buffer *one = &cmd->vk;
   uint64_t seq = 0;
   mtx_lock(&ws->submit_mutex);
   const int failed = anv_wddm_execute_commands(device, &one, 1, 0, token, true, &seq);
   mtx_unlock(&ws->submit_mutex);
   cmd->wddm->seq = 0;   /* verified by finish_pending, not by the queue's completion thread */
   if (!failed) w->pending_seq = seq;
   static int sync_present = -1;
   if (sync_present < 0) sync_present = getenv("HASVK_WDDM_SYNC_PRESENT") != NULL;
   if (!failed && sync_present && finish_pending(device, w)) return VK_ERROR_DEVICE_LOST;
   if (failed) {
      /* A present the kernel refuses (e.g. STATUS_GRAPHICS_PRESENT_OCCLUDED
       * while the display sleeps) is not a lost device; GDI takes this frame. */
      static unsigned reported;
      if (reported++ < 3) fprintf(stderr, "HasVK/WDDM redirected present: %s\n", hasvk_wddm_error());
      return VK_ERROR_OUT_OF_DATE_KHR;
   }
   return VK_SUCCESS;
}

void anv_wddm_present_redirected_finish(VkDevice handle, void *state)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   struct anv_wddm_window *w = state;
   if (!w) return;
   window_release(device, w);
   vk_free(&device->vk.alloc, w);
}
#endif
