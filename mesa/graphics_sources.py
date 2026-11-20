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


def fragment_dispatch(source):
    """Upstream forces fragment shader dispatch (3DSTATE_WM::ForceThreadDispatch)
    for every draw whose colour writes are all masked, which includes every
    depth-only pass: a fragment shader with no effect then runs on each pixel
    (half the depth-only rate, measured: 10 vs 22 Gpixels/s, and shadow maps
    are such passes). Kill and side effects already force dispatch on Gfx8
    (pipeline->force_fragment_thread_dispatch); with masked colour writes the
    shader can still matter only through depth, stencil or sample-mask
    outputs, so dispatch is forced for those. Otherwise the hardware's own
    decision stands, as with no fragment shader."""
    old = '''         .ForceThreadDispatchEnable = anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT) &&
                                      (pipeline->force_fragment_thread_dispatch ||
                                       anv_cmd_buffer_all_color_write_masked(cmd_buffer)) ?
                                      ForceON : 0,'''
    new = '''         .ForceThreadDispatchEnable = anv_pipeline_has_stage(pipeline, MESA_SHADER_FRAGMENT) &&
                                      (pipeline->force_fragment_thread_dispatch ||
                                       (anv_cmd_buffer_all_color_write_masked(cmd_buffer) &&
                                        (get_fs_prog_data(pipeline)->computed_depth_mode != PSCDEPTH_OFF ||
                                         get_fs_prog_data(pipeline)->computed_stencil ||
                                         get_fs_prog_data(pipeline)->uses_omask))) ?
                                      ForceON : 0,'''
    assert source.count(old) == 1
    source = source.replace(old, new)
    old = '   cmd_buffer->state.pma_fix_enabled = enable;\n'
    assert source.count(old) == 1
    source = source.replace(old, old + '''#ifdef _WIN32
   if (anv_wddm_profile_pipe_bits_enabled)
      anv_wddm_profile_pipe_bits(1, enable ? "PMA fix toggled on" : "PMA fix toggled off");
#endif
''')
    return source


def scratch_address(source):
    """Scratch space of a shader (register spills, large local arrays): the
    3DSTATE_*S / MEDIA_VFE_STATE scratch pointer is relative to General State
    Base Address. Upstream sets that base to 0 and asks for a BO below 4 GiB;
    this port's GPU address window starts above 4 GiB and its general state
    base is the window's start (device->wddm->origin), so the pointer is the
    BO's offset inside the window (anv_wddm_scratch_address). Before this the
    port refused ANV_BO_ALLOC_32BIT_ADDRESS, every scratch pointer was 0 and
    spills went to unmapped addresses: wrong results in every shader that
    spills (CTS graphicsfuzz, clustered i64vec4 subgroup operations)."""
    old = '''   return (struct anv_address) {
      .bo = anv_scratch_pool_alloc(pipeline->device,
                                   &pipeline->device->scratch_pool,
                                   stage, bin->prog_data->total_scratch),
      .offset = 0,
   };'''
    assert source.count(old) == 1
    source = source.replace(old, '''   return anv_wddm_scratch_address(pipeline->device, stage,
                                   bin->prog_data->total_scratch);''')
    old = 'static UNUSED struct anv_address\nget_scratch_address('
    assert source.count(old) == 1
    return source.replace(old, SCRATCH_PROTOTYPE + old)


def double_attribute_width(source):
    """A 64-bit vertex attribute goes into the URB without conversion, as 128
    bits (one or two components) or 256 bits (three or four), by its format
    (see the Broadwell note in vertex_element_comp_control). The vertex
    shader, however, reads one slot for double and dvec2 and two for dvec3 and
    dvec4, by its own type. Upstream sizes the element by the format alone, so
    a format with more components than the shader reads (R64G64B64 into a
    double) stored 256 bits where the shader expected 128, and everything
    after it in the vertex was read from the wrong place (CTS
    pipeline.*.vertex_input.component_mismatch.r64g64b64*_to_double/dvec2).
    Such an attribute is now fetched as R64G64: the extra components are not
    read at all."""
    old = '''      struct GENX(VERTEX_ELEMENT_STATE) element = {
         .VertexBufferIndex = vi->attributes[a].binding,'''
    assert source.count(old) == 1
    return source.replace(old, '''      /* elements_double marks dvec3 and dvec4 inputs (two slots); a 64-bit
       * attribute outside it is a double or dvec2 and takes one slot. */
      if (!(elements_double & (1u << a)) &&
          (format == ISL_FORMAT_R64G64B64_PASSTHRU ||
           format == ISL_FORMAT_R64G64B64A64_PASSTHRU))
         format = ISL_FORMAT_R64G64_PASSTHRU;

''' + old)


SCRATCH_PROTOTYPE = '''struct anv_address
anv_wddm_scratch_address(struct anv_device *device, mesa_shader_stage stage,
                         unsigned per_thread_scratch);

'''


def _gen8_condition(expression):
    """Value of an #if/#elif condition made only of GFX_VER/GFX_VERx10,
    integers, comparisons and logical operators, for Gen8; None otherwise."""
    expression = expression.split('/*')[0].split('//')[0].strip()
    if not expression or expression.endswith('\\') or 'defined' in expression:
        return None
    text = expression.replace('GFX_VERx10', '80').replace('GFX_VER', '8')
    if not re.fullmatch(r'[0-9\s()<>=!&|]+', text):
        return None
    text = text.replace('&&', ' and ').replace('||', ' or ')
    text = re.sub(r'!(?!=)', ' not ', text)
    try:
        return bool(eval(text, {'__builtins__': {}}, {}))
    except Exception:
        return None


def unifdef_gen8(source):
    """Drops the branches of #if/#elif/#else chains whose conditions depend
    only on GFX_VER/GFX_VERx10 and are false on Gen8 (these files are compiled
    with GFX_VERx10=80 only), and the directives of such chains. Chains with
    any other condition are kept as they are."""
    out, stack = [], []   # frames: [known, active, taken]
    def emitting():
        return all(frame[1] for frame in stack if frame[0])
    for line in source.split('\n'):
        stripped = line.strip()
        directive = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', stripped)
        if not directive:
            if emitting():
                out.append(line)
            continue
        kind, rest = directive.group(1), directive.group(2)
        if kind in ('if', 'ifdef', 'ifndef'):
            value = _gen8_condition(rest) if kind == 'if' else None
            if value is None:
                if emitting():
                    out.append(line)
                stack.append([False, True, False])
            else:
                stack.append([True, value, value])
        elif kind == 'elif':
            frame = stack[-1]
            if not frame[0]:
                if emitting():
                    out.append(line)
                continue
            if frame[2]:
                frame[1] = False
                continue
            value = _gen8_condition(rest)
            if value is None:
                # Not foldable after foldable branches: keep the rest as an #if.
                frame[0], frame[1] = False, True
                stack.pop(); parent = emitting(); stack.append(frame)
                if parent:
                    out.append(line.replace('#elif', '#if', 1))
                continue
            frame[1] = value; frame[2] = value
        elif kind == 'else':
            frame = stack[-1]
            if frame[0]:
                frame[1] = not frame[2]; frame[2] = True
            elif emitting():
                out.append(line)
        else:   # endif
            frame = stack.pop()
            if not frame[0] and emitting():
                out.append(line)
    assert not stack, 'unbalanced preprocessor conditionals'
    return '\n'.join(out)


def hiz_clear_value(hardware, blorp):
    """HiZ fast clears with the image's own clear value
    (anv_image_hiz_clear_value in anv_private.h) instead of the fixed 1.0:
    allowed for any image whose clear value matches, including Gfx8 images
    sampled with HiZ (value 0.0, which is what the sampler returns for
    fast-cleared blocks), and programmed the same way by 3DSTATE_CLEAR_PARAMS,
    the HiZ clear and the resolve/ambiguate ops."""
    old = '''   if (depth_clear_value != ANV_HZ_FC_VAL)
      return false;

   /* Only gfx9+ supports returning ANV_HZ_FC_VAL when sampling a fast-cleared
    * portion of a HiZ buffer. Testing has revealed that Gfx8 only supports
    * returning 0.0f. Gens prior to gfx8 do not support this feature at all.
    */
   if (GFX_VER == 8 && anv_can_sample_with_hiz(device->info, iview->image))
      return false;
'''
    new = '''   /* Gfx8 samplers return 0.0 for fast-cleared HiZ blocks, so images sampled
    * with HiZ fast-clear only to 0.0 (anv_image_hiz_clear_value). */
   if (depth_clear_value != anv_image_hiz_clear_value(device->info, iview->image))
      return false;
'''
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, new)
    old = '         info.depth_clear_value = ANV_HZ_FC_VAL;\n'
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, '         info.depth_clear_value = anv_image_hiz_clear_value(device->info, image);\n')
    old = '''         blorp_surf->clear_color = (union isl_color_value) {
            .f32 = { ANV_HZ_FC_VAL },
         };'''
    assert blorp.count(old) == 1
    blorp = blorp.replace(old, '''         blorp_surf->clear_color = (union isl_color_value) {
            .f32 = { anv_image_hiz_clear_value(device->info, image) },
         };''')
    old = '''                                 aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
                                 ANV_HZ_FC_VAL,'''
    assert blorp.count(old) == 1
    blorp = blorp.replace(old, '''                                 aspects & VK_IMAGE_ASPECT_DEPTH_BIT,
                                 anv_image_hiz_clear_value(cmd_buffer->device->info, image),''')
    return hardware, blorp


def dynamic_buffer_surface_state(hardware):
    """emit_binding_table writes a surface state for every dynamic uniform or
    storage buffer of every draw; the general ISL path was ~6 % of the
    driver's CPU time per draw. The port fills it from a per-device template
    (anv_wddm_fill_dynamic_buffer_surface_state, anv_wddm_memory.c), which is
    checked against ISL when the device is created."""
    old = '''               anv_fill_buffer_surface_state(cmd_buffer->device, surface_state,
                                             format, ISL_SWIZZLE_IDENTITY,
                                             usage, address, range, 1);
               if (need_client_mem_relocs)'''
    assert hardware.count(old) == 1
    return hardware.replace(old, '''               (void)format; (void)usage;
               anv_wddm_fill_dynamic_buffer_surface_state(cmd_buffer->device,
                                                          surface_state,
                                                          desc->type, address,
                                                          range);
               if (need_client_mem_relocs)''')


def push_constants_active_stages(hardware):
    """Push constants (3DSTATE_CONSTANT_*) and binding table/sampler pointers
    only for the stages the bound pipeline has.

    Upstream flushes every dirty graphics stage, and a pipeline change or a
    vkCmdPushConstants with VK_SHADER_STAGE_ALL_GRAPHICS dirties all five: a
    VS+FS pipeline then gets three empty 3DSTATE_CONSTANT_* and pointer packets
    per draw for stages that are disabled. Inactive stages keep their dirty
    bits and are flushed when a pipeline that has them is bound (so a later
    pipeline with the same push layout cannot see stale constants). The one
    exception is a re-programmed push constant allocation: the BDW PRM requires
    3DSTATE_CONSTANT_* for every stage after 3DSTATE_PUSH_CONSTANT_ALLOC_*, so
    that flush still covers all of them."""
    old = '''       !vk_dynamic_graphics_state_any_dirty(dyn) &&
       !cmd_buffer->state.push_constants_dirty)
      return;
'''
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, '''       !vk_dynamic_graphics_state_any_dirty(dyn) &&
       !(cmd_buffer->state.push_constants_dirty &
         (pipeline->active_stages | ~VK_SHADER_STAGE_ALL_GRAPHICS)))
      return;
''')
    old = '''   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) {
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->base.batch);

      /* If the pipeline changed, we may need to re-allocate push constant
       * space in the URB.
       */
      cmd_buffer_alloc_push_constants(cmd_buffer);
   }
'''
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, '''   bool push_alloc_changed = false;
   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) {
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->base.batch);

      /* If the pipeline changed, we may need to re-allocate push constant
       * space in the URB.
       */
      const VkShaderStageFlags push_stages = cmd_buffer->state.gfx.push_constant_stages;
      cmd_buffer_alloc_push_constants(cmd_buffer);
      push_alloc_changed = cmd_buffer->state.gfx.push_constant_stages != push_stages;
   }
''')
    old = '''   if (dirty || cmd_buffer->state.push_constants_dirty) {
      /* Because we're pushing UBOs, we have to push whenever either
       * descriptors or push constants is dirty.
       */
      dirty |= cmd_buffer->state.push_constants_dirty;
      cmd_buffer_flush_push_constants(cmd_buffer,
                                      dirty & VK_SHADER_STAGE_ALL_GRAPHICS);
   }
'''
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, '''   /* Disabled stages wait with their push constants until a pipeline that has
    * them is bound, except right after 3DSTATE_PUSH_CONSTANT_ALLOC_*, which
    * requires 3DSTATE_CONSTANT_* for every stage (BDW PRM).
    */
   const VkShaderStageFlags push_dirty = cmd_buffer->state.push_constants_dirty &
      (push_alloc_changed ? VK_SHADER_STAGE_ALL_GRAPHICS : pipeline->active_stages);
   if (dirty || push_dirty) {
      /* Because we're pushing UBOs, we have to push whenever either
       * descriptors or push constants is dirty.
       */
      dirty |= push_dirty;
      cmd_buffer_flush_push_constants(cmd_buffer,
                                      dirty & VK_SHADER_STAGE_ALL_GRAPHICS);
   }
''')
    return hardware


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

    # Upstream capability tables (extensions, features, limits) for the physical
    # device; anv_wddm_device.c restricts them to what this port supports.
    capabilities = '#include "anv_wddm.h"\n#include "util/os_misc.h"\n#include "vk_util.h"\n' \
        '/* No intel_perf on Windows: no performance-query extensions. */\n' \
        '#define intel_perf_has_hold_preemption(perf) false\n' \
        '#include "git_sha1.h"\n' \
        '/* Context priorities cannot be set through WDDM here: compare as unsupported. */\n' \
        '#define INTEL_CONTEXT_MEDIUM_PRIORITY 0x7fffffff\n' \
        '#define ANV_API_VERSION_1_3 VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)\n' \
        '#define ANV_API_VERSION_1_2 VK_MAKE_VERSION(1, 2, VK_HEADER_VERSION)\n' \
        '#define MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BUFFERS   64\n' \
        '#define MAX_PER_STAGE_DESCRIPTOR_INPUT_ATTACHMENTS 64\n' \
        '#define MAX_DESCRIPTOR_SET_INPUT_ATTACHMENTS       256\n' \
        '#define MAX_CUSTOM_BORDER_COLORS                   4096\n'
    for signature in ['static void\nget_device_extensions(', 'static void\nget_features(', 'static void\nget_properties_1_1(',
                      'static void\nget_properties_1_2(', 'static void\nget_properties_1_3(', 'static void\nget_properties(']:
        capabilities += function(device, signature) + '\n'
    # driconf is not loaded on Windows: instance->drirc stays zero, so its string
    # and option-cache members must not be dereferenced.
    capabilities, count = re.subn(r'\(strlen\(pdevice->instance->drirc\.debug\.force_vk_devicename\) > 0\) \?\s*pdevice->instance->drirc\.debug\.force_vk_devicename : pdevice->info\.name',
                                  'pdevice->info.name', capabilities)
    assert count == 1, count
    capabilities, count = re.subn(r'wsi_instance_supports_google_display_timing\([^;]*?\),', 'false,', capabilities)
    assert count == 1, count
    capabilities += '''void
anv_wddm_upstream_capabilities(const struct anv_physical_device *pdevice, struct vk_device_extension_table *ext,
                               struct vk_features *features, struct vk_properties *props)
{
   get_device_extensions(pdevice, ext);
   get_features(pdevice, features);
   get_properties(pdevice, props);
}
'''

    hardware = read('genX_cmd_buffer.c').replace('#include "anv_private.h"', '#include "anv_wddm.h"')
    # Begin/End/ExecuteCommands stay upstream; the Windows command-buffer file
    # wraps them for runtime state tracking and supplies the batch-chain hooks.
    # Each command buffer selects its pipeline before STATE_BASE_ADDRESS because
    # the WDDM context is shared with whatever ran before.
    old = '   genX(cmd_buffer_emit_state_base_address)(cmd_buffer);\n\n   /* We sometimes store vertex data'
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, '   genX(flush_pipeline_select_3d)(cmd_buffer);\n' + old)
    hardware = hardware.replace('ALWAYS_INLINE enum anv_pipe_bits\ngenX(emit_apply_pipe_flushes)',
                                'enum anv_pipe_bits\ngenX(emit_apply_pipe_flushes)')
    hardware = hardware.replace('ALWAYS_INLINE void\ngenX(cmd_buffer_apply_pipe_flushes)',
                                'void\ngenX(cmd_buffer_apply_pipe_flushes)')
    old = 'sba.GeneralStateBaseAddress = (struct anv_address) { NULL, 0 };'
    assert hardware.count(old) == 1
    hardware = hardware.replace(old, 'sba.GeneralStateBaseAddress = (struct anv_address) { NULL, intel_canonical_address(device->wddm->origin) };')
    # Events live in their own BO (anv_wddm_device.c): the dynamic state pool is
    # refreshed from a CPU shadow at every submit and its GPU addresses are not
    # block_pool.bo + offset in this port.
    hardware, count = re.subn(r'\(struct anv_address\) \{\s*cmd_buffer->device->dynamic_state_pool\.block_pool\.bo,\s*event->state\.offset\s*\}',
                              'anv_wddm_event_address(cmd_buffer->device, event)', hardware)
    assert count == 3, count
    # MI_SEMAPHORE_WAIT never completes in a WDDM context (tested: it hangs on an
    # event the host had already set), so waits split the batch instead.
    hardware, count = re.subn(r'#if GFX_VER >= 8\n   for \(uint32_t i = 0; i < eventCount; i\+\+\) \{.*?#else\n   anv_finishme\("Implement events on gfx7"\);\n#endif\n',
                              '   anv_wddm_cmd_wait_events(cmd_buffer, eventCount, pEvents);\n', hardware, flags=re.S)
    assert count == 1, count
    # Modifier images are explicitly rejected by the Windows image path.
    hardware = hardware.replace('image->vk.drm_format_mod', 'DRM_FORMAT_MOD_INVALID')
    state = read('genX_state.c').replace('#include <unistd.h>', '')
    state = remove_function(state, 'void\ngenX(init_physical_device_state)(')
    blorp = read('anv_blorp.c')
    start = blorp.index('   switch (device->info->verx10) {')
    end = blorp.index('\n   }', start) + len('\n   }')
    blorp = blorp[:start] + '   device->blorp.exec = gfx8_blorp_exec;' + blorp[end:]
    hardware, blorp = hiz_clear_value(hardware, blorp)
    hardware = push_constants_active_stages(hardware)
    hardware = dynamic_buffer_surface_state(hardware)
    # A failed shader upload (out of memory) marks the command buffer failed:
    # vkEndCommandBuffer then reports it instead of BLORP asserting.
    marker = '''                               NULL, 0, NULL, &bind_map);

   if (!bin)
      return false;
'''
    assert blorp.count(marker) == 1
    blorp = blorp.replace(marker, '''                               NULL, 0, NULL, &bind_map);

   if (!bin) {
      struct anv_cmd_buffer *cmd_buffer = batch->driver_batch;
      if (cmd_buffer)
         anv_batch_set_error(&cmd_buffer->batch, VK_ERROR_OUT_OF_HOST_MEMORY);
      return false;
   }
''')
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
       pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP &&
       pCreateInfo->queryType != VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT &&
       pCreateInfo->queryType != VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT)
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);
''')
    # Files compiled with GFX_VERx10=80 only (wddm_compiler/meson.build):
    # their Gfx7/7.5 (and Gfx9+) preprocessor branches are dropped.
    return {
        'src/intel/wddm_compiler/anv_wddm_graphics_query.c': unifdef_gen8(query),
        'src/intel/wddm_compiler/anv_wddm_capabilities.c': capabilities,
        'src/intel/wddm_compiler/anv_wddm_graphics_memcpy.c': unifdef_gen8(read('genX_gpu_memcpy.c')),
        'src/intel/wddm_compiler/anv_wddm_graphics_common.c': instrumentation(common),
        'src/intel/wddm_compiler/anv_wddm_graphics_gen8.c': unifdef_gen8(instrumentation(hardware)),
        'src/intel/wddm_compiler/anv_wddm_graphics_state.c': unifdef_gen8(state),
        'src/intel/wddm_compiler/anv_wddm_graphics_blorp.c': blorp,
        'src/intel/wddm_compiler/anv_wddm_graphics_blorp_exec.c': unifdef_gen8(instrumentation(read('genX_blorp_exec.c'))),
        'src/intel/wddm_compiler/anv_wddm_graphics_dynamic.c': unifdef_gen8(fragment_dispatch(read('gfx8_cmd_buffer.c').replace('#include <unistd.h>', ''))),
        'src/intel/wddm_compiler/anv_wddm_graphics_pipeline.c': unifdef_gen8(double_attribute_width(scratch_address(read('genX_pipeline.c').replace('#include <unistd.h>', '')))),
    }
