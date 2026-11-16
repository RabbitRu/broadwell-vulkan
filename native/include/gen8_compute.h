#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Test-only state generation for a fresh context and bounded 1D dispatch.
 * HasVK integration will use its normal pipeline/state generation instead. */
struct hasvk_gen8_compute_info {
   uint64_t instruction_base, dynamic_base, surface_base, output_base, scratch_base;
   uint32_t kernel_offset, simd_width, max_threads, per_thread_regs;
   uint32_t output_bytes, dispatch_threads, right_mask, group_count_x;
   uint32_t instruction_offset, dynamic_offset, surface_offset;
};
size_t hasvk_gen8_constant_compute(const struct hasvk_gen8_compute_info *info,
                                  void *commands, size_t command_capacity,
                                  void *dynamic_state, void *surface_state);
size_t hasvk_gen8_batch_jump(void *commands, uint64_t target);
#ifdef __cplusplus
}
#endif
