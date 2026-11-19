/* Initialization and hardware diagnostics for the real HasVK state pools. */
#include "anv_wddm.h"
#undef __gen_address_type
#undef __gen_user_data
#undef __gen_combine_address
#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data, location, address, delta) ((address) + (delta))
#include "genxml/gen80_pack.h"

static void pool_list(struct anv_device *device, struct anv_state_pool **pools)
{
   pools[0] = &device->general_state_pool;
   pools[1] = &device->dynamic_state_pool;
   pools[2] = &device->instruction_state_pool;
   pools[3] = &device->surface_state_pool;
   pools[4] = &device->binding_table_pool;
}

VkResult anv_wddm_pools_init(struct anv_device *device)
{
   struct anv_wddm_device_state *w = device->wddm;
   if (pthread_mutex_init(&device->mutex, NULL)) return VK_ERROR_OUT_OF_HOST_MEMORY;
   w->device_mutex_initialized = true;
   struct anv_state_pool *pools[5]; pool_list(device, pools);
   /* Dynamic state (where BLORP's vertex data lives) right below the client
    * heap at origin + 7 GiB, which is filled from its bottom: Gen8's VF cache
    * holds 32-bit addresses, and a vertex buffer slot whose bindings since the
    * last VF invalidation span more than 4 GiB costs a CS stall and an
    * invalidation (anv_gfx8_9_vb_cache_range_needs_workaround). With dynamic
    * state at 3 GiB every BLORP operation followed by a draw from application
    * memory did so (~14 per frame in The Talos Principle). */
   const uint64_t bases[5] = {w->origin, w->origin + (6ull << 30), w->origin + (3ull << 30),
      w->origin + (5ull << 30), w->origin + (5ull << 30)};
   const int32_t offsets[5] = {0x200000, 0, 0, 0, -(1 << 30)};
   const uint32_t blocks[5] = {16384, 16384, 16384, 4096, BINDING_TABLE_POOL_BLOCK_SIZE};
   const char *names[5] = {"general pool", "dynamic pool", "instruction pool", "surface state pool", "binding table pool"};
   for (unsigned i = 0; i < 5; ++i) {
      VkResult result = anv_state_pool_init(pools[i], device, names[i], bases[i], offsets[i], blocks[i]);
      if (result != VK_SUCCESS) return result;
      ++w->initialized_pools;
   }
   device->can_chain_batches = true;
   return VK_SUCCESS;
}

void anv_wddm_pools_finish(struct anv_device *device)
{
   struct anv_wddm_device_state *w = device->wddm;
   struct anv_state_pool *pools[5]; pool_list(device, pools);
   while (w->initialized_pools) anv_state_pool_finish(pools[--w->initialized_pools]);
   if (w->device_mutex_initialized) { pthread_mutex_destroy(&device->mutex); w->device_mutex_initialized = false; }
}

/* Write back CPU cache lines of every state pool before a submission. Only the
 * front of each block pool up to its high-water mark was ever handed out (the
 * port never allocates from the back), so nothing beyond it can be dirty.
 * Below the mark everything is flushed: states are rewritten after allocation
 * (descriptor updates, reused free-list entries). */
int anv_wddm_flush_state_pools(struct anv_device *device)
{
   struct anv_state_pool *pools[5]; pool_list(device, pools);
   for (unsigned p = 0; p < device->wddm->initialized_pools; ++p) {
      struct anv_block_pool *block_pool = &pools[p]->block_pool;
      struct anv_block_state state; state.u64 = p_atomic_read(&block_pool->state.u64);
      uint64_t start = 0;
      anv_block_pool_foreach_bo(bo, block_pool) {
         if (start >= state.next) break;
         const uint64_t bytes = MIN2(bo->size, state.next - start);
         if (hasvk_wddm_bo_flush_range(bo->wddm_bo, 0, bytes)) return -1;
         start += bo->size;
      }
   }
   return 0;
}

/* Diagnostic only: call on a fresh device before any queue submissions.
 * Reads Mesa suballocations through the GPU after growing both block pools
 * and CPU state tables. This is not a Vulkan command-buffer entry point. */
PUBLIC int hasvk_wddm_icd_test_state_pools(VkDevice handle, uint32_t *reads)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   if (!device || !reads || device->wddm->completed) return -1;
   *reads = 0;
   struct anv_wddm_device_state *w = device->wddm;
   struct anv_state_pool *pools[5]; pool_list(device, pools);
   enum { small_count = 2300, count = small_count + 1 };
   struct anv_state *all_states = vk_alloc(&device->vk.alloc, sizeof(*all_states) * count * 5, 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!all_states) return -1;
   unsigned allocated_pools = 0;
   int status = 0;
   uint32_t expected[90]; uint32_t read_count = 0;
   uint32_t *batch = hasvk_wddm_bo_map(w->entry), *next = batch;
   uint32_t *result = hasvk_wddm_bo_map(w->completion);
   for (unsigned p = 0; p < 5; ++p) {
      struct anv_state *states = all_states + p * count;
      for (unsigned i = 0; i < small_count; ++i) {
         states[i] = anv_state_pool_alloc(pools[p], 64, 64);
         memset(states[i].map, (p * 13 + i) & 255, 64);
      }
      void *first_map = states[0].map;
      states[small_count] = anv_state_pool_alloc(pools[p], 262145, 64);
      ++allocated_pools;
      memset(states[small_count].map, 0xa0 + p, states[small_count].alloc_size);
      bool valid = first_map == anv_block_pool_map(&pools[p]->block_pool,
         states[0].offset - pools[p]->start_offset, states[0].alloc_size) && pools[p]->table.size > 65536;
      for (unsigned i = 0; i < small_count; ++i) {
         const uint8_t *bytes = states[i].map;
         for (unsigned b = 0; b < 64; ++b) valid &= bytes[b] == ((p * 13 + i) & 255);
      }
      if (!valid) { status = -1; goto cleanup; }
      for (unsigned sample = 0; sample < 18; ++sample) {
         unsigned index = sample < 16 ? sample * (small_count - 1) / 15 : small_count;
         struct anv_state allocation = states[index];
         uint32_t offset = sample == 17 ? allocation.alloc_size - 4 : 0;
         memcpy(&expected[read_count], (char *)allocation.map + offset, 4);
         uint64_t source = intel_canonical_address(pools[p]->block_pool.start_address - pools[p]->start_offset + allocation.offset + offset);
         struct GFX8_MI_COPY_MEM_MEM copy = {GFX8_MI_COPY_MEM_MEM_header,
            .SourceMemoryAddress = source,
            .DestinationMemoryAddress = intel_canonical_address(hasvk_wddm_bo_address(w->completion) + 64 + read_count * 4),
         };
         GFX8_MI_COPY_MEM_MEM_pack(NULL, next, &copy); next += GFX8_MI_COPY_MEM_MEM_length;
         ++read_count;
      }
   }
   struct GFX8_MI_BATCH_BUFFER_END end = {GFX8_MI_BATCH_BUFFER_END_header};
   GFX8_MI_BATCH_BUFFER_END_pack(NULL, next++, &end);
   if ((next - batch) & 1) *next++ = 0;
   status = anv_wddm_flush_state_pools(device);
   if (!status) status = hasvk_wddm_submit_wait(w->transport, w->entry, 0, (next - batch) * 4);
   if (!status) {
      for (unsigned i = 1; i < 1024; ++i) {   /* dword 0 is the queue's completion counter */
         uint32_t wanted = i >= 16 && i < 16 + read_count ? expected[i - 16] : 0x39393939;
         if (result[i] != wanted) status = -1;
      }
   }
   memset(result + 1, 0x39, 4096 - 4);
cleanup:
   for (unsigned p = 0; p < allocated_pools; ++p)
      for (unsigned i = 0; i < count; ++i) anv_state_pool_free(pools[p], all_states[p * count + i]);
   if (!status) {
      // These buckets were not used above. Allocating them now splits a freed
      // large block and checks the biased general/binding-table offset path.
      for (unsigned p = 0; p < 5; ++p) {
         struct anv_state a = anv_state_pool_alloc(pools[p], 128, 128);
         struct anv_state b = anv_state_pool_alloc(pools[p], 256, 256);
         memset(a.map, 0x61, a.alloc_size); memset(b.map, 0xb2, b.alloc_size);
         if (a.map != anv_block_pool_map(&pools[p]->block_pool, a.offset - pools[p]->start_offset, a.alloc_size) ||
             b.map != anv_block_pool_map(&pools[p]->block_pool, b.offset - pools[p]->start_offset, b.alloc_size)) status = -1;
         for (unsigned i = 0; i < a.alloc_size; ++i) if (((uint8_t *)a.map)[i] != 0x61) status = -1;
         for (unsigned i = 0; i < b.alloc_size; ++i) if (((uint8_t *)b.map)[i] != 0xb2) status = -1;
         anv_state_pool_free(pools[p], b); anv_state_pool_free(pools[p], a);
      }
   }
   vk_free(&device->vk.alloc, all_states);
   if (!status) *reads = read_count;
   return status;
}
