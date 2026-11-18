/* Windows platform for real HasVK objects, compute and Mesa runtime.
 * This experimental ICD is not yet a conformant Vulkan implementation.
 * Graphics pipelines and presentation remain disabled. */
#include "anv_wddm.h"
#include "compiler/glsl_types.h"
#include "compiler/elk/elk_eu.h"
#include "util/cnd_monotonic.h"
#include "util/os_time.h"
#undef NSEC_PER_SEC
#include "util/timespec.h"
#include "vk_sync_timeline.h"
#include <stdarg.h>

struct anv_wddm_sync {
   struct vk_sync vk;
   bool signaled;
};
static struct anv_wddm_device_state *state(struct vk_device *device)
{
   return anv_wddm_state(device);
}
static VkResult sync_init(struct vk_device *d, struct vk_sync *s, uint64_t initial)
{
   (void)d; ((struct anv_wddm_sync *)s)->signaled = initial != 0; return VK_SUCCESS;
}
static void sync_finish(struct vk_device *d, struct vk_sync *s) { (void)d; (void)s; }
static VkResult sync_signal(struct vk_device *d, struct vk_sync *s, uint64_t value)
{
   (void)value;
   struct anv_wddm_device_state *w = state(d);
   mtx_lock(&w->mutex); ((struct anv_wddm_sync *)s)->signaled = true;
   u_cnd_monotonic_broadcast(&w->condition); mtx_unlock(&w->mutex);
   return VK_SUCCESS;
}
static VkResult sync_reset(struct vk_device *d, struct vk_sync *s)
{
   struct anv_wddm_device_state *w = state(d);
   mtx_lock(&w->mutex); ((struct anv_wddm_sync *)s)->signaled = false; mtx_unlock(&w->mutex);
   return VK_SUCCESS;
}
static VkResult sync_move(struct vk_device *d, struct vk_sync *dst, struct vk_sync *src)
{
   struct anv_wddm_device_state *w = state(d);
   mtx_lock(&w->mutex);
   ((struct anv_wddm_sync *)dst)->signaled = ((struct anv_wddm_sync *)src)->signaled;
   ((struct anv_wddm_sync *)src)->signaled = false;
   u_cnd_monotonic_broadcast(&w->condition); mtx_unlock(&w->mutex);
   return VK_SUCCESS;
}
static VkResult sync_wait_many(struct vk_device *d, uint32_t count, const struct vk_sync_wait *waits,
                                enum vk_sync_wait_flags flags, uint64_t timeout)
{
   if (!count) return VK_SUCCESS;
   struct anv_wddm_device_state *w = state(d);
   mtx_lock(&w->mutex);
   for (;;) {
      if (vk_device_is_lost_no_report(d)) { mtx_unlock(&w->mutex); return VK_ERROR_DEVICE_LOST; }
      uint32_t ready = 0;
      for (uint32_t i = 0; i < count; ++i) ready += ((struct anv_wddm_sync *)waits[i].sync)->signaled;
      if ((flags & VK_SYNC_WAIT_ANY) ? ready != 0 : ready == count) { mtx_unlock(&w->mutex); return VK_SUCCESS; }
      if (timeout != UINT64_MAX && os_time_get_nano() >= timeout) { mtx_unlock(&w->mutex); return VK_TIMEOUT; }
      int result;
      if (timeout == UINT64_MAX) result = u_cnd_monotonic_wait(&w->condition, &w->mutex);
      else {
         struct timespec time; timespec_from_nsec(&time, timeout);
         result = u_cnd_monotonic_timedwait(&w->condition, &w->mutex, &time);
      }
      if (result != thrd_success && result != thrd_timedout) { mtx_unlock(&w->mutex); return VK_ERROR_DEVICE_LOST; }
   }
}
static const struct vk_sync_type wddm_sync_type = {
   .size = sizeof(struct anv_wddm_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_GPU_MULTI_WAIT |
      VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL |
      VK_SYNC_FEATURE_WAIT_ANY | VK_SYNC_FEATURE_WAIT_PENDING,
   .init = sync_init, .finish = sync_finish, .signal = sync_signal, .reset = sync_reset,
   .move = sync_move, .wait_many = sync_wait_many,
};
#ifdef HASVK_WDDM_GRAPHICS
/* Timeline semaphores: Mesa's emulation on top of the binary WDDM sync type
 * (as upstream HasVK without kernel timelines). The threaded queue waits for
 * timeline points and vk_queue_submit_final turns them into binary syncs. */
static struct vk_sync_timeline_type wddm_timeline_type;
static const struct vk_sync_type *wddm_sync_types[3];
#endif
static void compiler_log(void *data, unsigned *id, const char *format, ...)
{
   (void)data; (void)id;
   va_list arguments; va_start(arguments, format); vfprintf(stderr, format, arguments); va_end(arguments);
}
static const struct vk_instance_extension_table instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .EXT_debug_utils = true,
#ifdef HASVK_WDDM_GRAPHICS
   .KHR_surface = true, .KHR_win32_surface = true,
   .KHR_get_surface_capabilities2 = true, .KHR_surface_protected_capabilities = true, .EXT_debug_report = true,
   .KHR_device_group_creation = true, .KHR_external_fence_capabilities = true,
   .KHR_external_memory_capabilities = true, .KHR_external_semaphore_capabilities = true,
#endif
};
#ifdef HASVK_WDDM_GRAPHICS
static PFN_vkVoidFunction wddm_wsi_proc_addr(VkPhysicalDevice handle, const char *name)
{
   ANV_FROM_HANDLE(anv_physical_device, physical, handle);
   return vk_instance_get_proc_addr_unchecked(&physical->instance->vk, name);
}
#endif
static VkQueueFlags queue_flags(void)
{
   return VK_QUEUE_COMPUTE_BIT
#ifdef HASVK_WDDM_GRAPHICS
      | VK_QUEUE_GRAPHICS_BIT
#endif
      ;
}
static VkImageUsageFlags image_usage(void)
{
   return VK_IMAGE_USAGE_STORAGE_BIT
#ifdef HASVK_WDDM_GRAPHICS
      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
#endif
      ;
}
static void physical_destroy(struct vk_physical_device *base)
{
   struct anv_physical_device *physical = container_of(base, struct anv_physical_device, vk);
#ifdef HASVK_WDDM_GRAPHICS
   if (base->wsi_device) wsi_device_finish(&physical->wsi_device, &physical->instance->vk.alloc);
#endif
   ralloc_free(physical->compiler);
   glsl_type_singleton_decref();
   vk_physical_device_finish(base);
   vk_free(&physical->instance->vk.alloc, physical);
}
#ifdef HASVK_WDDM_GRAPHICS
/* Device extensions taken from upstream HasVK only after they were checked on
 * this port. Everything else upstream reports stays off. */
static const char *const verified_device_extensions[] = {
   /* promoted to Vulkan 1.1 */
   "VK_KHR_16bit_storage", "VK_KHR_bind_memory2", "VK_KHR_dedicated_allocation",
   "VK_KHR_descriptor_update_template", "VK_KHR_device_group", "VK_KHR_external_memory",
   "VK_KHR_external_semaphore", "VK_KHR_get_memory_requirements2", "VK_KHR_maintenance1",
   "VK_KHR_maintenance2", "VK_KHR_maintenance3", "VK_KHR_multiview", "VK_KHR_relaxed_block_layout",
   "VK_KHR_shader_draw_parameters", "VK_KHR_storage_buffer_storage_class", "VK_KHR_variable_pointers",
   /* small, self-contained */
   "VK_KHR_driver_properties", "VK_EXT_host_query_reset", "VK_KHR_sampler_mirror_clamp_to_edge",
   /* second batch: upstream code paths, no new port infrastructure (CTS-checked) */
   "VK_KHR_8bit_storage", "VK_KHR_shader_float16_int8", "VK_KHR_uniform_buffer_standard_layout", "VK_EXT_scalar_block_layout",
   "VK_KHR_shader_float_controls", "VK_KHR_spirv_1_4", "VK_KHR_vulkan_memory_model", "VK_KHR_shader_subgroup_extended_types",
   "VK_EXT_shader_viewport_index_layer", "VK_KHR_image_format_list", "VK_KHR_format_feature_flags2", "VK_EXT_separate_stencil_usage",
   "VK_EXT_4444_formats", "VK_EXT_index_type_uint8", "VK_KHR_create_renderpass2", "VK_KHR_depth_stencil_resolve",
   "VK_KHR_separate_depth_stencil_layouts", "VK_KHR_imageless_framebuffer", "VK_KHR_copy_commands2", "VK_KHR_draw_indirect_count",
   "VK_EXT_multi_draw", "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_KHR_maintenance4",
   /* emulated on the binary WDDM sync type */
   "VK_KHR_timeline_semaphore",
   /* waits inside rendering are detected by gfx.layer_count (anv_wddm_command.c) */
   "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2",
   /* A64 buffer access on pinned BO addresses */
   "VK_KHR_buffer_device_address",
   /* Required for Vulkan 1.3 (upstream HasVK implementation, CTS-checked on this port) */
   "VK_EXT_inline_uniform_block", "VK_EXT_private_data", "VK_EXT_subgroup_size_control", "VK_EXT_texel_buffer_alignment",
   "VK_KHR_zero_initialize_workgroup_memory", "VK_KHR_shader_integer_dot_product", "VK_EXT_pipeline_creation_cache_control",
   "VK_EXT_pipeline_creation_feedback", "VK_EXT_shader_demote_to_helper_invocation", "VK_KHR_shader_terminate_invocation",
   "VK_EXT_image_robustness", "VK_EXT_tooling_info", "VK_KHR_shader_non_semantic_info",
   /* Stage 3, batch A: shader, SPIR-V and pipeline-reporting extensions */
   "VK_EXT_shader_subgroup_ballot", "VK_EXT_shader_subgroup_vote", "VK_EXT_shader_atomic_float", "VK_KHR_shader_clock",
   "VK_KHR_shader_expect_assume", "VK_KHR_shader_relaxed_extended_instruction", "VK_KHR_shader_subgroup_uniform_control_flow",
   "VK_EXT_shader_replicated_composites", "VK_NV_compute_shader_derivatives", "VK_KHR_workgroup_memory_explicit_layout",
   "VK_INTEL_shader_integer_functions2", "VK_GOOGLE_decorate_string", "VK_GOOGLE_hlsl_functionality1", "VK_GOOGLE_user_type",
   "VK_EXT_shader_module_identifier", "VK_KHR_pipeline_executable_properties", "VK_EXT_debug_marker",
   "VK_KHR_deferred_host_operations", "VK_VALVE_mutable_descriptor_type",
   /* Stage 3, batch B: state, rasterization, descriptors, maintenance */
   "VK_EXT_border_color_swizzle", "VK_EXT_custom_border_color", "VK_EXT_color_write_enable", "VK_EXT_depth_clamp_control",
   "VK_EXT_depth_clamp_zero_one", "VK_EXT_depth_clip_enable", "VK_EXT_depth_clip_control", "VK_EXT_image_2d_view_of_3d",
   "VK_EXT_image_view_min_lod", "VK_EXT_line_rasterization", "VK_EXT_non_seamless_cube_map",
   "VK_EXT_primitive_topology_list_restart", "VK_EXT_provoking_vertex", 
   "VK_EXT_vertex_attribute_divisor", "VK_KHR_push_descriptor", "VK_KHR_maintenance5", 
   "VK_EXT_buffer_device_address",
   /* Stage 3, batch C: predication, transform feedback, generated-primitives query */
   "VK_EXT_conditional_rendering", "VK_EXT_transform_feedback", "VK_EXT_primitives_generated_query",
   /* Stage 3: off after CTS failures, fixed in the port (robust_buffer_access,
    * per-bind status, vkGetPhysicalDeviceMultisamplePropertiesEXT) */
   "VK_EXT_robustness2", "VK_KHR_robustness2", "VK_KHR_maintenance6", "VK_EXT_sample_locations",
   /* Stage 3: YCbCr sampling and multi-planar images */
   "VK_KHR_sampler_ycbcr_conversion", "VK_EXT_ycbcr_image_arrays",
};
/* Limits, features and extensions as upstream HasVK reports them for Gen8
 * (anv_device.c, built as anv_wddm_capabilities.c), then the limits of this
 * port: Vulkan 1.2, one WDDM allocation of at most 128 MiB, no external
 * handles, not a conformant implementation. */
static void apply_upstream_capabilities(struct anv_physical_device *physical)
{
   struct vk_device_extension_table upstream_extensions; struct vk_features upstream_features; struct vk_properties upstream;
   memset(&upstream_extensions, 0, sizeof(upstream_extensions)); memset(&upstream_features, 0, sizeof(upstream_features));
   memset(&upstream, 0, sizeof(upstream));
   anv_wddm_upstream_capabilities(physical, &upstream_extensions, &upstream_features, &upstream);
   struct vk_properties *p = &physical->vk.properties;
   const struct vk_properties ours = *p;
   *p = upstream;
   p->apiVersion = VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION);
   memcpy(p->deviceName, ours.deviceName, sizeof(p->deviceName));
   memcpy(p->pipelineCacheUUID, ours.pipelineCacheUUID, VK_UUID_SIZE);
   memcpy(p->deviceLUID, ours.deviceLUID, VK_LUID_SIZE); p->deviceLUIDValid = true; p->deviceNodeMask = 1;
   memcpy(p->driverUUID, "HASVK-WDDM-DRV01", VK_UUID_SIZE);
   memset(p->deviceUUID, 0, VK_UUID_SIZE);
   const uint16_t ids[2] = {0x8086, 0x162b}; memcpy(p->deviceUUID, ids, sizeof(ids)); memcpy(p->deviceUUID + 8, ours.deviceLUID, VK_LUID_SIZE);
   memset(&p->conformanceVersion, 0, sizeof(p->conformanceVersion));
   p->maxMemoryAllocationCount = ANV_WDDM_MAX_BOS;
   p->maxMemoryAllocationSize = ANV_WDDM_MAX_BO_BYTES;
   p->maxStorageBufferRange = MIN2(p->maxStorageBufferRange, (uint32_t)ANV_WDDM_MAX_BO_BYTES);
   p->maxUniformBufferRange = MIN2(p->maxUniformBufferRange, (uint32_t)ANV_WDDM_MAX_BO_BYTES);
   p->maxBufferSize = MIN2(p->maxBufferSize, ANV_WDDM_MAX_BO_BYTES);
   p->maxTexelBufferElements = MIN2(p->maxTexelBufferElements, (uint32_t)(ANV_WDDM_MAX_BO_BYTES / 16));
   p->sparseAddressSpaceSize = 0;
   physical->vk.supported_features = upstream_features;
   struct vk_features *f = &physical->vk.supported_features;
   f->protectedMemory = false;
   /* No address that stays valid across processes or runs. */
   f->bufferDeviceAddressCaptureReplay = false;
   f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = false;
   f->sparseResidencyAliased = false;
   struct vk_device_extension_table *e = &physical->vk.supported_extensions;
   memset(e, 0, sizeof(*e));
   for (unsigned i = 0; i < ARRAY_SIZE(verified_device_extensions); ++i)
      for (unsigned x = 0; x < VK_DEVICE_EXTENSION_COUNT; ++x)
         if (!strcmp(vk_device_extensions[x].extensionName, verified_device_extensions[i])) e->extensions[x] = upstream_extensions.extensions[x];
   e->KHR_swapchain = true;
   e->KHR_swapchain_mutable_format = true;
   /* HASVK_WDDM_EXTRA_EXTENSIONS=VK_EXT_a,VK_EXT_b: also enable these upstream
    * extensions (investigating ones that are off because CTS failed). */
   const char *extra = getenv("HASVK_WDDM_EXTRA_EXTENSIONS");
   if (extra)
      for (unsigned x = 0; x < VK_DEVICE_EXTENSION_COUNT; ++x) {
         const char *name = vk_device_extensions[x].extensionName, *at = extra;
         const size_t length = strlen(name);
         while ((at = strstr(at, name)) != NULL) {
            if ((at == extra || at[-1] == ',') && (at[length] == ',' || !at[length])) {
               e->extensions[x] = upstream_extensions.extensions[x];
               break;
            }
            at += length;
         }
      }
   /* YCbCr sampling (core feature of 1.1) goes with its extension. */
   f->samplerYcbcrConversion = f->samplerYcbcrConversion && e->KHR_sampler_ycbcr_conversion;
   /* HASVK_WDDM_LIST_EXTENSIONS=1: upstream HasVK extensions not (yet) enabled here. */
   if (getenv("HASVK_WDDM_LIST_EXTENSIONS"))
      for (unsigned x = 0; x < VK_DEVICE_EXTENSION_COUNT; ++x)
         if (upstream_extensions.extensions[x] && !e->extensions[x])
            fprintf(stderr, "HasVK/WDDM upstream-only extension: %s\n", vk_device_extensions[x].extensionName);
}
#endif
/* HASVK_WDDM_ASSERTS_TO_STDERR=1 (set by the test scripts): a failed assert or
 * abort writes to stderr and ends the process without a message box or a
 * Windows Error Reporting dialog, so unattended runs never wait for a click. */
static void unattended_asserts(void)
{
   static bool done;
   if (done || !getenv("HASVK_WDDM_ASSERTS_TO_STDERR")) return;
   done = true;
   _set_error_mode(_OUT_TO_STDERR);
   _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
static VkResult enumerate(struct vk_instance *base)
{
   unattended_asserts();
   struct anv_instance *instance = container_of(base, struct anv_instance, vk);
   struct hasvk_wddm_device *check = NULL;
   if (hasvk_wddm_device_create(&check)) {
      fprintf(stderr, "HasVK/WDDM: adapter rejected: %s\n", hasvk_wddm_error());
      return VK_SUCCESS; /* No compatible adapter, as required for ICD enumeration. */
   }
   struct hasvk_wddm_adapter_info adapter;
   int queried = hasvk_wddm_device_get_info(check, &adapter);
   if (hasvk_wddm_device_destroy(check)) return vk_error(instance, VK_ERROR_INITIALIZATION_FAILED);
   if (queried) return VK_ERROR_INITIALIZATION_FAILED;
   struct anv_physical_device *physical = vk_zalloc(&base->alloc, sizeof(*physical), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!physical) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct vk_physical_device_dispatch_table dispatch;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch, &anv_physical_device_entrypoints, true);
#ifdef HASVK_WDDM_GRAPHICS
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch, &wsi_physical_device_entrypoints, false);
#endif
   VkResult result = vk_physical_device_init(&physical->vk, base, NULL, NULL, NULL, &dispatch);
   if (result != VK_SUCCESS) { vk_free(&base->alloc, physical); return result; }
   physical->instance = instance;
   if (!intel_get_device_info_for_build(0x162b, &physical->info)) {
      vk_physical_device_finish(&physical->vk); vk_free(&base->alloc, physical); return VK_ERROR_INITIALIZATION_FAILED;
   }
   glsl_type_singleton_init_or_ref();
   physical->compiler = elk_compiler_create(NULL, &physical->info);
   if (!physical->compiler) { physical_destroy(&physical->vk); return VK_ERROR_OUT_OF_HOST_MEMORY; }
   physical->compiler->shader_debug_log = compiler_log;
   physical->compiler->shader_perf_log = compiler_log;
   /* As upstream without context isolation: INSTPM "CONSTANT_BUFFER Address
    * Offset Disable" is not set in a WDDM context (read back as 0x6080), so the
    * hardware adds the dynamic state base to push buffer 0. Without this, a UBO
    * range promoted to buffer 0 is read from the wrong address (CTS
    * binding_model.shader_access ... multiple_descriptor_sets in graphics stages). */
   physical->compiler->constant_buffer_0_is_relative = true;
   physical->compiler->supports_shader_constants = true;
   isl_device_init(&physical->isl_dev, &physical->info);
   physical->supports_48bit_addresses = physical->use_softpin = true;
#ifdef HASVK_WDDM_GRAPHICS
   /* Every BO has a fixed GPU address, as with softpin upstream: A64 buffer
    * access (buffer device addresses, bindless SSBO addresses) works. */
   physical->has_a64_buffer_access = true;
#endif
   physical->gtt_size = 16ull << 30;
   /* As upstream HasVK on LLC parts, in the other order: type 0 is host
    * visible (cacheable mapping; coherence by flushing around submissions),
    * type 1 is GPU-only memory without a CPU mapping. */
   physical->memory.heap_count = 1;
   physical->memory.type_count = 2;
   physical->memory.heaps[0] = (struct anv_memory_heap){.size = ANV_WDDM_HEAP_BYTES, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
   physical->memory.types[0] = (struct anv_memory_type){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      .heapIndex = 0,
   };
   physical->memory.types[1] = (struct anv_memory_type){.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .heapIndex = 0};
   physical->queue.family_count = 1;
   physical->queue.families[0] = (struct anv_queue_family){queue_flags(), 1, INTEL_ENGINE_CLASS_RENDER};
#ifdef HASVK_WDDM_GRAPHICS
   wddm_timeline_type = vk_sync_timeline_get_type(&wddm_sync_type);
   wddm_sync_types[0] = &wddm_sync_type; wddm_sync_types[1] = &wddm_timeline_type.sync; wddm_sync_types[2] = NULL;
   physical->vk.supported_sync_types = wddm_sync_types;
#else
   physical->sync_types[0] = &wddm_sync_type;
   physical->vk.supported_sync_types = physical->sync_types;
#endif
   physical->vk.pipeline_cache_import_ops = anv_cache_import_ops;
   struct vk_properties *p = &physical->vk.properties;
   p->apiVersion = VK_API_VERSION_1_0;
   p->driverVersion = VK_MAKE_VERSION(0, 1, 0);
   p->vendorID = 0x8086; p->deviceID = 0x162b; p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
   memcpy(p->deviceLUID, &adapter.luid_low, 4); memcpy(p->deviceLUID + 4, &adapter.luid_high, 4);
   p->deviceLUIDValid = true; p->deviceNodeMask = 1;
   snprintf(p->deviceName, sizeof(p->deviceName), "Intel Iris 6100 (HasVK/WDDM)");
   /* Bump when shader/descriptor ABI changes (00002: A64 buffer access). */
   memcpy(p->pipelineCacheUUID, "HASVK-WDDM-00002", VK_UUID_SIZE);
   p->maxMemoryAllocationCount = 4096;
   p->maxStorageBufferRange = 128u << 20;
   p->minMemoryMapAlignment = 4096; p->nonCoherentAtomSize = 64; p->bufferImageGranularity = 4096;
   p->minStorageBufferOffsetAlignment = 16; p->minUniformBufferOffsetAlignment = ANV_UBO_ALIGNMENT;
   p->timestampPeriod = 1;
   p->maxComputeWorkGroupInvocations = 64;
   p->maxComputeWorkGroupSize[0] = 64; p->maxComputeWorkGroupSize[1] = p->maxComputeWorkGroupSize[2] = 1;
   p->maxComputeWorkGroupCount[0] = 65535; p->maxComputeWorkGroupCount[1] = p->maxComputeWorkGroupCount[2] = 1;
   p->maxBoundDescriptorSets = MAX_SETS;
   p->maxPushConstantsSize = 128; p->maxUniformBufferRange = 65536;
   p->maxPerStageDescriptorStorageBuffers = p->maxDescriptorSetStorageBuffers = 64;
   p->maxPerStageDescriptorUniformBuffers = p->maxDescriptorSetUniformBuffers = 12;
   p->maxPerStageResources = 128;
   p->maxImageDimension2D = 4096; p->maxImageArrayLayers = 1;
   p->maxPerStageDescriptorStorageImages = p->maxDescriptorSetStorageImages = 1;
#ifdef HASVK_WDDM_GRAPHICS
   apply_upstream_capabilities(physical);
   result = wsi_device_init(&physical->wsi_device, anv_physical_device_to_handle(physical), wddm_wsi_proc_addr,
      &base->alloc, -1, NULL, &(struct wsi_device_options){.sw_device = false});
   if (result != VK_SUCCESS) { physical_destroy(&physical->vk); return result; }
   physical->vk.wsi_device = &physical->wsi_device;
   physical->wsi_device.supports_modifiers = false;
   physical->wsi_device.disable_unordered_submits = true;
   /* Windowed present by redirected submission (GPU copy into a window
    * surface, no CPU copy); HASVK_WDDM_PRESENT=gdi keeps the GDI path. */
   const char *present = getenv("HASVK_WDDM_PRESENT");
   if (!present || strcmp(present, "gdi")) {
      physical->wsi_device.win32.present_redirected_record = anv_wddm_present_redirected_record;
      physical->wsi_device.win32.present_redirected_finish = anv_wddm_present_redirected_finish;
   }
#endif
   list_addtail(&physical->vk.link, &base->physical_devices.list);
   return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_EnumerateInstanceVersion(uint32_t *version)
{
#ifdef HASVK_WDDM_GRAPHICS
   *version = VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION);
#else
   *version = VK_API_VERSION_1_0;
#endif
   return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *properties)
{
   if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
   return vk_enumerate_instance_extension_properties(&instance_extensions, count, properties);
}
VKAPI_ATTR VkResult VKAPI_CALL anv_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out)
{
   *out = VK_NULL_HANDLE;
   if (!allocator) allocator = vk_default_allocator();
   struct anv_instance *instance = vk_zalloc(allocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct vk_instance_dispatch_table dispatch;
   vk_instance_dispatch_table_from_entrypoints(&dispatch, &anv_instance_entrypoints, true);
#ifdef HASVK_WDDM_GRAPHICS
   vk_instance_dispatch_table_from_entrypoints(&dispatch, &wsi_instance_entrypoints, false);
#endif
   VkResult result = vk_instance_init(&instance->vk, &instance_extensions, &dispatch, info, allocator);
   if (result != VK_SUCCESS) { vk_free(allocator, instance); return result; }
   instance->vk.physical_devices.enumerate = enumerate;
   instance->vk.physical_devices.destroy = physical_destroy;
   /* driconf is not loaded on Windows, so instance->drirc keeps zeros. That is
    * the default of every hasvk option except lower_depth_range_rate (1.0): at
    * 0 the viewport depth translation (m32) was multiplied away, breaking
    * depth_clip_control and any viewport with minDepth != 0. */
   instance->drirc.quality.lower_depth_range_rate = 1.0f;
   process_intel_debug_variable();
   /* No CCS_D: on this GPU it only speeds up colour fast clears, and with it a
    * multisample resolve into mip level 3 came out wrong (CTS renderpass1
    * multisample_resolve.*_resolve_level_3). HiZ and MCS stay on. */
   BITSET_SET(intel_debug, DEBUG_NO_CCS);
   *out = anv_instance_to_handle(instance);
   return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL anv_DestroyInstance(VkInstance handle, const VkAllocationCallbacks *allocator)
{
   (void)allocator; ANV_FROM_HANDLE(anv_instance, instance, handle);
   if (!instance) return;
   vk_instance_finish(&instance->vk); vk_free(&instance->vk.alloc, instance);
}
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice handle, uint32_t *count, VkQueueFamilyProperties2 *properties)
{
   (void)handle;
   /* 36-bit TIMESTAMP register, as in HasVK; only meaningful with queries (graphics build). */
#ifdef HASVK_WDDM_GRAPHICS
   const uint32_t timestamp_bits = 36;
#else
   const uint32_t timestamp_bits = 0;
#endif
   if (properties && *count) properties[0].queueFamilyProperties = (VkQueueFamilyProperties){queue_flags(), 1, timestamp_bits, {1, 1, 1}};
   *count = properties && !*count ? 0 : 1;
}
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties *properties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical, handle);
   memset(properties, 0, sizeof(*properties));
   properties->memoryHeapCount = physical->memory.heap_count;
   properties->memoryTypeCount = physical->memory.type_count;
   properties->memoryHeaps[0] = (VkMemoryHeap){ANV_WDDM_HEAP_BYTES, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
   for (uint32_t i = 0; i < physical->memory.type_count; ++i)
      properties->memoryTypes[i] = (VkMemoryType){physical->memory.types[i].propertyFlags, physical->memory.types[i].heapIndex};
}
/* With graphics enabled, format and image-format queries are upstream HasVK
 * (anv_formats.c); this short list serves the compute-only build. */
#ifndef HASVK_WDDM_GRAPHICS
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceFormatProperties(VkPhysicalDevice handle, VkFormat format, VkFormatProperties *properties)
{
   (void)handle; memset(properties, 0, sizeof(*properties));
   if (format == VK_FORMAT_R8G8B8A8_UNORM
#ifdef HASVK_WDDM_GRAPHICS
       || format == VK_FORMAT_B8G8R8A8_UNORM
#endif
      )
      properties->linearTilingFeatures = properties->optimalTilingFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
#ifdef HASVK_WDDM_GRAPHICS
         | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
#endif
         ;
#ifdef HASVK_WDDM_GRAPHICS
   if (format == VK_FORMAT_D16_UNORM)
      properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
#endif
}
VKAPI_ATTR VkResult VKAPI_CALL anv_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice handle, VkFormat f, VkImageType t,
   VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *properties)
{
   (void)handle; memset(properties, 0, sizeof(*properties));
   if ((f != VK_FORMAT_R8G8B8A8_UNORM
#ifdef HASVK_WDDM_GRAPHICS
        && f != VK_FORMAT_D16_UNORM
        && f != VK_FORMAT_B8G8R8A8_UNORM
#endif
       ) || t != VK_IMAGE_TYPE_2D || flags ||
       (tiling != VK_IMAGE_TILING_LINEAR && tiling != VK_IMAGE_TILING_OPTIMAL) || (usage & ~image_usage()))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   *properties = (VkImageFormatProperties){.maxExtent = {4096, 4096, 1}, .maxMipLevels = 1,
      .maxArrayLayers = 1, .sampleCounts = VK_SAMPLE_COUNT_1_BIT, .maxResourceSize = 128ull << 20};
#ifdef HASVK_WDDM_GRAPHICS
   if (f == VK_FORMAT_D16_UNORM && (tiling != VK_IMAGE_TILING_OPTIMAL ||
       (usage & ~(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (f != VK_FORMAT_D16_UNORM && (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
#endif
   return VK_SUCCESS;
}
#endif
/* VK_KHR_get_physical_device_properties2 is advertised, so the loader and
 * layers call the "2" queries directly; they must not be left unimplemented. */
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice handle, VkPhysicalDeviceMemoryProperties2 *properties)
{
   anv_GetPhysicalDeviceMemoryProperties(handle, &properties->memoryProperties);
}
#ifndef HASVK_WDDM_GRAPHICS
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice handle, VkFormat format, VkFormatProperties2 *properties)
{
   anv_GetPhysicalDeviceFormatProperties(handle, format, &properties->formatProperties);
   vk_foreach_struct(ext, properties->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
         VkFormatProperties3 *wide = (void *)ext;
         wide->linearTilingFeatures = properties->formatProperties.linearTilingFeatures;
         wide->optimalTilingFeatures = properties->formatProperties.optimalTilingFeatures;
         wide->bufferFeatures = properties->formatProperties.bufferFeatures;
      }
   }
}
VKAPI_ATTR VkResult VKAPI_CALL anv_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice handle,
   const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *properties)
{
   /* External memory, modifiers, stencil usage and view-format lists are not supported. */
   vk_foreach_struct_const(ext, info->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO &&
          ((const VkPhysicalDeviceExternalImageFormatInfo *)ext)->handleType == 0) continue;
      memset(&properties->imageFormatProperties, 0, sizeof(properties->imageFormatProperties));
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   VkResult result = anv_GetPhysicalDeviceImageFormatProperties(handle, info->format, info->type, info->tiling,
      info->usage, info->flags, &properties->imageFormatProperties);
   if (result != VK_SUCCESS) return result;
   vk_foreach_struct(ext, properties->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
         memset(&((VkExternalImageFormatProperties *)ext)->externalMemoryProperties, 0, sizeof(VkExternalMemoryProperties));
   }
   return VK_SUCCESS;
}
/* Sparse resources are not supported: no sparse formats. */
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice handle, VkFormat format, VkImageType type,
   VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *count, VkSparseImageFormatProperties *properties)
{
   (void)handle; (void)format; (void)type; (void)samples; (void)usage; (void)tiling; (void)properties; *count = 0;
}
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice handle,
   const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count, VkSparseImageFormatProperties2 *properties)
{
   (void)handle; (void)info; (void)properties; *count = 0;
}
#endif
/* VK_EXT_sample_locations, as upstream anv_device.c: a 1x1 grid for every
 * multisampled count the hardware supports. */
VKAPI_ATTR void VKAPI_CALL anv_GetPhysicalDeviceMultisamplePropertiesEXT(VkPhysicalDevice handle, VkSampleCountFlagBits samples,
   VkMultisamplePropertiesEXT *properties)
{
   ANV_FROM_HANDLE(anv_physical_device, physical, handle);
   const VkSampleCountFlags counts = ~VK_SAMPLE_COUNT_1_BIT & isl_device_get_sample_counts(&physical->isl_dev);
   const uint32_t grid = (samples & counts) ? 1 : 0;
   properties->maxSampleLocationGridSize = (VkExtent2D){grid, grid};
   vk_foreach_struct(ext, properties->pNext) vk_debug_ignored_stype(ext->sType);
}
static VkResult queue_lost(struct vk_queue *queue, const char *message)
{
   VkResult result = vk_queue_set_lost(queue, "%s", message);
   struct anv_wddm_device_state *w = state(queue->base.device);
   mtx_lock(&w->mutex); u_cnd_monotonic_broadcast(&w->condition); mtx_unlock(&w->mutex);
   return result;
}
static VkResult submit(struct vk_queue *queue, struct vk_queue_submit *work)
{
   struct anv_device *device = container_of(queue->base.device, struct anv_device, vk);
   struct anv_wddm_device_state *w = device->wddm;
   if (vk_queue_submit_has_bind(work)) return VK_ERROR_FEATURE_NOT_PRESENT;
   VkResult result = vk_sync_wait_many(&device->vk, work->wait_count, work->waits, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS) return result;
   /* The command buffers and a completion write of the next serial go to the
    * kernel as one submission (unless event waits or a repeated command
    * buffer split it), which is not waited for here: the completion thread
    * verifies it and signals the syncs, while this thread submits on. */
   struct anv_wddm_pending *pending = vk_zalloc(&device->vk.alloc, sizeof(*pending) +
      work->command_buffer_count * sizeof(*pending->commands) + work->signal_count * sizeof(*pending->signals),
      8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!pending) return VK_ERROR_OUT_OF_HOST_MEMORY;
   pending->commands = (struct anv_cmd_buffer **)(pending + 1);
   pending->signals = (struct vk_sync_signal *)(pending->commands + work->command_buffer_count);
   pending->command_count = work->command_buffer_count; pending->signal_count = work->signal_count;
   for (uint32_t i = 0; i < work->command_buffer_count; ++i)
      pending->commands[i] = container_of(work->command_buffers[i], struct anv_cmd_buffer, vk);
   memcpy(pending->signals, work->signals, work->signal_count * sizeof(*pending->signals));
   /* A window copy recorded for a redirected present (WSI's present
    * submission) makes this kernel submission the present. */
   uint64_t present_token = 0;
   for (uint32_t i = 0; i < work->command_buffer_count; ++i)
      if (pending->commands[i]->wddm->present_token) present_token = pending->commands[i]->wddm->present_token;
   mtx_lock(&w->submit_mutex);
   pending->serial = ++w->next_serial;
   const int failed = anv_wddm_execute_commands(device, work->command_buffers, work->command_buffer_count,
                                                pending->serial, present_token, true, &pending->seq);
   const uint64_t seq = pending->seq;   /* the completion thread frees pending */
   if (!failed) {
      mtx_lock(&w->mutex);
      list_addtail(&pending->link, &w->pending);
      u_cnd_monotonic_broadcast(&w->condition);
      mtx_unlock(&w->mutex);
   }
   mtx_unlock(&w->submit_mutex);
   if (failed) { vk_free(&device->vk.alloc, pending); return queue_lost(queue, "HasVK command buffer submission failed"); }
   /* HASVK_WDDM_SYNC_SUBMIT=1: wait for each submission as before (comparison, debugging). */
   static int sync_submit = -1;
   if (sync_submit < 0) sync_submit = getenv("HASVK_WDDM_SYNC_SUBMIT") != NULL;
   if (sync_submit) anv_wddm_wait_verified(device, seq);
   return VK_SUCCESS;
}
/* Completion thread: in submission order, waits for each non-blocking
 * submission, verifies its command buffers' markers and guards and the
 * completion write, then signals its syncs. A failure loses the device. */
static int completion_main(void *data)
{
   struct anv_device *device = data;
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex);
   for (;;) {
      while (list_is_empty(&w->pending) && !w->completion_exit) u_cnd_monotonic_wait(&w->condition, &w->mutex);
      if (list_is_empty(&w->pending)) break;
      struct anv_wddm_pending *p = list_first_entry(&w->pending, struct anv_wddm_pending, link);
      mtx_unlock(&w->mutex);
      const char *error = NULL;
      if (vk_device_is_lost_no_report(&device->vk)) error = "device lost earlier";
      else if (hasvk_wddm_wait_submission(w->transport, p->seq)) error = hasvk_wddm_error();
      for (uint32_t i = 0; !error && i < p->command_count; ++i)
         if (p->commands[i]->wddm->seq == p->seq && anv_wddm_check_completed(p->commands[i])) error = "command buffer completion check failed";
      if (!error) {
         const uint32_t *actual = hasvk_wddm_bo_map(w->completion);
         if ((int32_t)(actual[0] - p->serial) < 0) error = "GPU completion counter behind";
         for (unsigned i = 1; !error && i < 1024; ++i) if (actual[i] != 0x39393939) error = "GPU guard corrupted";
      }
      if (error && !vk_device_is_lost_no_report(&device->vk)) vk_device_set_lost(&device->vk, "HasVK/WDDM completion: %s", error);
      /* All signals at once (one lock): a waiter woken by a fence must also
       * see the timeline points of the same submission. Mesa's timeline
       * emulation hands the driver only binary syncs of our type. */
      bool foreign = false;
      for (uint32_t i = 0; !error && i < p->signal_count; ++i) foreign |= p->signals[i].sync->type != &wddm_sync_type;
      if (!error && foreign && vk_sync_signal_many(&device->vk, p->signal_count, p->signals) != VK_SUCCESS)
         vk_device_set_lost(&device->vk, "HasVK/WDDM: signalling completion failed");
      mtx_lock(&w->mutex);
      for (uint32_t i = 0; !error && !foreign && i < p->signal_count; ++i) ((struct anv_wddm_sync *)p->signals[i].sync)->signaled = true;
      list_del(&p->link);
      w->verified_seq = p->seq; ++w->completed;
      u_cnd_monotonic_broadcast(&w->condition);
      vk_free(&device->vk.alloc, p);
   }
   mtx_unlock(&w->mutex);
   return 0;
}
static void completion_finish(struct anv_device *device)
{
   struct anv_wddm_device_state *w = device->wddm;
   if (!w || !w->completion_thread_started) return;
   mtx_lock(&w->mutex); w->completion_exit = true; u_cnd_monotonic_broadcast(&w->condition); mtx_unlock(&w->mutex);
   thrd_join(w->completion_thread, NULL);
   w->completion_thread_started = false;
}
#ifdef HASVK_WDDM_GRAPHICS
/* Device initialization runs before the queue is returned to the application. */
VkResult anv_queue_submit_simple_batch(struct anv_queue *queue, struct anv_batch *batch)
{
   struct anv_wddm_device_state *w = queue->device->wddm;
   size_t bytes = (char *)batch->next - (char *)batch->start;
   if (batch->status != VK_SUCCESS) return batch->status;
   if (!bytes || bytes > 8180) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   size_t aligned = align(bytes, 8);
   mtx_lock(&w->submit_mutex);
   /* The batch fills the entry BO from offset 0: every ring slot must be idle. */
   if (hasvk_wddm_wait_submission(w->transport, UINT64_MAX)) {
      mtx_unlock(&w->submit_mutex);
      return vk_errorf(queue->device, VK_ERROR_DEVICE_LOST, "WDDM initialization batch: %s", hasvk_wddm_error());
   }
   memset(w->slot_seq, 0, sizeof(w->slot_seq));
   memcpy(hasvk_wddm_bo_map(w->entry), batch->start, bytes);
   if (aligned > bytes) memset((char *)hasvk_wddm_bo_map(w->entry) + bytes, 0, aligned - bytes);
   const int failed = anv_wddm_flush_state_pools(queue->device) || hasvk_wddm_submit_wait(w->transport, w->entry, 0, aligned);
   mtx_unlock(&w->submit_mutex);
   if (failed) return vk_errorf(queue->device, VK_ERROR_DEVICE_LOST, "WDDM initialization batch: %s", hasvk_wddm_error());
   return VK_SUCCESS;
}
#endif
static void state_finish(struct anv_wddm_device_state *w)
{
   if (!w) return;
   hasvk_wddm_bo_destroy(w->completion); hasvk_wddm_bo_destroy(w->entry);
   if (w->client_heap_initialized) {
      util_vma_heap_finish(&w->client_heap); util_vma_heap_finish(&w->state_heap);
   }
   if (w->reservation) hasvk_wddm_release(w->transport, w->reservation);
   if (w->transport && hasvk_wddm_device_destroy(w->transport)) fprintf(stderr, "HasVK/WDDM cleanup: %s\n", hasvk_wddm_error());
   u_cnd_monotonic_destroy(&w->condition); mtx_destroy(&w->submit_mutex); mtx_destroy(&w->mutex);
   free(w->bo_ids); free(w);
}
VKAPI_ATTR VkResult VKAPI_CALL anv_CreateDevice(VkPhysicalDevice physical_handle, const VkDeviceCreateInfo *info,
   const VkAllocationCallbacks *allocator, VkDevice *out)
{
   *out = VK_NULL_HANDLE;
   ANV_FROM_HANDLE(anv_physical_device, physical, physical_handle);
   if (info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].queueFamilyIndex != 0 ||
       info->pQueueCreateInfos[0].queueCount != 1 || info->pQueueCreateInfos[0].flags) return VK_ERROR_INITIALIZATION_FAILED;
   struct anv_device *device = vk_zalloc2(&physical->instance->vk.alloc, allocator, sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct vk_device_dispatch_table dispatch;
   vk_device_dispatch_table_from_entrypoints(&dispatch, &anv_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch, &gfx8_device_entrypoints, false);
#ifdef HASVK_WDDM_GRAPHICS
   vk_device_dispatch_table_from_entrypoints(&dispatch, &wsi_device_entrypoints, false);
#endif
   VkResult result = vk_device_init(&device->vk, &physical->vk, &dispatch, info, allocator);
   if (result != VK_SUCCESS) { vk_free2(&physical->instance->vk.alloc, allocator, device); return result; }
   anv_device_set_physical(device, physical);
   device->vk.command_buffer_ops = &anv_cmd_buffer_ops;
   /* As upstream anv_CreateDevice: shaders get bounds-checked UBO/SSBO access
    * (ELK_ROBUSTNESS_*) when robustBufferAccess or nullDescriptor is on. */
   device->robust_buffer_access = device->vk.enabled_features.robustBufferAccess ||
                                  device->vk.enabled_features.nullDescriptor;
   struct anv_wddm_device_state *w = calloc(1, sizeof(*w));
   if (!w) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_base; }
   device->wddm = w;
   if (mtx_init(&w->mutex, mtx_plain) != thrd_success) { free(w); device->wddm = NULL; result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_base; }
   if (mtx_init(&w->submit_mutex, mtx_plain) != thrd_success) { mtx_destroy(&w->mutex); free(w); device->wddm = NULL; result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_base; }
   if (u_cnd_monotonic_init(&w->condition) != thrd_success) { mtx_destroy(&w->submit_mutex); mtx_destroy(&w->mutex); free(w); device->wddm = NULL; result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_base; }
   if (hasvk_wddm_device_create(&w->transport) || hasvk_wddm_reserve(w->transport, 16ull << 30, 0, &w->reservation)) {
      result = VK_ERROR_INITIALIZATION_FAILED; goto fail_state;
   }
   w->origin = (w->reservation + 0xffffffffull) & ~0xffffffffull;
   util_vma_heap_init(&w->client_heap, w->origin + 0x1c0000000, 4ull << 30);
   util_vma_heap_init(&w->state_heap, w->origin + 0x200000, (7ull << 30) - 0x200000);
   w->client_heap_initialized = true;
   if (hasvk_wddm_bo_create(w->transport, 8192, w->origin + 0x2c0000000, 1, &w->entry) ||
       hasvk_wddm_bo_create(w->transport, 4096, w->origin + 0x2c0200000, 1, &w->completion)) { result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail_state; }
   memset(hasvk_wddm_bo_map(w->completion), 0x39, 4096);
   *(uint32_t *)hasvk_wddm_bo_map(w->completion) = 0;
   list_inithead(&w->pending);
   if (thrd_create(&w->completion_thread, completion_main, device) != thrd_success) { result = VK_ERROR_INITIALIZATION_FAILED; goto fail_state; }
   w->completion_thread_started = true;
   result = anv_wddm_pools_init(device);
   if (result != VK_SUCCESS) goto fail_state;
#ifdef HASVK_WDDM_GRAPHICS
   /* As upstream: every custom border color is reserved up front, near the
    * dynamic state base, because the border color pointer has 24 bits. */
   anv_state_reserved_pool_init(&device->custom_border_colors, &device->dynamic_state_pool, 4096,
                                sizeof(struct gfx8_border_color), 64);
   w->custom_border_colors_initialized = true;
#endif
   device->null_surface_state = anv_state_pool_alloc(&device->surface_state_pool, device->isl_dev.ss.size, device->isl_dev.ss.align);
   isl_null_fill_state(&device->isl_dev, device->null_surface_state.map, .size = isl_extent3d(1, 1, 1));
   result = anv_device_alloc_bo(device, "workaround", 4096, ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_CAPTURE, 0, &device->workaround_bo);
   if (result != VK_SUCCESS) goto fail_state;
   device->workaround_address = (struct anv_address){device->workaround_bo, 0};
   anv_scratch_pool_init(device, &device->scratch_pool);
   struct vk_pipeline_cache_create_info cache_info = {0};
   device->default_pipeline_cache = vk_pipeline_cache_create(&device->vk, &cache_info, NULL);
   if (!device->default_pipeline_cache) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_state; }
#ifdef HASVK_WDDM_GRAPHICS
   cache_info.force_enable = true;
   device->internal_cache = vk_pipeline_cache_create(&device->vk, &cache_info, NULL);
   if (!device->internal_cache) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_state; }
   anv_device_init_blorp(device);
   anv_wddm_init_border_colors(device);
   result = anv_device_alloc_bo(device, "events", ANV_WDDM_EVENT_SLOTS * 8, ANV_BO_ALLOC_MAPPED, 0, &w->event_bo);
   if (result != VK_SUCCESS) goto fail_state;
#endif
   device->queues = vk_zalloc(&device->vk.alloc, sizeof(*device->queues), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device->queues) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_state; }
   device->vk.submit_mode = VK_QUEUE_SUBMIT_MODE_THREADED;
   result = vk_queue_init(&device->queues[0].vk, &device->vk, &info->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS) goto fail_queue_alloc;
   device->queues[0].device = device; device->queues[0].family = &physical->queue.families[0];
   device->queues[0].vk.driver_submit = submit;
   device->queue_count = 1;
#ifdef HASVK_WDDM_GRAPHICS
   result = gfx8_init_device_state(device);
   if (result != VK_SUCCESS) { vk_queue_finish(&device->queues[0].vk); goto fail_queue_alloc; }
#endif
   *out = anv_device_to_handle(device);
   return VK_SUCCESS;
fail_queue_alloc:
   vk_free(&device->vk.alloc, device->queues);
fail_state:
   fprintf(stderr, "HasVK/WDDM device init: %s\n", hasvk_wddm_error());
   completion_finish(device);
   if (device->default_pipeline_cache) vk_pipeline_cache_destroy(device->default_pipeline_cache, NULL);
#ifdef HASVK_WDDM_GRAPHICS
   if (device->internal_cache) { anv_device_finish_blorp(device); vk_pipeline_cache_destroy(device->internal_cache, NULL); }
#endif
   anv_scratch_pool_finish(device, &device->scratch_pool);
   anv_device_release_bo(device, device->workaround_bo);
   anv_device_release_bo(device, w->event_bo);
#ifdef HASVK_WDDM_GRAPHICS
   if (device->wddm && device->wddm->custom_border_colors_initialized) anv_state_reserved_pool_finish(&device->custom_border_colors);
#endif
   anv_wddm_pools_finish(device);
   state_finish(w); device->wddm = NULL;
fail_base:
   vk_device_finish(&device->vk); vk_free(&device->vk.alloc, device);
   return result;
}
VKAPI_ATTR void VKAPI_CALL anv_DestroyDevice(VkDevice handle, const VkAllocationCallbacks *allocator)
{
   (void)allocator; ANV_FROM_HANDLE(anv_device, device, handle);
   if (!device) return;
   vk_queue_finish(&device->queues[0].vk); vk_free(&device->vk.alloc, device->queues);
   completion_finish(device);   /* drains every non-blocking submission */
   vk_pipeline_cache_destroy(device->default_pipeline_cache, NULL);
#ifdef HASVK_WDDM_GRAPHICS
   anv_device_finish_blorp(device); vk_pipeline_cache_destroy(device->internal_cache, NULL);
#endif
   anv_scratch_pool_finish(device, &device->scratch_pool);
   anv_device_release_bo(device, device->workaround_bo);
   anv_device_release_bo(device, device->wddm->event_bo);
#ifdef HASVK_WDDM_GRAPHICS
   if (device->wddm && device->wddm->custom_border_colors_initialized) anv_state_reserved_pool_finish(&device->custom_border_colors);
#endif
   anv_wddm_pools_finish(device);
   state_finish(device->wddm);
   vk_device_finish(&device->vk); vk_free(&device->vk.alloc, device);
}
#ifdef HASVK_WDDM_GRAPHICS
/* Events: one 8-byte slot each in a device BO, written by the host directly and
 * by the GPU with PIPE_CONTROL, polled by MI_SEMAPHORE_WAIT (genX_cmd_buffer.c). */
struct anv_address anv_wddm_event_address(struct anv_device *device, const struct anv_event *event)
{
   return (struct anv_address){device->wddm->event_bo, (uint32_t)event->state.offset};
}
VKAPI_ATTR VkResult VKAPI_CALL anv_CreateEvent(VkDevice handle, const VkEventCreateInfo *info,
   const VkAllocationCallbacks *allocator, VkEvent *out)
{
   (void)info; ANV_FROM_HANDLE(anv_device, device, handle);
   struct anv_wddm_device_state *w = device->wddm;
   struct anv_event *event = vk_object_zalloc(&device->vk, allocator, sizeof(*event), VK_OBJECT_TYPE_EVENT);
   if (!event) return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   uint32_t slot = UINT32_MAX;
   mtx_lock(&w->mutex);
   for (uint32_t i = 0; i < ANV_WDDM_EVENT_SLOTS; ++i)
      if (!BITSET_TEST(w->event_slots, i)) { BITSET_SET(w->event_slots, i); slot = i; break; }
   mtx_unlock(&w->mutex);
   if (slot == UINT32_MAX) { vk_object_free(&device->vk, allocator, event); return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY); }
   event->state = (struct anv_state){.offset = (int32_t)(slot * 8), .alloc_size = 8, .map = (char *)w->event_bo->map + slot * 8};
   *(volatile uint64_t *)event->state.map = VK_EVENT_RESET;
   *out = anv_event_to_handle(event);
   return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL anv_DestroyEvent(VkDevice handle, VkEvent event_handle, const VkAllocationCallbacks *allocator)
{
   ANV_FROM_HANDLE(anv_device, device, handle); ANV_FROM_HANDLE(anv_event, event, event_handle);
   if (!event) return;
   mtx_lock(&device->wddm->mutex); BITSET_CLEAR(device->wddm->event_slots, event->state.offset / 8); mtx_unlock(&device->wddm->mutex);
   vk_object_free(&device->vk, allocator, event);
}
VKAPI_ATTR VkResult VKAPI_CALL anv_GetEventStatus(VkDevice handle, VkEvent event_handle)
{
   ANV_FROM_HANDLE(anv_device, device, handle); ANV_FROM_HANDLE(anv_event, event, event_handle);
   if (vk_device_is_lost(&device->vk)) return VK_ERROR_DEVICE_LOST;
   MemoryBarrier();
   return (VkResult)*(volatile uint64_t *)event->state.map;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_SetEvent(VkDevice handle, VkEvent event_handle)
{
   (void)handle; ANV_FROM_HANDLE(anv_event, event, event_handle);
   *(volatile uint64_t *)event->state.map = VK_EVENT_SET; MemoryBarrier();
   return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_ResetEvent(VkDevice handle, VkEvent event_handle)
{
   (void)handle; ANV_FROM_HANDLE(anv_event, event, event_handle);
   *(volatile uint64_t *)event->state.map = VK_EVENT_RESET; MemoryBarrier();
   return VK_SUCCESS;
}
#endif
#ifndef HASVK_WDDM_GRAPHICS
VKAPI_ATTR VkResult VKAPI_CALL anv_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
   const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
   (void)device; (void)cache; (void)infos; (void)allocator;
   for (uint32_t i = 0; i < count; ++i) out[i] = VK_NULL_HANDLE;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL anv_GetDeviceProcAddr(VkDevice handle, const char *name)
{ ANV_FROM_HANDLE(anv_device, device, handle); return vk_device_get_proc_addr(&device->vk, name); }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL anv_GetInstanceProcAddr(VkInstance handle, const char *name)
{ ANV_FROM_HANDLE(anv_instance, instance, handle); return vk_instance_get_proc_addr(instance ? &instance->vk : NULL, &anv_instance_entrypoints, name); }
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{ return anv_GetInstanceProcAddr(instance, name); }
PUBLIC int hasvk_wddm_icd_query_stats(VkDevice handle, uint64_t *completed, uint64_t *stamp)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   if (!device || !completed || !stamp) return -1;
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex); *completed = w->completed; *stamp = *(uint32_t *)hasvk_wddm_bo_map(w->completion); mtx_unlock(&w->mutex);
   return 0;
}
/* Coverage diagnostics: whether a Vulkan command resolves to a real function. */
PUBLIC int hasvk_wddm_icd_has_entrypoint(VkDevice handle, const char *name)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   if (!device || !name) return -1;
   /* Dispatch tables directly: the proc-addr lookups also hide commands whose
    * version or extension is not enabled, even when they are implemented. */
   if (vk_device_dispatch_table_get(&device->vk.dispatch_table, name)) return 1;
   struct anv_physical_device *physical = device->physical;
   return vk_instance_dispatch_table_get(&physical->instance->vk.dispatch_table, name) != NULL ||
          vk_physical_device_dispatch_table_get(&physical->vk.dispatch_table, name) != NULL;
}
PUBLIC int hasvk_wddm_icd_query_memory(VkDevice handle, uint64_t *bytes, uint32_t *count)
{
   ANV_FROM_HANDLE(anv_device, device, handle);
   if (!device || !bytes || !count) return -1;
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex); *bytes = w->allocated_bytes; *count = w->allocation_count; mtx_unlock(&w->mutex);
   return 0;
}
PUBLIC int hasvk_wddm_icd_query_compute_pipeline(VkPipeline handle, uint32_t *values)
{
   ANV_FROM_HANDLE(anv_pipeline, base, handle);
   if (!base || !values || base->type != ANV_PIPELINE_COMPUTE) return -1;
   struct anv_compute_pipeline *pipeline = anv_pipeline_to_compute(base);
   const struct anv_shader_bin *shader = pipeline->cs;
   const struct elk_cs_prog_data *program = (const void *)shader->prog_data;
   const struct elk_isa_info *isa = &base->device->physical->compiler->isa;
   if (!shader->kernel_size || !elk_validate_instructions(isa, shader->kernel.map, 0, shader->kernel_size, NULL)) return -1;
   values[0] = shader->kernel_size;
   for (unsigned i = 0; i < 3; ++i) values[1 + i] = program->local_size[i];
   values[4] = program->prog_mask; values[5] = shader->bind_map.surface_count;
   values[6] = program->base.nr_params; values[7] = program->base.total_scratch;
   values[8] = program->base.total_shared; values[9] = shader->kernel.offset;
   values[10] = (char *)base->batch.next - (char *)base->batch.start;
   values[11] = program->push.per_thread.regs;
   return 0;
}
