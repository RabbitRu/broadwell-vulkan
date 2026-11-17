#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private C ABI for Mesa. Calls on a device are serialized. Submission waits
 * for actual GPU completion; this is not yet an asynchronous Vulkan queue.
 * A failed call returns -1 and sets a per-thread UTF-8 diagnostic. */
struct hasvk_wddm_device;
struct hasvk_wddm_bo;
struct hasvk_wddm_readonly_pool;
struct hasvk_wddm_adapter_info {
   uint64_t driver_version;
   uint32_t vendor_id, device_id, luid_low;
   int32_t luid_high;
};
const char *hasvk_wddm_error(void);
int hasvk_wddm_verify_no_umd(void);
int hasvk_wddm_device_create(struct hasvk_wddm_device **out);
int hasvk_wddm_device_destroy(struct hasvk_wddm_device *device);
int hasvk_wddm_device_get_info(struct hasvk_wddm_device *device, struct hasvk_wddm_adapter_info *out);
int hasvk_wddm_reserve(struct hasvk_wddm_device *device, uint64_t bytes, uint64_t requested, uint64_t *out);
int hasvk_wddm_release(struct hasvk_wddm_device *device, uint64_t address);
int hasvk_wddm_bo_create(struct hasvk_wddm_device *device, uint64_t bytes,
                        uint64_t requested, int writable, struct hasvk_wddm_bo **out);
int hasvk_wddm_bo_destroy(struct hasvk_wddm_bo *bo);
void *hasvk_wddm_bo_map(struct hasvk_wddm_bo *bo);
uint64_t hasvk_wddm_bo_address(const struct hasvk_wddm_bo *bo);
uint64_t hasvk_wddm_bo_size(const struct hasvk_wddm_bo *bo);
/* CPU-cache tracking, a mask: HASVK_WDDM_FLUSH_BEFORE (CPU writes the GPU must
 * see) and HASVK_WDDM_FLUSH_AFTER (GPU writes the CPU will read). New BOs use
 * both. For anything not covered the caller flushes explicitly. */
enum { HASVK_WDDM_FLUSH_BEFORE = 1, HASVK_WDDM_FLUSH_AFTER = 2 };
int hasvk_wddm_bo_set_cpu_tracking(struct hasvk_wddm_bo *bo, int mode);
int hasvk_wddm_bo_flush(struct hasvk_wddm_bo *bo);
int hasvk_wddm_bo_flush_range(struct hasvk_wddm_bo *bo, uint64_t offset, uint64_t bytes);
int hasvk_wddm_submit_wait(struct hasvk_wddm_device *device, struct hasvk_wddm_bo *commands,
                          uint32_t offset, uint32_t bytes);
/* Windowed present without D3D (see tools/test-present-redirect.ps1): a
 * shared B8G8R8A8 window surface, Y-tiled, pitch align(width * 4, 128),
 * height aligned to 32 rows, GPU-mapped, never CPU-mapped. A submission with
 * present_token = (0x80000000 | HWND) << 32 | share handle shows the surface
 * contents in that window once the submission completes. */
int hasvk_wddm_bo_create_window_surface(struct hasvk_wddm_device *device, uint32_t width, uint32_t height,
                                       struct hasvk_wddm_bo **out);
int hasvk_wddm_bo_window_surface_info(const struct hasvk_wddm_bo *bo, uint32_t *share, uint64_t *pitch);
int hasvk_wddm_submit_present(struct hasvk_wddm_device *device, struct hasvk_wddm_bo *commands,
                             uint32_t offset, uint32_t bytes, uint64_t present_token);
/* Stable contiguous CPU shadow for GPU-read-only state/instruction pools.
 * map commits on demand in 64 KiB chunks. submit_wait uploads every committed
 * chunk before submitting. Vulkan callers must keep submitted state immutable
 * through completion and serialize all CPU shadow writes against submit_wait
 * (the initial bring-up target is single-threaded). Scratch, queries and
 * application memory must use BOs. */
int hasvk_wddm_readonly_pool_create(struct hasvk_wddm_device *device, uint64_t address,
                                   uint32_t size, struct hasvk_wddm_readonly_pool **out);
int hasvk_wddm_readonly_pool_map(struct hasvk_wddm_readonly_pool *pool, uint32_t offset,
                                uint32_t bytes, void **out);
int hasvk_wddm_readonly_pool_destroy(struct hasvk_wddm_readonly_pool *pool);
#ifdef __cplusplus
}
#endif
