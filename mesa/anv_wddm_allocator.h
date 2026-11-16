/* Windows backing for the upstream HasVK state allocator.
 * Included only by anv_allocator.c after its private type definitions.
 * Softpin block pools retain separate, stable Lock2 mappings per real BO.
 */
#pragma once
#include <windows.h>

static VkResult
anv_state_table_expand_range(struct anv_state_table *table, uint32_t size)
{
   if (!size || size > BLOCK_POOL_MEMFD_SIZE)
      return vk_error(table->device, VK_ERROR_OUT_OF_HOST_MEMORY);
   size = align(size, 65536);
   if (!VirtualAlloc(table->map, size, MEM_COMMIT, PAGE_READWRITE))
      return vk_error(table->device, VK_ERROR_OUT_OF_HOST_MEMORY);
   table->size = size;
   return VK_SUCCESS;
}

VkResult
anv_state_table_init(struct anv_state_table *table, struct anv_device *device, uint32_t initial_entries)
{
   table->device = device; table->fd = -1; table->size = 0; table->state.u64 = 0;
   table->map = VirtualAlloc(NULL, BLOCK_POOL_MEMFD_SIZE, MEM_RESERVE, PAGE_READWRITE);
   if (!table->map) return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   VkResult result = anv_state_table_expand_range(table, initial_entries * ANV_STATE_ENTRY_SIZE);
   if (result != VK_SUCCESS) { VirtualFree(table->map, 0, MEM_RELEASE); table->map = NULL; }
   return result;
}

void
anv_state_table_finish(struct anv_state_table *table)
{
   if (table->map) VirtualFree(table->map, 0, MEM_RELEASE);
   table->map = NULL;
}

static VkResult
anv_block_pool_expand_range(struct anv_block_pool *pool, uint32_t center, uint32_t size)
{
   if (center || size <= pool->size || size > BLOCK_POOL_MEMFD_SIZE || pool->nbos >= ANV_MAX_BLOCK_POOL_BOS)
      return vk_error(pool->device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   struct anv_bo *bo;
   VkResult result = anv_device_alloc_bo(pool->device, pool->name, size - pool->size,
      ANV_BO_ALLOC_CAPTURE | ANV_BO_ALLOC_FIXED_ADDRESS | ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_SNOOPED,
      intel_48b_address(pool->start_address + pool->size), &bo);
   if (result != VK_SUCCESS) return result;
   pool->bos[pool->nbos++] = bo;
   pool->bo = pool->bos[0]; pool->size = size;
   return VK_SUCCESS;
}

VkResult
anv_block_pool_init(struct anv_block_pool *pool, struct anv_device *device,
                    const char *name, uint64_t address, uint32_t initial_size)
{
   pool->name = name; pool->device = device; pool->use_relocations = false;
   pool->nbos = 0; pool->size = 0; pool->center_bo_offset = 0;
   pool->start_address = intel_canonical_address(address); pool->map = NULL;
   pool->bo = NULL; pool->fd = -1; pool->state.u64 = pool->back_state.u64 = 0;
   VkResult result = anv_block_pool_expand_range(pool, 0, initial_size);
   if (result == VK_SUCCESS) pool->state.end = pool->size;
   return result;
}

void
anv_block_pool_finish(struct anv_block_pool *pool)
{
   anv_block_pool_foreach_bo(bo, pool) {
      assert(bo->refcount == 1);
      anv_device_release_bo(pool->device, bo);
   }
   pool->nbos = 0; pool->bo = NULL;
}
