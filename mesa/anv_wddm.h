/* Private Windows platform state for the HasVK port. */
#pragma once
#include "anv_private.h"
#include "wddm/hasvk_wddm.h"
#include "util/cnd_monotonic.h"
#include "util/vma.h"
#include "util/list.h"
#include "c11/threads.h"

/* One WDDM allocation is at most 128 MiB (transport profile). The device-wide
 * budget is what the single Vulkan heap reports. */
#define ANV_WDDM_MAX_BO_BYTES (1ull << 30)   /* = hasvk::wddm::kMaxLinearAllocationBytes */
#define ANV_WDDM_HEAP_BYTES (3ull << 30)
#define ANV_WDDM_INTERNAL_BYTES (1ull << 30)
/* The heap size reported and enforced: ANV_WDDM_HEAP_BYTES, or
 * HASVK_WDDM_HEAP_MB (64 MiB .. 3 GiB) for out-of-memory tests that must not
 * exhaust the machine's own memory. */
static inline uint64_t
anv_wddm_heap_bytes(void)
{
   static uint64_t bytes;
   if (!bytes) {
      const char *mb = getenv("HASVK_WDDM_HEAP_MB");
      uint64_t value = mb ? strtoull(mb, NULL, 10) << 20 : ANV_WDDM_HEAP_BYTES;
      if (value < (64ull << 20)) value = 64ull << 20;
      if (value > ANV_WDDM_HEAP_BYTES) value = ANV_WDDM_HEAP_BYTES;
      bytes = value;
   }
   return bytes;
}
#define ANV_WDDM_MAX_BOS 65536u
#define ANV_WDDM_EVENT_SLOTS 4096u   /* 8 bytes each in one BO */
#define ANV_WDDM_ENTRY_SLOTS 64u     /* 128 bytes each in the 8192-byte entry BO */

struct anv_wddm_device_state {
   struct hasvk_wddm_device *transport;
   struct hasvk_wddm_bo *entry, *completion;
   uint64_t reservation, origin, completed;
   mtx_t mutex;
   /* Serializes kernel submissions (the shared entry BO): the queue thread
    * and windowed presents from application threads. */
   mtx_t submit_mutex;
   struct u_cnd_monotonic condition;
   struct util_vma_heap client_heap;
   struct util_vma_heap state_heap;
   bool client_heap_initialized;
   bool custom_border_colors_initialized;
   bool device_mutex_initialized;
   uint32_t initialized_pools;
   /* All BOs; of them, application memory (vkAllocateMemory, name "user"),
    * which alone is limited by the reported heap. Driver-internal BOs (state
    * pools, command blocks, query and descriptor pools) have their own limit,
    * ANV_WDDM_INTERNAL_BYTES: an application that fills the heap can still
    * record and submit work. */
   uint64_t allocated_bytes, user_bytes;
   uint32_t allocation_count;
   /* Sizes of the internal and default pipeline cache data loaded from disk
    * (anv_wddm_device.c, cache_file_*). */
   size_t internal_cache_loaded, default_cache_loaded;
   /* BO ids index the per-batch dependency bitsets, so they are recycled
    * rather than growing with every allocation the device ever made. */
   BITSET_WORD *bo_ids;
   uint32_t bo_id_hint;
   struct anv_bo *event_bo;
   BITSET_WORD event_slots[BITSET_WORDS(ANV_WDDM_EVENT_SLOTS)];
   uint64_t command_batches_completed;
   uint64_t runs;   /* kernel submissions of command buffers, for chaining */
   /* The entry BO is a ring of 128-byte slots (jump at +0, completion write
    * at +64); a slot is reused only after the transport submission that last
    * used it completed (slot_seq). Protected by submit_mutex. */
   uint64_t slot_seq[ANV_WDDM_ENTRY_SLOTS];
   uint32_t next_slot;
   uint32_t next_serial;
   uint32_t last_slot;   /* slot of the last kernel submission (submit_mutex) */
   /* Non-blocking queue: a vkQueueSubmit's last kernel submission is not
    * waited for; the completion thread waits for it, verifies markers and
    * guards, then signals its syncs. pending (oldest first), verified_seq and
    * completion_exit are protected by mutex and announced on condition. */
   struct list_head pending;
   uint64_t verified_seq;
   /* HASVK_WDDM_STATS=1: command stream volume, printed every 5 s by submit(). */
   uint64_t stats_command_bytes, stats_command_buffers, stats_submits, stats_last_ms;
   thrd_t completion_thread;
   bool completion_thread_started, completion_exit;
};
struct anv_wddm_pending {
   struct list_head link;
   uint64_t seq;        /* transport submission number */
   uint32_t serial;     /* value of the completion write */
   uint32_t slot;       /* entry slot of its last kernel submission (timeline) */
   uint32_t command_count, signal_count;
   struct anv_cmd_buffer **commands;
   struct vk_sync_signal *signals;
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
   /* Chunks beyond the first, kept across resets for reuse (released at
    * destroy): allocating a WDDM buffer and waiting for its residency on every
    * re-recording cost the application's render thread ~6 % in Talos. */
   struct anv_bo **spare;
   uint32_t spare_count, spare_capacity;
   struct anv_wddm_wait *waits;
   uint32_t wait_count, wait_capacity;
   /* Last 16 bytes of a recorded primary: MI_BATCH_BUFFER_END, patched at
    * submission into a jump to the next command buffer of the same kernel
    * submission or to the queue's completion write. */
   uint32_t *tail;
   /* Kernel submission ("run") this buffer's final segment last joined. */
   uint64_t run;
   /* Transport submission of its last non-blocking execution, until verified. */
   uint64_t seq;
   /* Window copy recorded for a redirected present: the kernel submission
    * that ends with this buffer carries the token. If the kernel refuses the
    * present, the commands run without it and *present_refused is set. */
   uint64_t present_token;
   int *present_refused;
   /* HASVK_WDDM_PROFILE_PASSES (anv_wddm_profile.c): timestamp slot + 1 of
    * the open render pass; since vkBeginCommandBuffer: draws, the bound
    * graphics pipeline's kill_pixel (1 no, 2 yes) and how often it changed. */
   uint32_t profile_slot, profile_draws, profile_kill, profile_kill_switches, profile_cmd_slot;
   /* The open render pass (HASVK_WDDM_DUMP_DIR shadow map dumps). */
   uint64_t profile_framebuffer, profile_render_pass;
   uint32_t profile_width, profile_height;
};
/* Executes command buffers in order, chaining them into as few kernel
 * submissions as their event waits and repetitions allow. serial != 0: the
 * last one jumps to a completion write of serial (completion BO dword 0).
 * async: the last kernel submission is not waited for; *seq receives its
 * transport number and the caller must verify the command buffers
 * (anv_wddm_check_completed) after it completed. Otherwise everything is
 * verified before returning. present_token != 0: the last kernel submission
 * is a redirected present and executable (not only pending) command buffers
 * are accepted (driver-internal use).
 * Caller holds submit_mutex. */
int anv_wddm_execute_commands(struct anv_device *device, struct vk_command_buffer *const *commands, uint32_t count,
                              uint32_t serial, uint64_t present_token, bool async, uint64_t *seq);
int anv_wddm_execute_command(struct anv_cmd_buffer *cmd, uint64_t present_token);
int anv_wddm_check_completed(struct anv_cmd_buffer *cmd);
/* Waits until every non-blocking submission up to seq has been verified. */
void anv_wddm_wait_verified(struct anv_device *device, uint64_t seq);
VkResult anv_wddm_alloc_window_surface_bo(struct anv_device *device, uint32_t width, uint32_t height,
                                          struct anv_bo **out, uint32_t *share, uint64_t *pitch);
#ifdef HASVK_WDDM_GRAPHICS
VkCommandBuffer anv_wddm_present_redirected_record(VkDevice device, VkQueue queue, VkImage image, uint32_t index,
                                                   void *hwnd, VkExtent2D extent, void **state);
void anv_wddm_present_redirected_finish(VkDevice device, void *state);
#endif
#ifdef HASVK_WDDM_GRAPHICS
void anv_wddm_profile_install(struct vk_device_dispatch_table *dispatch);
void anv_wddm_profile_report(struct anv_device *device);
void anv_wddm_capture_pipelines(const VkGraphicsPipelineCreateInfo *infos, uint32_t count, const VkPipeline *pipelines);
void anv_wddm_capture_draw(struct anv_cmd_buffer *cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                           int32_t vertex_offset, uint32_t first_instance);
void anv_wddm_capture_end_command_buffer(struct anv_cmd_buffer *cmd);
void anv_wddm_capture_poll(void);
void anv_wddm_capture_pass_begin(struct anv_cmd_buffer *cmd, const VkRenderPassBeginInfo *info);
void anv_wddm_capture_pass_end(struct anv_cmd_buffer *cmd);
void anv_wddm_capture_execute(struct anv_cmd_buffer *cmd, uint32_t count);
void anv_wddm_cmd_state_finish(struct anv_cmd_buffer *cmd);
void anv_wddm_init_border_colors(struct anv_device *device);
struct anv_address anv_wddm_event_address(struct anv_device *device, const struct anv_event *event);
void anv_wddm_cmd_wait_events(struct anv_cmd_buffer *cmd, uint32_t count, const VkEvent *events);
int anv_wddm_flush_state_pools(struct anv_device *device);
void anv_wddm_upstream_capabilities(const struct anv_physical_device *pdevice, struct vk_device_extension_table *ext,
                                    struct vk_features *features, struct vk_properties *props);
#endif

/* HASVK_WDDM_TIMELINE=<file.csv> (anv_wddm_profile.c): a log of what each
 * thread does around the frame, written when the device is destroyed — the
 * application's Vulkan calls that can block (submit, present, acquire, fence
 * and query waits, allocations), the queue thread's semaphore waits and kernel
 * submissions, the completion thread's wake-ups, and the GPU's own start and
 * end time of every kernel submission (TIMESTAMP stored by the entry slot,
 * ANV_WDDM_TIMELINE_START/END bytes into it). Independent of
 * HASVK_WDDM_PROFILE_PASSES, whose per-pass timestamps serialize the GPU. */
#define ANV_WDDM_TIMELINE_START 112u
#define ANV_WDDM_TIMELINE_END 116u
enum anv_wddm_timeline_type {
   ANV_TL_CALL_BEGIN = 1,   /* a: anv_wddm_timeline_call; b: thread cycles (frame calls) */
   ANV_TL_CALL_END,         /* a: call; b: VkResult */
   ANV_TL_SUBMIT_BEGIN,     /* queue thread enters submit(): a: command buffers, b: waits */
   ANV_TL_SUBMIT_WAITED,    /* after its CPU semaphore waits: a: waits done on the CPU */
   ANV_TL_KERNEL_BEGIN,     /* a: serial, b: 1 if a present */
   ANV_TL_KERNEL_END,       /* a: serial, b: thread cycles */
   ANV_TL_GPU_DONE,         /* completion thread woke: a: serial, b: GPU start | end << 32 (ticks) */
   ANV_TL_SIGNALED,         /* a: serial, b: thread cycles */
   ANV_TL_CALIBRATE,        /* a: rdtsc, b: FILETIME (local) */
};
enum anv_wddm_timeline_call {
   ANV_TL_QUEUE_SUBMIT = 1, ANV_TL_QUEUE_PRESENT, ANV_TL_ACQUIRE, ANV_TL_WAIT_FENCES, ANV_TL_FENCE_STATUS,
   ANV_TL_QUERY_RESULTS, ANV_TL_QUEUE_WAIT_IDLE, ANV_TL_DEVICE_WAIT_IDLE, ANV_TL_ALLOCATE, ANV_TL_MAP,
   ANV_TL_RESET_FENCES, ANV_TL_WAIT_SEMAPHORES, ANV_TL_CREATE_PIPELINES, ANV_TL_FREE, ANV_TL_RESET_POOL,
};
extern bool anv_wddm_timeline_on;
bool anv_wddm_timeline_enabled(void);
void anv_wddm_timeline_event(uint32_t type, uint64_t a, uint64_t b);
uint64_t anv_wddm_timeline_thread_cycles(void);
void anv_wddm_timeline_write(void);

static inline struct anv_wddm_device_state *
anv_wddm_state(struct vk_device *device)
{
   return container_of(device, struct anv_device, vk)->wddm;
}
