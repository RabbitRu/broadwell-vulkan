/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
/* HasVK's Vulkan memory/buffer objects backed by the Windows transport.
 * Only ordinary, host-visible allocations are supported in this bring-up.
 * Import/export, sparse memory and capture/replay addresses remain disabled.
 */
#include "anv_wddm.h"

/* Common helper from anv_cmd_buffer.c; kept here until that unit is ported. */
#ifndef HASVK_WDDM_GRAPHICS
enum isl_format
anv_isl_format_for_descriptor_type(const struct anv_device *device, VkDescriptorType type)
{
   (void)device;
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return ISL_FORMAT_R32G32B32A32_FLOAT;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return ISL_FORMAT_RAW;
   default: UNREACHABLE("Invalid buffer descriptor type");
   }
}
#endif

/* Same ISL buffer-state path as anv_device.c, with the Windows BO address. */
void
anv_fill_buffer_surface_state(struct anv_device *device, struct anv_state state,
                              enum isl_format format, struct isl_swizzle swizzle,
                              isl_surf_usage_flags_t usage, struct anv_address address,
                              uint32_t range, uint32_t stride)
{
   isl_buffer_fill_state(&device->isl_dev, state.map,
      .address = anv_address_physical(address),
      .mocs = isl_mocs(&device->isl_dev, usage, address.bo && address.bo->is_external),
      .size_B = range, .format = format, .swizzle = swizzle, .stride_B = stride);
}

static VkResult
alloc_bo(struct anv_device *device, const char *name, uint64_t size, enum anv_bo_alloc_flags flags,
         uint64_t explicit_address, bool cpu_map, struct anv_bo **out);
VkResult
anv_device_alloc_bo(struct anv_device *device, const char *name, uint64_t size,
                    enum anv_bo_alloc_flags flags, uint64_t explicit_address,
                    struct anv_bo **out)
{
   return alloc_bo(device, name, size, flags, explicit_address, true, out);
}
/* cpu_map = false: GPU-only application memory (memory type 1), no CPU mapping. */
static VkResult
alloc_bo(struct anv_device *device, const char *name, uint64_t size, enum anv_bo_alloc_flags flags,
         uint64_t explicit_address, bool cpu_map, struct anv_bo **out)
{
   *out = NULL;
   const unsigned allowed = ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_SNOOPED |
      ANV_BO_ALLOC_CAPTURE | ANV_BO_ALLOC_CLIENT_VISIBLE_ADDRESS | ANV_BO_ALLOC_FIXED_ADDRESS;
   const bool fixed = (flags & ANV_BO_ALLOC_FIXED_ADDRESS) != 0;
   if ((flags & ~allowed) || (fixed ? !explicit_address || (explicit_address & 4095) : explicit_address != 0))
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
   if (!size || size > ANV_WDDM_MAX_BO_BYTES)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   size = align64(size, 4096);
   struct anv_bo *bo = vk_zalloc(&device->vk.alloc, sizeof(*bo), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!bo) return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex);
   if (!w->bo_ids) w->bo_ids = calloc(BITSET_WORDS(ANV_WDDM_MAX_BOS + 1), sizeof(BITSET_WORD));
   if (!w->bo_ids || w->allocation_count >= ANV_WDDM_MAX_BOS || size > ANV_WDDM_HEAP_BYTES - w->allocated_bytes) {
      mtx_unlock(&w->mutex); vk_free(&device->vk.alloc, bo);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   struct util_vma_heap *heap = fixed ? &w->state_heap : &w->client_heap;
   uint64_t address = fixed ? (util_vma_heap_alloc_addr(heap, explicit_address, size) ? explicit_address : 0) :
      util_vma_heap_alloc(heap, size, 4096);
   if (!address || (cpu_map ? hasvk_wddm_bo_create(w->transport, size, address, 1, &bo->wddm_bo)
                            : hasvk_wddm_bo_create_unmapped(w->transport, size, address, &bo->wddm_bo))) {
      if (address) util_vma_heap_free(heap, address, size);
      mtx_unlock(&w->mutex); vk_free(&device->vk.alloc, bo);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   bo->name = name; bo->size = size; bo->offset = intel_canonical_address(address);
   bo->refcount = 1; bo->flags = EXEC_OBJECT_PINNED;
   /* Dependency-list id, not a GEM handle: lowest free id after the last one handed out. */
   uint32_t id = w->bo_id_hint;
   do { id = id % ANV_WDDM_MAX_BOS + 1; } while (BITSET_TEST(w->bo_ids, id));
   BITSET_SET(w->bo_ids, id); w->bo_id_hint = id; bo->gem_handle = id;
   bo->has_fixed_address = fixed;
   bo->map = hasvk_wddm_bo_map(bo->wddm_bo);
   bo->has_client_visible_address = (flags & ANV_BO_ALLOC_CLIENT_VISIBLE_ADDRESS) != 0;
   /* CPU cache flushing around submissions, by how the BO is used: state pool
    * BOs are written by the CPU and only read by the GPU, and only up to the
    * pool's high-water mark (anv_wddm_flush_state_pools); command BOs are
    * flushed by anv_wddm_execute_command for the buffers it submits. */
   const size_t name_length = strlen(name);
   int tracking = HASVK_WDDM_FLUSH_BEFORE | HASVK_WDDM_FLUSH_AFTER;
   if ((name_length > 5 && !strcmp(name + name_length - 5, " pool")) || !strcmp(name, "command")) tracking = 0;
   if (tracking != (HASVK_WDDM_FLUSH_BEFORE | HASVK_WDDM_FLUSH_AFTER) && hasvk_wddm_bo_set_cpu_tracking(bo->wddm_bo, tracking)) {
      /* w->mutex is still held here. */
      hasvk_wddm_bo_destroy(bo->wddm_bo); util_vma_heap_free(heap, address, size); BITSET_CLEAR(w->bo_ids, id);
      mtx_unlock(&w->mutex);
      vk_free(&device->vk.alloc, bo); return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   w->allocated_bytes += size; ++w->allocation_count;
   mtx_unlock(&w->mutex);
   { static int profile = -1; if (profile < 0) profile = getenv("HASVK_WDDM_PROFILE") != NULL;
     if (profile) fprintf(stderr, "WDDM BO alloc %s %llu\n", name, (unsigned long long)size); }
   *out = bo;
   return VK_SUCCESS;
}

/* Shared window surface for redirected present (anv_wddm_present.c). Its GPU
 * address is chosen by the kernel, outside the device's VA heaps; it has no
 * CPU mapping. is_external marks both (and selects the external MOCS). */
VkResult
anv_wddm_alloc_window_surface_bo(struct anv_device *device, uint32_t width, uint32_t height,
                                 struct anv_bo **out, uint32_t *share, uint64_t *pitch)
{
   *out = NULL;
   struct anv_bo *bo = vk_zalloc(&device->vk.alloc, sizeof(*bo), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!bo) return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex);
   if (!w->bo_ids) w->bo_ids = calloc(BITSET_WORDS(ANV_WDDM_MAX_BOS + 1), sizeof(BITSET_WORD));
   if (!w->bo_ids || w->allocation_count >= ANV_WDDM_MAX_BOS ||
       hasvk_wddm_bo_create_window_surface(w->transport, width, height, &bo->wddm_bo) ||
       hasvk_wddm_bo_window_surface_info(bo->wddm_bo, share, pitch)) {
      if (bo->wddm_bo) hasvk_wddm_bo_destroy(bo->wddm_bo);
      mtx_unlock(&w->mutex); vk_free(&device->vk.alloc, bo);
      return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY, "window surface: %s", hasvk_wddm_error());
   }
   bo->name = "window surface"; bo->size = hasvk_wddm_bo_size(bo->wddm_bo);
   bo->offset = intel_canonical_address(hasvk_wddm_bo_address(bo->wddm_bo));
   bo->refcount = 1; bo->flags = EXEC_OBJECT_PINNED; bo->is_external = true;
   uint32_t id = w->bo_id_hint;
   do { id = id % ANV_WDDM_MAX_BOS + 1; } while (BITSET_TEST(w->bo_ids, id));
   BITSET_SET(w->bo_ids, id); w->bo_id_hint = id; bo->gem_handle = id;
   w->allocated_bytes += bo->size; ++w->allocation_count;
   mtx_unlock(&w->mutex);
   *out = bo;
   return VK_SUCCESS;
}

VkResult
anv_device_map_bo(struct anv_device *device, struct anv_bo *bo, uint64_t offset,
                  size_t size, uint32_t flags, void **out)
{
   *out = NULL;
   if (flags || offset > bo->size || size > bo->size - offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   *out = (char *)bo->map + offset;
   return VK_SUCCESS;
}

void
anv_device_unmap_bo(struct anv_device *device, struct anv_bo *bo, void *map, size_t size)
{
   /* Lock2 mapping belongs to the BO and is released at BO destruction. */
   (void)device; (void)bo; (void)map; (void)size;
}

void
anv_device_release_bo(struct anv_device *device, struct anv_bo *bo)
{
   if (!bo || !p_atomic_dec_zero(&bo->refcount)) return;
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex);
   if (hasvk_wddm_bo_destroy(bo->wddm_bo)) {
      fprintf(stderr, "HasVK/WDDM BO cleanup: %s\n", hasvk_wddm_error());
      vk_device_set_lost(&device->vk, "WDDM BO destruction failed");
   } else {
      if (!bo->is_external)   /* window surfaces: the kernel chose and frees the address */
         util_vma_heap_free(bo->has_fixed_address ? &w->state_heap : &w->client_heap, intel_48b_address(bo->offset), bo->size);
      w->allocated_bytes -= bo->size; --w->allocation_count; BITSET_CLEAR(w->bo_ids, bo->gem_handle);
   }
   mtx_unlock(&w->mutex);
   vk_free(&device->vk.alloc, bo);
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_AllocateMemory(VkDevice handle, const VkMemoryAllocateInfo *info,
                   const VkAllocationCallbacks *allocator, VkDeviceMemory *out)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   *out = VK_NULL_HANDLE;
   if (info->memoryTypeIndex >= device->physical->memory.type_count || !info->allocationSize ||
       info->allocationSize > ANV_WDDM_MAX_BO_BYTES)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   const struct anv_memory_type *type = &device->physical->memory.types[info->memoryTypeIndex];
   const bool host_visible = (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
   /* Unsupported allocation controls must not be silently accepted. */
   vk_foreach_struct_const(ext, info->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
         const VkMemoryDedicatedAllocateInfo *dedicated = (const void *)ext;
         if (dedicated->image && dedicated->buffer) return VK_ERROR_FEATURE_NOT_PRESENT;
      } else if (ext->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) {
         /* Device groups: one device, mask 1. Device addresses: every BO is pinned.
          * Capture/replay addresses are not supported. */
         const VkMemoryAllocateFlagsInfo *flags = (const void *)ext;
         if ((flags->flags & ~(VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT | VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)) ||
             ((flags->flags & VK_MEMORY_ALLOCATE_DEVICE_MASK_BIT) && flags->deviceMask != 1))
            return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
      } else if (ext->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
         if (((const VkExportMemoryAllocateInfo *)ext)->handleTypes) return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      } else if (ext->sType == VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA) {
         const struct wsi_memory_allocate_info *wsi = (const void *)ext;
         if (wsi->implicit_sync || wsi->dma_buf_sync_file) return VK_ERROR_FEATURE_NOT_PRESENT;
      } else {
         return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
      }
   }
   struct anv_device_memory *mem = vk_object_zalloc(&device->vk, allocator, sizeof(*mem), VK_OBJECT_TYPE_DEVICE_MEMORY);
   if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = alloc_bo(device, "user", info->allocationSize, host_visible ? ANV_BO_ALLOC_MAPPED : 0, 0,
                              host_visible, &mem->bo);
   if (result != VK_SUCCESS) { vk_object_free(&device->vk, allocator, mem); return result; }
   /* The CPU reaches application memory only through vkMapMemory: while it is
    * not mapped, submissions do not need to flush its cache lines. */
   if (hasvk_wddm_bo_set_cpu_tracking(mem->bo->wddm_bo, 0)) {
      anv_device_release_bo(device, mem->bo); vk_object_free(&device->vk, allocator, mem);
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   mem->type = type;
   mem->wddm_allocation_size = info->allocationSize;
   const VkMemoryDedicatedAllocateInfo *dedicated = vk_find_struct_const(info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   if (dedicated && dedicated->image) mem->dedicated_image = anv_image_from_handle(dedicated->image);
   *out = anv_device_memory_to_handle(mem);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
anv_FreeMemory(VkDevice handle, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   ANV_FROM_HANDLE(anv_device_memory, mem, memory);
   if (!mem) return;
   anv_device_release_bo(device, mem->bo);
   vk_object_free(&device->vk, allocator, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_MapMemory(VkDevice handle, VkDeviceMemory memory, VkDeviceSize offset,
              VkDeviceSize size, VkMemoryMapFlags flags, void **out)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   ANV_FROM_HANDLE(anv_device_memory, mem, memory);
   *out = NULL;
   if (!mem || mem->map || flags || offset >= mem->wddm_allocation_size || !mem->bo->map ||
       !(mem->type->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   if (size == VK_WHOLE_SIZE) size = mem->wddm_allocation_size - offset;
   if (!size || size > mem->wddm_allocation_size - offset)
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   /* Drop lines the CPU may still hold from before; from now on every
    * submission flushes this BO (HOST_COHERENT semantics). */
   if (hasvk_wddm_bo_flush(mem->bo->wddm_bo) ||
       hasvk_wddm_bo_set_cpu_tracking(mem->bo->wddm_bo, HASVK_WDDM_FLUSH_BEFORE | HASVK_WDDM_FLUSH_AFTER))
      return vk_error(device, VK_ERROR_MEMORY_MAP_FAILED);
   mem->map = mem->bo->map; mem->map_size = size; mem->map_delta = offset;
   *out = (char *)mem->map + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
anv_UnmapMemory(VkDevice handle, VkDeviceMemory memory)
{
   (void)handle; ANV_FROM_HANDLE(anv_device_memory, mem, memory);
   if (!mem) return;
   /* Write back what the CPU wrote, then stop flushing this BO per submission. */
   hasvk_wddm_bo_flush(mem->bo->wddm_bo);
   hasvk_wddm_bo_set_cpu_tracking(mem->bo->wddm_bo, 0);
   mem->map = NULL; mem->map_size = mem->map_delta = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_FlushMappedMemoryRanges(VkDevice handle, uint32_t count, const VkMappedMemoryRange *ranges)
{
   /* This configuration exposes only HOST_COHERENT memory. The transport
    * publishes CPU writes before submit and invalidates mappings after the
    * actual GPU completion event, including for wait-idle/fence consumers. */
   (void)handle; (void)count; (void)ranges; MemoryBarrier(); return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_InvalidateMappedMemoryRanges(VkDevice handle, uint32_t count, const VkMappedMemoryRange *ranges)
{
   (void)handle; (void)count; (void)ranges; MemoryBarrier(); return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
anv_GetDeviceMemoryCommitment(VkDevice handle, VkDeviceMemory memory, VkDeviceSize *size)
{
   (void)handle; ANV_FROM_HANDLE(anv_device_memory, mem, memory);
   *size = mem->wddm_allocation_size;
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_CreateBuffer(VkDevice handle, const VkBufferCreateInfo *info,
                 const VkAllocationCallbacks *allocator, VkBuffer *out)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   *out = VK_NULL_HANDLE;
   if (info->flags || !info->size || info->size > ANV_WDDM_MAX_BO_BYTES) return VK_ERROR_FEATURE_NOT_PRESENT;
   const VkBufferUsageFlags allowed = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
   if (info->usage & ~allowed) return VK_ERROR_FEATURE_NOT_PRESENT;
   struct anv_buffer *buffer = vk_buffer_create(&device->vk, info, allocator, sizeof(*buffer));
   if (!buffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
   buffer->address = ANV_NULL_ADDRESS;
   *out = anv_buffer_to_handle(buffer);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
anv_DestroyBuffer(VkDevice handle, VkBuffer buffer_handle, const VkAllocationCallbacks *allocator)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   ANV_FROM_HANDLE(anv_buffer, buffer, buffer_handle);
   if (buffer) vk_buffer_destroy(&device->vk, allocator, &buffer->vk);
}

VKAPI_ATTR void VKAPI_CALL
anv_GetBufferMemoryRequirements2(VkDevice handle, const VkBufferMemoryRequirementsInfo2 *info,
                                 VkMemoryRequirements2 *requirements)
{
   ANV_FROM_HANDLE(anv_device, device, handle); ANV_FROM_HANDLE(anv_buffer, buffer, info->buffer);
   requirements->memoryRequirements = (VkMemoryRequirements){
      .size = buffer->vk.size, .alignment = (buffer->vk.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? ANV_UBO_ALIGNMENT : 16,
      .memoryTypeBits = (1u << device->physical->memory.type_count) - 1,
   };
   vk_foreach_struct(ext, requirements->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->prefersDedicatedAllocation = dedicated->requiresDedicatedAllocation = false;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
anv_BindBufferMemory2(VkDevice handle, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   for (uint32_t i = 0; i < count; ++i) {
      ANV_FROM_HANDLE(anv_buffer, buffer, infos[i].buffer);
      ANV_FROM_HANDLE(anv_device_memory, mem, infos[i].memory);
      const uint64_t offset = infos[i].memoryOffset;
      const uint64_t alignment = (buffer->vk.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? ANV_UBO_ALIGNMENT : 16;
      if (!mem || offset % alignment || offset > mem->wddm_allocation_size ||
          buffer->vk.size > mem->wddm_allocation_size - offset)
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      buffer->address = (struct anv_address){.bo = mem->bo, .offset = offset};
      buffer->vk.device_address = anv_address_physical(buffer->address);
   }
   return VK_SUCCESS;
}

/* Integration diagnostics, not a Vulkan entry point. Validate the actual
 * descriptor and independently read the Gen8 surface's 64-bit base address. */
PUBLIC int hasvk_wddm_icd_check_buffer_descriptor(VkDescriptorSet handle, uint32_t binding,
   uint32_t element, VkBuffer buffer_handle, uint64_t offset, uint64_t range)
{
   ANV_FROM_HANDLE(anv_descriptor_set, set, handle);
   ANV_FROM_HANDLE(anv_buffer, buffer, buffer_handle);
   if (!set || !buffer || binding >= set->layout->binding_count) return -1;
   const struct anv_descriptor_set_binding_layout *layout = &set->layout->binding[binding];
   if (element >= layout->array_size) return -1;
   const struct anv_descriptor *descriptor = &set->descriptors[layout->descriptor_index + element];
   if (descriptor->buffer != buffer || descriptor->offset != offset || descriptor->range != range) return -1;
   if (vk_descriptor_type_is_dynamic(layout->type)) return 0;
   const struct anv_buffer_view *view = descriptor->set_buffer_view;
   uint64_t expected = anv_address_physical(anv_address_add(buffer->address, offset));
   if (!view || view->range != vk_buffer_range(&buffer->vk, offset, range) ||
       anv_address_physical(view->address) != expected) return -1;
   uint64_t surface_address;
   memcpy(&surface_address, (const char *)view->surface_state.map + 32, 8);
   return surface_address == expected ? 0 : -1;
}
/* Readback diagnostics only: shader execution is validated separately through
 * an SSBO. This independent CPU detile checks the actual image allocation. */
PUBLIC int hasvk_wddm_icd_read_image(VkImage handle, void *rgba, uint64_t bytes, uint32_t *tiling)
{
   ANV_FROM_HANDLE(anv_image, image, handle);
   if (!image || !rgba || !tiling || (image->vk.format != VK_FORMAT_R8G8B8A8_UNORM && image->vk.format != VK_FORMAT_D16_UNORM) ||
       image->planes[0].aux_usage != ISL_AUX_USAGE_NONE) return -1;
   const struct anv_surface *surface = &image->planes[0].primary_surface;
   const struct isl_surf *surf = &surface->isl;
   const struct anv_address address = anv_image_address(image, &surface->memory_range);
   const uint32_t width = image->vk.extent.width, height = image->vk.extent.height;
   const uint32_t pixel_bytes = image->vk.format == VK_FORMAT_D16_UNORM ? 2 : 4;
   if (!address.bo || !address.bo->map || bytes != (uint64_t)width * height * pixel_bytes || address.offset > address.bo->size ||
       surf->size_B > address.bo->size - address.offset) return -1;
   hasvk_wddm_bo_flush(address.bo->wddm_bo);   /* CPU read without vkMapMemory */
   const char *source = (const char *)address.bo->map + address.offset;
   if (surf->tiling == ISL_TILING_LINEAR) {
      for (uint32_t y = 0; y < height; ++y) memcpy((char *)rgba + y * width * pixel_bytes, source + y * surf->row_pitch_B, width * pixel_bytes);
   } else {
      isl_memcpy_tiled_to_linear(0, width * pixel_bytes, 0, height, rgba, source, width * pixel_bytes, surf->row_pitch_B, false, surf->tiling, ISL_MEMCPY);
   }
   *tiling = surf->tiling;
   return 0;
}
/* Diagnostics: CPU read of one mip level/layer of an uncompressed single-plane
 * image, located by ISL's layout. Reports where ISL places the level. */
PUBLIC int hasvk_wddm_icd_read_image_level(VkImage handle, uint32_t level, uint32_t layer, void *out, uint64_t bytes, uint32_t *position)
{
   ANV_FROM_HANDLE(anv_image, image, handle);
   if (!image || !out || !position || image->planes[0].aux_usage != ISL_AUX_USAGE_NONE) return -1;
   const struct anv_surface *surface = &image->planes[0].primary_surface;
   const struct isl_surf *surf = &surface->isl;
   const struct isl_format_layout *layout = isl_format_get_layout(surf->format);
   if (layout->bw != 1 || layout->bh != 1 || level >= surf->levels || layer >= surf->logical_level0_px.array_len) return -1;
   const uint32_t texel = layout->bpb / 8, width = u_minify(surf->logical_level0_px.width, level), height = u_minify(surf->logical_level0_px.height, level);
   const struct anv_address address = anv_image_address(image, &surface->memory_range);
   if (!address.bo || !address.bo->map || bytes != (uint64_t)width * height * texel) return -1;
   uint32_t x, y, z, array;
   isl_surf_get_image_offset_el(surf, level, layer, 0, &x, &y, &z, &array);
   if (z || array) return -1;
   position[0] = x; position[1] = y; position[2] = surf->row_pitch_B; position[3] = surf->tiling;
   position[4] = surf->image_alignment_el.width; position[5] = surf->image_alignment_el.height; position[6] = (uint32_t)surf->size_B;
   hasvk_wddm_bo_flush(address.bo->wddm_bo);   /* CPU read without vkMapMemory */
   const char *source = (const char *)address.bo->map + address.offset;
   if (surf->tiling == ISL_TILING_LINEAR) {
      for (uint32_t row = 0; row < height; ++row)
         memcpy((char *)out + row * width * texel, source + (size_t)(y + row) * surf->row_pitch_B + x * texel, width * texel);
   } else {
      isl_memcpy_tiled_to_linear(x * texel, (x + width) * texel, y, y + height, out, source, width * texel, surf->row_pitch_B, false, surf->tiling, ISL_MEMCPY);
   }
   return 0;
}

/* VK_KHR_maintenance4 / Vulkan 1.3: requirements of a buffer that is not created. */
VKAPI_ATTR void VKAPI_CALL
anv_GetDeviceBufferMemoryRequirements(VkDevice handle, const VkDeviceBufferMemoryRequirements *info,
                                      VkMemoryRequirements2 *requirements)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   const VkBufferCreateInfo *create = info->pCreateInfo;
   requirements->memoryRequirements = (VkMemoryRequirements){
      .size = create->size, .alignment = (create->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? ANV_UBO_ALIGNMENT : 16,
      .memoryTypeBits = (1u << device->physical->memory.type_count) - 1,
   };
   vk_foreach_struct(ext, requirements->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->prefersDedicatedAllocation = dedicated->requiresDedicatedAllocation = false;
      }
   }
}

/* Vulkan 1.2 core entry points for buffer-device-address capture/replay. Only
 * valid with bufferDeviceAddressCaptureReplay, which this port does not
 * expose (no fixed client addresses across processes). */
VKAPI_ATTR uint64_t VKAPI_CALL
anv_GetBufferOpaqueCaptureAddress(VkDevice handle, const VkBufferDeviceAddressInfo *info)
{
   (void)handle; (void)info; return 0;
}
VKAPI_ATTR uint64_t VKAPI_CALL
anv_GetDeviceMemoryOpaqueCaptureAddress(VkDevice handle, const VkDeviceMemoryOpaqueCaptureAddressInfo *info)
{
   (void)handle; (void)info; return 0;
}

/* As upstream anv_GetBufferDeviceAddress: the pinned GPU address of the bound
 * memory. (The runtime's common version returns vk_buffer::device_address,
 * which only drivers that fill it at bind time can use.) */
VKAPI_ATTR VkDeviceAddress VKAPI_CALL
anv_GetBufferDeviceAddress(VkDevice handle, const VkBufferDeviceAddressInfo *info)
{
   (void)handle; ANV_FROM_HANDLE(anv_buffer, buffer, info->buffer);
   assert(!anv_address_is_null(buffer->address));
   return anv_address_physical(buffer->address);
}
