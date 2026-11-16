/* Platform types for HasVK's Windows transport. No DRM ioctl implementation. */
#pragma once
#include "c11/threads.h"
#include <stdint.h>
typedef mtx_t pthread_mutex_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
   (void)attr; return mtx_init(m, mtx_plain) == thrd_success ? 0 : -1;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) { return mtx_lock(m) == thrd_success ? 0 : -1; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { return mtx_unlock(m) == thrd_success ? 0 : -1; }
static inline void pthread_mutex_destroy(pthread_mutex_t *m) { mtx_destroy(m); }
/* Linux transport declarations are retained as opaque types until those
 * source files are split; Windows operations use hasvk_wddm.h exclusively. */
struct drm_i915_gem_execbuffer2;
struct drm_i915_gem_relocation_entry;
#define EXEC_OBJECT_PINNED (1ull << 4) /* Internal legacy BO flag only. */
static inline uint64_t intel_canonical_address(uint64_t address)
{
   return (uint64_t)((int64_t)(address << 16) >> 16);
}
static inline uint64_t intel_48b_address(uint64_t address) { return address & 0xffffffffffffull; }
