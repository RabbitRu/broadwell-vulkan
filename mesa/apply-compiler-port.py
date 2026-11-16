"""Apply the bounded Windows Mesa library port to pinned sources.

This enables ELK/ISL/BLORP, Vulkan runtime, Win32 WSI and an experimental HasVK
hardware compute/storage-image ICD. Graphics and presentation remain disabled.
It does not substitute fake DRM responses for a working WDDM implementation.
"""
import difflib
import pathlib
import tarfile
import re
from compute_sources import generate_compute_sources
from graphics_sources import generate_graphics_sources

ROOT = pathlib.Path(__file__).resolve().parent.parent
MESA = ROOT / "third_party/mesa-26.2.7"
if (MESA / "VERSION").read_text().strip() != "26.2.7":
    raise SystemExit("Wrong Mesa version")
patches = []
originals = ROOT / '.tools/mesa-upstream-26.2.7'
names = {
    'meson.options', 'meson.build', 'src/intel/meson.build',
    'src/intel/compiler/meson.build', 'src/intel/dev/meson.build',
    'src/intel/dev/intel_debug.c', 'src/intel/dev/intel_device_info.c',
    'src/util/macros.h', 'src/util/u_atomic.h', 'include/drm-uapi/drm_fourcc.h',
    'src/intel/compiler/elk/elk_eu.c',
    'src/intel/compiler/elk/elk_eu_validate.c',
    'src/intel/compiler/elk/elk_fs_bank_conflicts.cpp',
    'src/intel/compiler/elk/elk_fs.cpp',
    'src/intel/mda/debug_archiver.c',
    'src/intel/compiler/elk/elk_eu_defines.h',
    'src/intel/compiler/elk/elk_reg_type.h',
    'src/intel/compiler/elk/elk_compiler.h',
    'src/intel/common/intel_measure.h', 'src/intel/blorp/blorp_clear.c',
    'src/intel/blorp/blorp_blit.c',
    'src/intel/isl/isl_priv.h', 'src/intel/isl/isl_drm.c',
    'src/vulkan/wsi/wsi_common_win32.cpp', 'src/vulkan/wsi/meson.build',
    'src/vulkan/wsi/wsi_common.c',
    'src/intel/vulkan_hasvk/anv_private.h',
    'src/intel/vulkan_hasvk/anv_allocator.c',
    'src/intel/vulkan_hasvk/anv_descriptor_set.c',
    'src/intel/vulkan_hasvk/anv_pipeline.c', 'src/intel/vulkan_hasvk/anv_pipeline_cache.c',
    'src/intel/vulkan_hasvk/genX_pipeline.c',
    'src/intel/vulkan_hasvk/anv_formats.c',
    'src/intel/vulkan_hasvk/anv_cmd_buffer.c', 'src/intel/vulkan_hasvk/genX_cmd_buffer.c',
    'src/intel/vulkan_hasvk/anv_batch_chain.c', 'src/intel/vulkan_hasvk/genX_state.c',
    'src/intel/vulkan_hasvk/anv_image.c',
    'src/intel/vulkan_hasvk/anv_blorp.c', 'src/intel/vulkan_hasvk/genX_blorp_exec.c',
    'src/intel/vulkan_hasvk/gfx8_cmd_buffer.c',
    'src/intel/vulkan_hasvk/anv_device.c',
}
missing = {name for name in names if not (originals / name).exists()}
if missing:
    # One streaming decompression pass instead of seeking through xz for every
    # edited file. Original contents remain separate from the working tree.
    with tarfile.open(ROOT / '.tools/downloads/mesa-26.2.7.tar.xz', 'r|xz') as archive:
        for member in archive:
            name = member.name.removeprefix('mesa-26.2.7/')
            if name in missing:
                target = originals / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
                missing.remove(name)
                if not missing:
                    break
    if missing:
        raise ValueError(f'Missing upstream files: {missing}')


def edit(name, replacements):
    path = MESA / name
    # Always derive the complete patch from the pinned upstream archive. This
    # also makes repeated bootstrap/build calls idempotent.
    before = (originals / name).read_text(encoding='utf-8')
    after = before
    for old, new in replacements:
        if new in after:
            continue
        if after.count(old) != 1:
            raise ValueError(f"Unexpected source context: {name}: {old[:80]!r}")
        after = after.replace(old, new)
    if after != before:
        if path.read_text(encoding="utf-8") != after:
            path.write_text(after, encoding="utf-8", newline="\n")
        patches.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), f"a/{name}", f"b/{name}"))


edit("meson.options", [("option(\n  'split-debug',", "option('hasvk-wddm-compiler-only', type : 'boolean', value : false,\n       description : 'Windows WDDM libraries and experimental hardware ICD; incomplete Vulkan')\noption('hasvk-wddm-graphics', type : 'boolean', value : false,\n       description : 'Experimental Windows HasVK graphics modules')\n\noption(\n  'split-debug',")])
edit("meson.build", [
    ("with_gallium_iris, with_gallium_crocus, with_intel_vk, with_intel_hasvk,\n  with_gallium_asahi,", "with_gallium_iris, with_gallium_crocus, with_intel_vk,\n  with_intel_hasvk and not get_option('hasvk-wddm-compiler-only'),\n  with_gallium_asahi,"),
    ("elif with_intel_vk or with_intel_hasvk\n  error('Intel", "elif (with_intel_vk or with_intel_hasvk) and not get_option('hasvk-wddm-compiler-only')\n  error('Intel"),
])
edit("src/intel/meson.build", [
    ("subdir('isl')\nsubdir('common')\nsubdir('compiler')", "if get_option('hasvk-wddm-compiler-only')\n  if host_machine.system() != 'windows'\n    error('The isolated WDDM compiler build is Windows-only')\n  endif\n  subdir('isl')\n  subdir('compiler')\n  subdir('wddm_compiler')\nelse\nsubdir('isl')\nsubdir('common')\nsubdir('compiler')"),
    ("if with_intel_hasvk\n  subdir('vulkan_hasvk')\nendif", "if with_intel_hasvk\n  subdir('vulkan_hasvk')\nendif\nendif # not hasvk-wddm-compiler-only"),
])
edit("src/intel/compiler/meson.build", [
    ("subdir('gen')\nsubdir('brw')\nsubdir('jay')", "if not get_option('hasvk-wddm-compiler-only')\n  subdir('gen')\n  subdir('brw')\n  subdir('jay')\nendif"),
])
edit("src/intel/dev/meson.build", [
    ("inc_intel_dev_virtio = []", "if get_option('hasvk-wddm-compiler-only')\n  files_libintel_dev = files('intel_debug.c', 'intel_device_info.c')\nendif\n\ninc_intel_dev_virtio = []"),
])
edit("src/intel/dev/intel_debug.c", [
    ('#include "util/u_math.h"', '#include "util/u_math.h"\n#include "util/u_string.h"'),
])
edit('src/intel/common/intel_measure.h', [
    ('#include <pthread.h>', '#ifdef _WIN32\n#include "util/simple_mtx.h"\n#else\n#include <pthread.h>\n#endif'),
    ('   pthread_mutex_t mutex;', '#ifdef _WIN32\n   simple_mtx_t mutex;\n#else\n   pthread_mutex_t mutex;\n#endif'),
])
edit('src/intel/isl/isl_priv.h', [
    ('#include <strings.h>', '#ifndef _WIN32\n#include <strings.h>\n#endif'),
])
edit('src/intel/isl/isl_drm.c', [
    ('#include "drm-uapi/i915_drm.h"', '#ifndef _WIN32\n#include "drm-uapi/i915_drm.h"\n#else\n/* Tiling identifiers for pure layout conversion, no DRM calls. */\n#define I915_TILING_NONE 0\n#define I915_TILING_X 1\n#define I915_TILING_Y 2\n#endif'),
])
edit('src/vulkan/wsi/wsi_common_win32.cpp', [
    ('#include <directx/d3d12.h>\n#include <dxguids/dxguids.h>',
     '#ifdef HASVK_WDDM_GDI\n/* The isolated WDDM build uses the pinned Windows SDK interfaces. */\n#include <d3d12.h>\n#else\n#include <directx/d3d12.h>\n#include <dxguids/dxguids.h>\n#endif'),
    ('   if (!wsi_device->sw) {\n      wsi->dxgi.factory = dxgi_get_factory',
     '   if (!wsi_device->sw && wsi_device->win32.get_d3d12_command_queue) {\n      wsi->dxgi.factory = dxgi_get_factory'),
    ('   { VK_FORMAT_R8G8B8A8_UNORM },\n   { VK_FORMAT_B8G8R8A8_SRGB },',
     '#ifndef HASVK_WDDM_GDI\n   { VK_FORMAT_R8G8B8A8_UNORM },\n   { VK_FORMAT_B8G8R8A8_SRGB },\n#endif'),
])
edit('src/vulkan/wsi/wsi_common.c', [
    ('      if (wsi->sw) {\n         wsi->WaitForFences',
     '''#ifdef HASVK_WDDM_GDI
      /* CPU presentation of a hardware image must wait for its GPU copy. */
      if (true) {
#else
      if (wsi->sw) {
#endif
         wsi->WaitForFences'''),
])
edit('src/vulkan/wsi/meson.build', [
    ('libvulkan_wsi = static_library(',
     "wddm_gdi_args = []\nif get_option('hasvk-wddm-compiler-only')\n  wddm_gdi_args = ['-DHASVK_WDDM_GDI']\nendif\n\nlibvulkan_wsi = static_library("),
    ('  link_with: links_vulkan_wsi,', '  link_with: links_vulkan_wsi,\n  cpp_args : wddm_gdi_args,\n  c_args : wddm_gdi_args,'),
])
edit('src/intel/vulkan_hasvk/anv_private.h', [
    ('#include <pthread.h>', '#ifdef _WIN32\n#include "anv_wddm_compat.h"\n#else\n#include <pthread.h>\n#endif'),
    ('#include "drm-uapi/i915_drm.h"', '#ifndef _WIN32\n#include "drm-uapi/i915_drm.h"\n#endif'),
    ('#include "common/intel_gem.h"', '#ifndef _WIN32\n#include "common/intel_gem.h"\n#endif'),
    ('   return image->vk.drm_format_mod != DRM_FORMAT_MOD_INVALID ||\n          image->vk.external_handle_types != 0;', '#ifndef _WIN32\n   return image->vk.drm_format_mod != DRM_FORMAT_MOD_INVALID ||\n          image->vk.external_handle_types != 0;\n#else\n   return image->vk.external_handle_types != 0;\n#endif'),
    ('struct anv_device {\n    struct vk_device                            vk;', 'struct anv_device {\n    struct vk_device                            vk;\n#ifdef _WIN32\n    struct anv_wddm_device_state *wddm;\n#endif'),
    ('struct anv_bo {\n', 'struct anv_bo {\n#ifdef _WIN32\n   struct hasvk_wddm_bo *wddm_bo;\n#endif\n'),
    ('struct anv_cmd_buffer {\n   struct vk_command_buffer                     vk;', 'struct anv_cmd_buffer {\n   struct vk_command_buffer                     vk;\n#ifdef _WIN32\n   struct anv_wddm_cmd_state *wddm;\n#endif'),
    ('   enum isl_format isl_format:16;', '#if defined(_WIN32) && defined(__clang__)\n   unsigned isl_format:16;\n#else\n   enum isl_format isl_format:16;\n#endif'),
    ('struct anv_device_memory {\n   struct vk_object_base                        base;', 'struct anv_device_memory {\n   struct vk_object_base                        base;\n#ifdef _WIN32\n   uint64_t wddm_allocation_size;\n#endif'),
    ('#define anv_genX(devinfo, thing) ({', '#ifdef _WIN32\n/* The WDDM adapter gate accepts only Gen8. */\n#define anv_genX(devinfo, thing) (&gfx8_##thing)\n#else\n#define anv_genX(devinfo, thing) ({'),
    ('})\n\n/* Gen-specific function declarations */', '})\n#endif\n\n/* Gen-specific function declarations */'),
])
# Keep Mesa's lock-free state/block allocators and free lists. Only the Linux
# mmap backing and GEM BO implementation are replaced on Windows.
allocator_name = 'src/intel/vulkan_hasvk/anv_allocator.c'
allocator_original = (originals / allocator_name).read_text(encoding='utf-8')
allocator_replacements = [
    ('p_atomic_cmpxchg(&pool->bos[scratch_size_log2][stage], NULL, bo)',
     'p_atomic_cmpxchg_ptr(&pool->bos[scratch_size_log2][stage], NULL, bo)'),
    # Block-pool maps take a BO-relative offset; state offsets include the
    # general/negative binding-table pool bias. Normalize at the boundary.
    ('                                        state_i->offset,',
     '#ifdef _WIN32\n                                        state_i->offset - pool->start_offset,\n#else\n                                        state_i->offset,\n#endif'),
    ('         int32_t chunk_offset = state->offset;',
     '         int32_t chunk_offset = state->offset;\n#ifdef _WIN32\n         chunk_offset -= pool->start_offset;\n#endif'),
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif'),
    ('#include <sys/mman.h>', '#ifndef _WIN32\n#include <sys/mman.h>\n#endif'),
    ('#include "util/anon_file.h"', '#ifndef _WIN32\n#include "util/anon_file.h"\n#endif'),
    ('#define ANV_STATE_ENTRY_SIZE (sizeof(struct anv_free_entry))',
     '#define ANV_STATE_ENTRY_SIZE (sizeof(struct anv_free_entry))\n#ifdef _WIN32\n#include "anv_wddm_allocator.h"\n#endif'),
]
for function in ['anv_state_table_init', 'anv_state_table_expand_range', 'anv_state_table_finish',
                 'anv_block_pool_init', 'anv_block_pool_expand_range', 'anv_block_pool_finish']:
    match = re.search(r'\n(?:static )?(?:VkResult|void)\n' + function + r'\([^;{]*\)\n\{', allocator_original)
    if not match:
        raise ValueError(f'Missing upstream allocator function: {function}')
    end = allocator_original.index('\n}\n', match.end()) + 3
    body = allocator_original[match.start() + 1:end]
    allocator_replacements.append((body, '#ifndef _WIN32\n' + body + '\n#endif'))
gem_allocator = allocator_original[allocator_original.index('#define ANV_BO_CACHE_SUPPORTED_FLAGS'):]
allocator_replacements.append((gem_allocator, '#ifndef _WIN32\n' + gem_allocator + '\n#endif\n'))
edit(allocator_name, allocator_replacements)
edit('src/intel/vulkan_hasvk/anv_descriptor_set.c', [
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif'),
])
pipeline_name = 'src/intel/vulkan_hasvk/anv_pipeline.c'
pipeline_original = (originals / pipeline_name).read_text(encoding='utf-8')
pipeline_replacements = [
    ('   anv_genX(device->info, compute_pipeline_emit)(pipeline);', '''#ifdef _WIN32
   const struct elk_cs_prog_data *program = get_cs_prog_data(pipeline);
   if (program->local_size[0] > 64 || program->local_size[1] != 1 || program->local_size[2] != 1 ||
       program->base.total_scratch || program->base.total_shared) {
      anv_pipeline_finish(&pipeline->base, device, pAllocator);
      vk_free2(&device->vk.alloc, pAllocator, pipeline);
      *pPipeline = VK_NULL_HANDLE;
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
   }
#endif
   anv_genX(device->info, compute_pipeline_emit)(pipeline);'''),
    ('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif\n#include "util/memstream.h"'),
    ('      FILE *stream = open_memstream(&stream_data, &stream_size);',
     '      struct u_memstream memstream;\n      if (!u_memstream_open(&memstream, &stream_data, &stream_size)) return;\n      FILE *stream = u_memstream_get(&memstream);'),
    ('      fclose(stream);', '      u_memstream_close(&memstream);'),
]
for function in ['anv_graphics_pipeline_init', 'anv_graphics_pipeline_create', 'anv_CreateGraphicsPipelines']:
    match = re.search(r'\n(?:static\s+)?VkResult\s+' + function + r'\([^;{]*\)\n\{', pipeline_original)
    if not match:
        raise ValueError(f'Missing upstream graphics function: {function}')
    end = pipeline_original.index('\n}\n', match.end()) + 3
    body = pipeline_original[match.start() + 1:end]
    pipeline_replacements.append((body, '#if !defined(_WIN32) || defined(HASVK_WDDM_GRAPHICS)\n' + body + '\n#endif'))
edit(pipeline_name, pipeline_replacements)
edit('src/intel/vulkan_hasvk/anv_pipeline_cache.c', [
    ('   uint64_t shader_data_addr = INSTRUCTION_STATE_POOL_MIN_ADDRESS +',
     '#ifdef _WIN32\n   uint64_t shader_data_addr = device->instruction_state_pool.block_pool.start_address -\n                               device->instruction_state_pool.start_offset +\n#else\n   uint64_t shader_data_addr = INSTRUCTION_STATE_POOL_MIN_ADDRESS +\n#endif'),
])
image_name = 'src/intel/vulkan_hasvk/anv_image.c'
image_before = (originals / image_name).read_text(encoding='utf-8')
image_after = image_before.replace('#include <unistd.h>', '#ifndef _WIN32\n#include <unistd.h>\n#endif')
image_after = image_after.replace('#include <sys/mman.h>', '#ifndef _WIN32\n#include <sys/mman.h>\n#endif')
image_after = image_after.replace('image->vk.drm_format_mod', 'anv_image_modifier(image)')
image_after = image_after.replace('      anv_image_modifier(image) = isl_mod_info->modifier;',
    '#ifndef _WIN32\n      image->vk.drm_format_mod = isl_mod_info->modifier;\n#endif')
image_after = image_after.replace('#define ANV_OFFSET_IMPLICIT UINT64_MAX', '''#define ANV_OFFSET_IMPLICIT UINT64_MAX
static inline uint64_t anv_image_modifier(const struct anv_image *image)
{
#ifdef _WIN32
   /* No external/modifier images are accepted by this Windows path. */
   return DRM_FORMAT_MOD_INVALID;
#else
   return image->vk.drm_format_mod;
#endif
}''')
image_after = image_after.replace('   vk_image_init(&device->vk, &image->vk, pCreateInfo);', '''#ifdef _WIN32
   if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
       (pCreateInfo->format != VK_FORMAT_R8G8B8A8_UNORM
#ifdef HASVK_WDDM_GRAPHICS
        && pCreateInfo->format != VK_FORMAT_D16_UNORM
        && pCreateInfo->format != VK_FORMAT_B8G8R8A8_UNORM
#endif
       ) ||
       (pCreateInfo->flags & ~VK_IMAGE_CREATE_ALIAS_BIT) || pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1 ||
       pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT || pCreateInfo->extent.depth != 1 ||
       !pCreateInfo->extent.width || !pCreateInfo->extent.height ||
       pCreateInfo->extent.width > 4096 || pCreateInfo->extent.height > 4096 ||
       (pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR && pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL) ||
       (pCreateInfo->usage & ~(VK_IMAGE_USAGE_STORAGE_BIT
#ifdef HASVK_WDDM_GRAPHICS
         | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
         | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
#endif
       ))) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   create_info->isl_extra_usage_flags |= ISL_SURF_USAGE_DISABLE_AUX_BIT;
#ifdef HASVK_WDDM_GRAPHICS
   if (pCreateInfo->format == VK_FORMAT_D16_UNORM &&
       (pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL ||
        (pCreateInfo->usage & ~(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (pCreateInfo->format != VK_FORMAT_D16_UNORM && (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
#endif
#endif
   vk_image_init(&device->vk, &image->vk, pCreateInfo);''')
edit(image_name, [(image_before, image_after)])
for name in ['src/intel/blorp/blorp_clear.c', 'src/intel/blorp/blorp_blit.c']:
    edit(name, [('#include "blorp_shaders.h"', '#ifdef HASVK_WDDM_LEGACY_BLORP\n#include "blorp_bounds_nir.h"\n#else\n#include "blorp_shaders.h"\n#endif')])
edit("src/intel/mda/debug_archiver.c", [
    ('#include "util/u_process.h"', '#include "util/u_process.h"\n#include "util/os_file.h"'),
    ('#include <unistd.h>', '#ifdef _WIN32\n#include <process.h>\n#define getpid _getpid\n#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)\n#else\n#include <unistd.h>\n#endif'),
    ('mkdir(dir, 0755)', 'os_mkdir(dir, 0755)'),
])
edit("src/intel/dev/intel_device_info.c", [
    ('#include <unistd.h>\n\n#include "util/libdrm.h"', '#ifndef _WIN32\n#include <unistd.h>\n#include "util/libdrm.h"\n#endif'),
    ('#include "i915/intel_device_info.h"\n#include "xe/intel_device_info.h"\n#include "virtio/intel_virtio.h"\n\n#include "common/intel_gem.h"', '#ifndef _WIN32\n#include "i915/intel_device_info.h"\n#include "xe/intel_device_info.h"\n#include "virtio/intel_virtio.h"\n#include "common/intel_gem.h"\n#endif'),
    ('#include "util/u_debug.h"', '#include "util/u_debug.h"\n#include "util/strndup.h"'),
    ('   intel_device_info_i915_update_from_masks(devinfo,\n', '''#ifdef _WIN32
   /* Static PCI-table topology for the offline compiler only. WDDM runtime
    * discovery must supply the real enabled slice/subslice/EU masks. */
   unsigned slices = devinfo->num_slices;
   unsigned subslices = devinfo->num_subslices[0];
   unsigned eus = devinfo->max_eus_per_subslice;
   intel_device_info_topology_reset_masks(devinfo);
   devinfo->max_slices = slices;
   devinfo->max_subslices_per_slice = subslices;
   devinfo->subslice_slice_stride = DIV_ROUND_UP(subslices, 8);
   devinfo->eu_subslice_stride = DIV_ROUND_UP(eus, 8);
   devinfo->eu_slice_stride = subslices * devinfo->eu_subslice_stride;
   devinfo->slice_masks = (1u << slices) - 1;
   for (unsigned s = 0; s < slices; ++s) {
      for (unsigned ss = 0; ss < subslices; ++ss) {
         devinfo->subslice_masks[s * devinfo->subslice_slice_stride + ss / 8] |= 1u << (ss % 8);
         for (unsigned eu = 0; eu < eus; ++eu)
            devinfo->eu_masks[s * devinfo->eu_slice_stride + ss * devinfo->eu_subslice_stride + eu / 8] |= 1u << (eu % 8);
      }
   }
   intel_device_info_topology_update_counts(devinfo);
   intel_device_info_update_pixel_pipes(devinfo, devinfo->subslice_masks);
   intel_device_info_update_l3_banks(devinfo);
#else
   intel_device_info_i915_update_from_masks(devinfo,
'''),
    ('                          devinfo->max_eus_per_subslice);\n}', '                          devinfo->max_eus_per_subslice);\n#endif\n}'),
    ("static void\nintel_device_info_adjust_memory", "#ifndef _WIN32\nstatic void\nintel_device_info_adjust_memory"),
    ("enum intel_wa_steppings\nintel_device_info_wa_stepping", "#endif /* Linux DRM device queries; the offline compiler uses PCI tables */\n\nenum intel_wa_steppings\nintel_device_info_wa_stepping"),
])
edit("src/util/u_atomic.h", [
    ('_interlockedexchange64', '_InterlockedExchange64'),
    ('_interlockedexchangeadd64', '_InterlockedExchangeAdd64'),
    ('_interlockedincrement64', '_InterlockedIncrement64'),
    ('_interlockeddecrement64', '_InterlockedDecrement64'),
    ('_interlockedadd  ((long', 'p_atomic_msvc_add32((long'),
    ('_interlockedadd64((__int64', 'p_atomic_msvc_add64((__int64'),
    ('/* MSVC supports decltype keyword,', '''static __forceinline long
p_atomic_msvc_add32(long volatile *address, long value)
{
   return (long)((unsigned long)_InterlockedExchangeAdd(address, value) + (unsigned long)value);
}

static __forceinline __int64
p_atomic_msvc_add64(__int64 volatile *address, __int64 value)
{
   return (__int64)((unsigned __int64)_InterlockedExchangeAdd64(address, value) + (unsigned __int64)value);
}

/* MSVC supports decltype keyword,'''),
])
edit("src/intel/compiler/elk/elk_eu.c", [
    ('#include <fcntl.h>', '#include <fcntl.h>\n#ifdef _WIN32\n#include <stdint.h>\n#include <io.h>\ntypedef intptr_t ssize_t;\n#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)\n#else\n#define O_BINARY 0\n#endif'),
    ('O_CREAT | O_WRONLY | O_TRUNC,', 'O_CREAT | O_WRONLY | O_TRUNC | O_BINARY,'),
    ('open(name, O_RDONLY)', 'open(name, O_RDONLY | O_BINARY)'),
])
edit("src/intel/compiler/elk/elk_eu_validate.c", [
    ('/* We\'re going to do lots of string concatenation, so this should help. */', '''#ifdef _WIN32
static const void *
elk_memmem(const void *haystack, size_t size, const void *needle, size_t length)
{
   if (!length) return haystack;
   if (length > size) return NULL;
   for (size_t i = 0; i <= size - length; ++i)
      if (!memcmp((const char *)haystack + i, needle, length))
         return (const char *)haystack + i;
   return NULL;
}
#define memmem elk_memmem
#endif

/* We're going to do lots of string concatenation, so this should help. */'''),
])
edit("src/intel/compiler/elk/elk_fs_bank_conflicts.cpp", [
    ('#include "util/macros.h"', '#include "util/macros.h"\n#include "util/u_memory.h"'),
    ('         free(v);', '         align_free(v);'),
    ('         void *p;\n         if (posix_memalign(&p, align, size))', '         void *p = align_malloc(size, align);\n         if (!p)'),
])
edit("src/intel/compiler/elk/elk_fs.cpp", [
    ('#include "util/u_math.h"', '#include "util/u_math.h"\n#include "util/u_string.h"'),
])
for name, enum_names in {
    'src/intel/compiler/elk/elk_eu_defines.h': ['elk_reg_file', 'elk_predicate'],
    'src/intel/compiler/elk/elk_reg_type.h': ['elk_reg_type'],
    'src/intel/compiler/elk/elk_compiler.h': ['elk_robustness_flags', 'elk_sf_primitive', 'elk_clip_fill_mode', 'elk_clip_mode', 'elk_sometimes'],
}.items():
    upstream = (originals / name).read_text(encoding='utf-8')
    first_include = re.search(r'^#include [^\n]+', upstream, re.M).group()
    replacements = [(first_include, '#include "elk_win32_compat.h"\n' + first_include)]
    for enum_name in enum_names:
        pattern = r'\benum(?:\s+(?:ENUM_PACKED|PACKED))?\s+' + enum_name + r'\s*\{'
        declaration = re.search(pattern, upstream)
        if declaration is None:
            raise ValueError(f'Missing enum declaration: {enum_name}')
        old = declaration.group()
        replacements.append((old, old[:-1].rstrip() + ' ELK_ENUM_BASE {'))
    if name.endswith('elk_reg_type.h'):
        replacements.append(('enum elk_reg_file;', 'enum elk_reg_file ELK_ENUM_BASE;'))
    if name.endswith('elk_compiler.h'):
        replacements.append(('alpha_test_func:3;', 'alpha_test_func:ELK_COMPARE_FUNC_BITS;'))
    edit(name, replacements)
edit("src/util/macros.h", [
    ("#define PRAGMA_POISON\n", "#define PRAGMA_POISON(X)\n"),
])
edit("include/drm-uapi/drm_fourcc.h", [
    ('#include "drm.h"', '#ifdef _WIN32\n/* Format/modifier constants only; no DRM transport on Windows. */\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef uint64_t __u64;\n#else\n#include "drm.h"\n#endif'),
])
owned_sources = {
    'src/intel/vulkan_hasvk/anv_wddm_compat.h': (ROOT / 'mesa/anv_wddm_compat.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_device.c': (ROOT / 'mesa/anv_wddm_device.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm.h': (ROOT / 'mesa/anv_wddm.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_memory.c': (ROOT / 'mesa/anv_wddm_memory.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_allocator.h': (ROOT / 'mesa/anv_wddm_allocator.h').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_pools.c': (ROOT / 'mesa/anv_wddm_pools.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_batch.c': (ROOT / 'mesa/anv_wddm_batch.c').read_text(encoding='utf-8'),
    'src/intel/vulkan_hasvk/anv_wddm_command.c': (ROOT / 'mesa/anv_wddm_command.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/vulkan_hasvk_wddm.def': (ROOT / 'mesa/anv_wddm.def').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/elk_compile_probe.c': (ROOT / 'mesa/elk_compile_probe.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/elk_abi_probe.cpp': (ROOT / 'mesa/elk_abi_probe.cpp').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/spirv_probe.c': (ROOT / 'mesa/spirv_probe.c').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/spirv_probe.h': (ROOT / 'mesa/spirv_probe.h').read_text(encoding='utf-8'),
    'src/intel/wddm_compiler/image_probe.c': (ROOT / 'mesa/image_probe.c').read_text(encoding='utf-8'),
    'src/intel/compiler/elk/elk_win32_compat.h': (ROOT / 'mesa/elk_win32_compat.h').read_text(encoding='utf-8'),
    'src/intel/blorp/blorp_bounds_nir.h': '''/* Equivalent NIR builder for blorp_shaders.cl::blorp_check_in_bounds.
 * Only for the bounded Gen8 Windows bring-up. Indirect-copy shaders are not
 * compiled in this target and no corresponding Vulkan extension is exposed. */
#pragma once
#include "compiler/nir/nir_builder.h"
static inline nir_def *
blorp_check_in_bounds(nir_builder *b, nir_def *rect, nir_def *pos)
{
   nir_def *x = nir_channel(b, pos, 0), *y = nir_channel(b, pos, 1);
   return nir_iand(b,
      nir_iand(b, nir_uge(b, x, nir_channel(b, rect, 0)), nir_ult(b, x, nir_channel(b, rect, 1))),
      nir_iand(b, nir_uge(b, y, nir_channel(b, rect, 2)), nir_ult(b, y, nir_channel(b, rect, 3))));
}
''',
    'src/intel/wddm_compiler/meson.build': '''# Windows ELK/ISL/BLORP and initial, incomplete HasVK ICD.
executable('IrisElkCompile', ['elk_compile_probe.c', 'elk_abi_probe.cpp', 'spirv_probe.c'],
  include_directories : [inc_include, inc_src, inc_intel],
  dependencies : [idep_intel_compiler_elk, idep_intel_dev, idep_nir, idep_mesautil, idep_mda],
  c_args : [c_msvc_compat_args],
  win_subsystem : 'console',
)
libwddm_blorp = static_library('wddm_blorp',
  ['../blorp/blorp.c', '../blorp/blorp_blit.c', '../blorp/blorp_clear.c', '../blorp/blorp_elk.c',
   '../common/intel_compute_slm.c', '../common/intel_l3_config.c',
   '../common/intel_sample_positions.c', '../common/intel_urb_config.c'],
  include_directories : [inc_include, inc_src, inc_intel],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_intel_dev],
  c_args : [c_msvc_compat_args, '-DHASVK_WDDM_LEGACY_BLORP'],
  build_by_default : false,
)
''',
}
# Gen8's actual compute emitter and its two helpers, verbatim from the pinned
# source. Graphics emission is added when graphics command buffers are enabled.
gen_pipeline = (originals / 'src/intel/vulkan_hasvk/genX_pipeline.c').read_text(encoding='utf-8')
compute_emitter = gen_pipeline[:gen_pipeline.index('\nstatic ')]
for signature in ['static uint32_t\nget_sampler_count(', 'static UNUSED struct anv_address\nget_scratch_address(',
                  'void\ngenX(compute_pipeline_emit)(']:
    start = gen_pipeline.index(signature)
    end = gen_pipeline.index('\n}\n', start) + 3
    compute_emitter += '\n' + gen_pipeline[start:end] + '\n'
owned_sources['src/intel/wddm_compiler/anv_wddm_compute_emit.c'] = compute_emitter
formats = (originals / 'src/intel/vulkan_hasvk/anv_formats.c').read_text(encoding='utf-8')
owned_sources['src/intel/wddm_compiler/anv_wddm_format_table.c'] = formats[:formats.index('// Format capabilities')]
owned_sources.update(generate_compute_sources(originals))
owned_sources.update(generate_graphics_sources(originals))
for name in ['hasvk_wddm.h', 'hasvk_wddm.hpp']:
    owned_sources['src/intel/wddm/' + name] = (ROOT / 'native/include' / name).read_text(encoding='utf-8')
for name in ['hasvk_wddm.cpp', 'hasvk_wddm_c.cpp', 'intel_2019_profile.cpp']:
    owned_sources['src/intel/wddm/' + name] = (ROOT / 'native/src' / name).read_text(encoding='utf-8')
owned_sources['src/intel/wddm_compiler/meson.build'] += '''
libhasvk_wddm = static_library('hasvk_wddm',
  ['../wddm/hasvk_wddm.cpp', '../wddm/hasvk_wddm_c.cpp', '../wddm/intel_2019_profile.cpp'],
  include_directories : [include_directories('../wddm')],
  cpp_args : ['/EHsc', '/U_HAS_EXCEPTIONS', '/D_HAS_EXCEPTIONS=1'],
  dependencies : [cpp.find_library('gdi32'), cpp.find_library('dxgi')],
)
executable('IrisImageProbe', 'image_probe.c',
  include_directories : [inc_include, inc_src, inc_intel],
  link_with : [libwddm_blorp, libisl, libhasvk_wddm],
  dependencies : [idep_nir, idep_intel_dev, idep_intel_compiler_elk, idep_genxml, idep_mesautil,
    cc.find_library('gdi32'), cc.find_library('dxgi')],
  c_args : [c_msvc_compat_args, '-DGFX_VERx10=80'],
  win_subsystem : 'console',
)
anv_wddm_entrypoints = custom_target('anv_wddm_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml], output : ['anv_entrypoints.h', 'anv_entrypoints.c'],
  command : [prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'anv',
    '--beta', with_vulkan_beta.to_string(), '--device-prefix', 'gfx8'],
  depend_files : vk_entrypoints_gen_depend_files,
)
anv_wddm_drirc = custom_target('anv_wddm_drirc',
  input : ['../vulkan_hasvk/anv_drirc_gen.py', drirc_gen, '../vulkan_hasvk/00-hasvk-defaults.conf'],
  output : ['hasvk_drirc.c', 'hasvk_drirc.h'],
  command : [prog_python, '@INPUT0@', '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@', '--drirc-hdr', '@OUTPUT1@', '--validate', '@INPUT2@'],
)
graphics_args = []
common_commands = ['anv_wddm_command_common.c']
gen8_commands = ['anv_wddm_compute_emit.c', 'anv_wddm_command_gen8.c']
if get_option('hasvk-wddm-graphics')
  graphics_args = ['-DHASVK_WDDM_GRAPHICS']
  common_commands = ['anv_wddm_graphics_common.c', 'anv_wddm_graphics_blorp.c']
  gen8_commands = ['anv_wddm_graphics_gen8.c', 'anv_wddm_graphics_pipeline.c',
    'anv_wddm_graphics_dynamic.c', 'anv_wddm_graphics_state.c', 'anv_wddm_graphics_blorp_exec.c']
endif
libanv_wddm_objects = static_library('anv_wddm_objects',
  ['../vulkan_hasvk/anv_wddm_device.c', '../vulkan_hasvk/anv_wddm_memory.c', '../vulkan_hasvk/anv_wddm_pools.c', '../vulkan_hasvk/anv_allocator.c',
   '../vulkan_hasvk/anv_descriptor_set.c',
   '../vulkan_hasvk/anv_image.c', '../vulkan_hasvk/anv_android_stubs.c',
   '../vulkan_hasvk/anv_pipeline.c', '../vulkan_hasvk/anv_pipeline_cache.c', '../vulkan_hasvk/anv_wddm_batch.c',
   '../vulkan_hasvk/anv_nir_apply_pipeline_layout.c', '../vulkan_hasvk/anv_nir_compute_push_layout.c',
   '../vulkan_hasvk/anv_nir_lower_multiview.c', '../vulkan_hasvk/anv_nir_lower_ubo_loads.c',
   '../vulkan_hasvk/anv_nir_lower_ycbcr_textures.c', '../vulkan_hasvk/anv_util.c',
   'anv_wddm_format_table.c',
   anv_wddm_entrypoints[0], anv_wddm_drirc[1]] + common_commands,
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_vulkan_runtime_headers,
    idep_vulkan_wsi_headers, idep_vulkan_util_headers],
  c_args : [c_msvc_compat_args, graphics_args],
)
libanv_wddm_gen8 = static_library('anv_wddm_gen8',
  ['../vulkan_hasvk/anv_wddm_command.c',
   anv_wddm_entrypoints[0], anv_wddm_drirc[1]] + gen8_commands,
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  dependencies : [idep_nir_headers, idep_genxml, idep_mesautil, idep_vulkan_runtime_headers,
    idep_vulkan_wsi_headers, idep_vulkan_util_headers],
  c_args : [c_msvc_compat_args, graphics_args, '-DGFX_VERx10=80'],
)
shared_library('vulkan_hasvk_wddm', anv_wddm_entrypoints[1],
  vs_module_defs : 'vulkan_hasvk_wddm.def',
  include_directories : [inc_include, inc_src, inc_intel, include_directories('../vulkan_hasvk')],
  link_whole : [libanv_wddm_objects, libanv_wddm_gen8],
  link_with : [libhasvk_wddm, libisl, libwddm_blorp],
  dependencies : [idep_nir, idep_intel_dev, idep_intel_compiler_elk, idep_mesautil,
    idep_vulkan_runtime, idep_vulkan_util, idep_vulkan_wsi,
    cc.find_library('gdi32'), cc.find_library('dxgi'), cc.find_library('user32'), cc.find_library('uuid')],
  c_args : c_msvc_compat_args,
  install : false,
)
'''
for name, contents in owned_sources.items():
    target = MESA / name
    target.parent.mkdir(exist_ok=True)
    if not target.exists() or target.read_text(encoding='utf-8') != contents:
        target.write_text(contents, encoding='utf-8', newline='\n')
    patches.extend(difflib.unified_diff([], contents.splitlines(True), '/dev/null', f'b/{name}'))
if patches:
    patch = ROOT / "mesa/patches/compiler-port.patch"
    patch.parent.mkdir(exist_ok=True)
    patch.write_text("".join(patches), encoding="utf-8", newline="\n")
print("Prepared Windows Mesa libraries and HasVK control-plane ICD (no shader command buffers)")
