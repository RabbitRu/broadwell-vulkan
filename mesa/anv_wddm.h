/* Private Windows platform state for the HasVK port. */
#pragma once
#include "anv_private.h"
#include "wddm/hasvk_wddm.h"
#include "util/cnd_monotonic.h"
#include "util/vma.h"

/* One WDDM allocation is at most 128 MiB (transport profile). The device-wide
 * budget is what the single Vulkan heap reports. */
#define ANV_WDDM_MAX_BO_BYTES (128ull << 20)
#define ANV_WDDM_HEAP_BYTES (2ull << 30)
#define ANV_WDDM_MAX_BOS 65536u

struct anv_wddm_device_state {
   struct hasvk_wddm_device *transport;
   struct hasvk_wddm_bo *entry, *completion;
   uint64_t reservation, origin, completed;
   mtx_t mutex;
   struct u_cnd_monotonic condition;
   struct util_vma_heap client_heap;
   struct util_vma_heap state_heap;
   bool client_heap_initialized;
   bool device_mutex_initialized;
   uint32_t initialized_pools;
   uint64_t allocated_bytes;
   uint32_t allocation_count;
   uint32_t next_bo_id;
   uint64_t command_batches_completed;
};

VkResult anv_wddm_pools_init(struct anv_device *device);
void anv_wddm_pools_finish(struct anv_device *device);
/* A command buffer is a chain of fixed-size BOs. chunks[0].bo == bo lives as
 * long as the command buffer; further chunks are released on reset. */
struct anv_wddm_cmd_chunk {
   struct anv_bo *bo;
   uint32_t length;
};
struct anv_wddm_cmd_state {
   struct anv_bo *bo;
   struct anv_wddm_cmd_chunk *chunks;
   uint32_t chunk_count, chunk_capacity;
};
int anv_wddm_execute_command(struct anv_cmd_buffer *cmd);
#ifdef HASVK_WDDM_GRAPHICS
void anv_wddm_cmd_state_finish(struct anv_cmd_buffer *cmd);
void anv_wddm_init_border_colors(struct anv_device *device);
#endif

static inline struct anv_wddm_device_state *
anv_wddm_state(struct vk_device *device)
{
   return container_of(device, struct anv_device, vk)->wddm;
}
