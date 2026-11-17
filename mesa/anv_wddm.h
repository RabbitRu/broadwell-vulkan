/* Private Windows platform state for the HasVK port. */
#pragma once
#include "anv_private.h"
#include "wddm/hasvk_wddm.h"
#include "util/cnd_monotonic.h"
#include "util/vma.h"

/* One WDDM allocation is at most 128 MiB (transport profile). The device-wide
 * budget is what the single Vulkan heap reports. */
#define ANV_WDDM_MAX_BO_BYTES (1ull << 30)   /* = hasvk::wddm::kMaxLinearAllocationBytes */
#define ANV_WDDM_HEAP_BYTES (3ull << 30)
#define ANV_WDDM_MAX_BOS 65536u
#define ANV_WDDM_EVENT_SLOTS 4096u   /* 8 bytes each in one BO */

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
   /* BO ids index the per-batch dependency bitsets, so they are recycled
    * rather than growing with every allocation the device ever made. */
   BITSET_WORD *bo_ids;
   uint32_t bo_id_hint;
   struct anv_bo *event_bo;
   BITSET_WORD event_slots[BITSET_WORDS(ANV_WDDM_EVENT_SLOTS)];
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
/* vkCmdWaitEvents outside a render pass ends the batch there; the queue thread
 * waits for the events on the CPU and resumes the batch at resume. */
struct anv_wddm_wait {
   struct anv_address resume;
   uint32_t event_count;
   struct anv_event **events;
};
struct anv_wddm_cmd_state {
   struct anv_bo *bo;
   struct anv_wddm_cmd_chunk *chunks;
   uint32_t chunk_count, chunk_capacity;
   struct anv_wddm_wait *waits;
   uint32_t wait_count, wait_capacity;
};
int anv_wddm_execute_command(struct anv_cmd_buffer *cmd);
#ifdef HASVK_WDDM_GRAPHICS
void anv_wddm_cmd_state_finish(struct anv_cmd_buffer *cmd);
void anv_wddm_init_border_colors(struct anv_device *device);
struct anv_address anv_wddm_event_address(struct anv_device *device, const struct anv_event *event);
void anv_wddm_cmd_wait_events(struct anv_cmd_buffer *cmd, uint32_t count, const VkEvent *events);
void anv_wddm_upstream_capabilities(const struct anv_physical_device *pdevice, struct vk_device_extension_table *ext,
                                    struct vk_features *features, struct vk_properties *props);
#endif

static inline struct anv_wddm_device_state *
anv_wddm_state(struct vk_device *device)
{
   return container_of(device, struct anv_device, vk)->wddm;
}
