"""Pinned upstream graphics emission with Windows transport kept separate.

Only lifecycle/submission and unavailable performance instrumentation are
removed. Shader compilation, raster state, draw and BLORP remain Mesa code.
"""
import re
from compute_sources import function


def remove_function(source, signature):
    body = function(source, signature)
    return source.replace(body.rstrip("\n"), "")


def instrumentation(source):
    source = source.replace('#include "ds/intel_tracepoints.h"', '')
    # Keep a statement, including beneath unbraced if statements.
    return re.sub(r'\b(?:trace_intel_\w+|anv_measure_\w+)\([^;]*\);',
                  '(void)0;', source)


def generate_graphics_sources(originals):
    def read(name):
        return (originals / ('src/intel/vulkan_hasvk/' + name)).read_text(encoding='utf-8')

    common = read('anv_cmd_buffer.c')
    start = common.index('static void\nanv_cmd_state_init(')
    end = common.index('void\nanv_cmd_buffer_emit_state_base_address(')
    # The owned Windows command-buffer ops manage BOs and streams.
    finish = function(common, 'static void\nanv_cmd_pipeline_state_finish(')
    finish += function(common, 'static void\nanv_cmd_state_finish(').replace(
        'static void\nanv_cmd_state_finish(', 'void\nanv_wddm_cmd_state_finish(')
    common = common[:start] + finish + common[end:]
    common = common.replace('#include <unistd.h>', '')
    chain = read('anv_batch_chain.c')
    for signature in [
        'struct anv_address\nanv_cmd_buffer_surface_base_address(',
        'struct anv_state\nanv_cmd_buffer_alloc_binding_table(',
        'struct anv_state\nanv_cmd_buffer_alloc_surface_state(',
        'struct anv_state\nanv_cmd_buffer_alloc_dynamic_state(',
        'VkResult\nanv_cmd_buffer_new_binding_table_block(',
    ]:
        common += function(chain, signature)
    device = read('anv_device.c')
    common += function(device, 'static struct anv_state\nanv_state_pool_emit_data(')
    common += function(device, 'static void\nanv_device_init_border_colors(').replace(
        'static void\nanv_device_init_border_colors(', 'void\nanv_wddm_init_border_colors(')
    common += function(device, 'void anv_DestroySampler(')

    hardware = read('genX_cmd_buffer.c').replace('#include "anv_private.h"', '#include "anv_wddm.h"')
    for signature in ['VkResult\ngenX(BeginCommandBuffer)(', 'VkResult\ngenX(EndCommandBuffer)(',
                      'void\ngenX(CmdExecuteCommands)(']:
        hardware = remove_function(hardware, signature)
    hardware = hardware.replace('ALWAYS_INLINE enum anv_pipe_bits\ngenX(emit_apply_pipe_flushes)',
                                'enum anv_pipe_bits\ngenX(emit_apply_pipe_flushes)')
    hardware = hardware.replace('ALWAYS_INLINE void\ngenX(cmd_buffer_apply_pipe_flushes)',
                                'void\ngenX(cmd_buffer_apply_pipe_flushes)')
    old = 'sba.GeneralStateBaseAddress = (struct anv_address) { NULL, 0 };'
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, 'sba.GeneralStateBaseAddress = (struct anv_address) { NULL, intel_canonical_address(device->wddm->origin) };')
    # Modifier images are explicitly rejected by the Windows image path.
    hardware = hardware.replace('image->vk.drm_format_mod', 'DRM_FORMAT_MOD_INVALID')
    state = read('genX_state.c').replace('#include <unistd.h>', '')
    state = remove_function(state, 'void\ngenX(init_physical_device_state)(')
    blorp = read('anv_blorp.c')
    start = blorp.index('   switch (device->info->verx10) {')
    end = blorp.index('\n   }', start) + len('\n   }')
    blorp = blorp[:start] + '   device->blorp.exec = gfx8_blorp_exec;' + blorp[end:]
    # Upstream query code is kept verbatim. Performance-query extensions are
    # not exposed and intel_perf is not built for Windows, so those unreachable
    # branches are compiled out through no-op macros and their pool types are
    # rejected at creation.
    query = read('genX_query.c')
    for include in ['#include <unistd.h>\n', '#include <fcntl.h>\n']:
        assert query.count(include) == 1
        query = query.replace(include, '')
    marker = '#include "vk_util.h"\n'
    assert query.count(marker) == 1
    query = query.replace(marker, marker + '''
#define intel_perf_get_n_passes(...) 0
#define intel_perf_get_counters_passes(...) ((void)0)
#define intel_perf_query_result_clear(...) ((void)0)
#define intel_perf_query_result_accumulate_fields(...) ((void)0)
#define intel_perf_query_result_write_mdapi(...) ((void)0)
#define intel_perf_query_mdapi_write_marker(...) ((void)0)
#define anv_perf_write_pass_results(...) ((void)0)
''')
    marker = '''   ANV_FROM_HANDLE(anv_device, device, _device);
   const struct anv_physical_device *pdevice = device->physical;
'''
    assert query.count(marker) == 1
    query = query.replace(marker, marker + '''   if (pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION &&
       pCreateInfo->queryType != VK_QUERY_TYPE_PIPELINE_STATISTICS &&
       pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP)
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
''')
    return {
        'src/intel/wddm_compiler/anv_wddm_graphics_query.c': query,
        'src/intel/wddm_compiler/anv_wddm_graphics_common.c': instrumentation(common),
        'src/intel/wddm_compiler/anv_wddm_graphics_gen8.c': instrumentation(hardware),
        'src/intel/wddm_compiler/anv_wddm_graphics_state.c': state,
        'src/intel/wddm_compiler/anv_wddm_graphics_blorp.c': blorp,
        'src/intel/wddm_compiler/anv_wddm_graphics_blorp_exec.c': instrumentation(read('genX_blorp_exec.c')),
        'src/intel/wddm_compiler/anv_wddm_graphics_dynamic.c': read('gfx8_cmd_buffer.c').replace('#include <unistd.h>', ''),
        'src/intel/wddm_compiler/anv_wddm_graphics_pipeline.c': read('genX_pipeline.c').replace('#include <unistd.h>', ''),
    }
