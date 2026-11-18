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

VkCommandBuffer anv_wddm_present_redirected_record(VkDevice handle, VkQueue queue, VkImage src, uint32_t index,
                                                   void *hwnd, VkExtent2D extent, void **state)
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
   if (w->bo && (w->hwnd != hwnd || w->extent.width != window.width || w->extent.height != window.height || w->format != format)) {
      window_idle(device, w);   /* earlier copies still target the old surface */
      surface_release(device, w);
   }
   if (!w->bo && surface_create(device, w, hwnd, window, format) != VK_SUCCESS) { surface_release(device, w); return VK_NULL_HANDLE; }

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
   if (swap) {
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
   recorded->wddm->present_token = (uint64_t)(0x80000000u | (uint32_t)(uintptr_t)hwnd) << 32 | w->share;
   recorded->wddm->present_refused = &w->refused;
   static int debug = -1;
   if (debug < 0) debug = getenv("HASVK_WDDM_PRESENT_DEBUG") != NULL;
   if (debug > 0 && debug++ < 4)
      fprintf(stderr, "HasVK/WDDM present: hwnd=%p token=%#llx extent=%ux%u client=%ldx%ld window=%ldx%ld visible=%d iconic=%d\n",
              hwnd, (unsigned long long)recorded->wddm->present_token, extent.width, extent.height, client.right, client.bottom,
              outer.right - outer.left, outer.bottom - outer.top, IsWindowVisible(hwnd), IsIconic(hwnd));
   return cmd;
}

void anv_wddm_present_redirected_finish(VkDevice handle, void *state)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   struct anv_wddm_window *w = state;
   if (!w) return;
   window_idle(device, w);
   if (w->pool) device->vk.dispatch_table.DestroyCommandPool(handle, w->pool, NULL);
   surface_release(device, w);
   vk_free(&device->vk.alloc, w);
}
#endif
