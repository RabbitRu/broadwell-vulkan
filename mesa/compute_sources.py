"""Select the pinned HasVK compute path without linking Linux submission.

Bodies remain upstream code except the explicitly listed Windows address and
instrumentation adaptations. Unsupported graphics/conditional rendering and
performance tracing are not exposed by the bring-up ICD.
"""
import re


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f"Ambiguous upstream function: {signature}")
    start = source.index(signature)
    end = source.index("\n}\n", source.index("{", start)) + 3
    return source[start:end] + "\n"


def generate_compute_sources(originals):
    def read(name):
        return (originals / ("src/intel/vulkan_hasvk/" + name)).read_text(encoding="utf-8")

    command = read("anv_cmd_buffer.c")
    chain = read("anv_batch_chain.c")
    gen = read("genX_cmd_buffer.c")
    state = read("genX_state.c")
    license_text = command[:command.index("#include")]
    common = license_text + '#include "anv_wddm.h"\n#include "vk_util.h"\n'
    for signature in [
        "static bool\nmem_update(", "static void\nset_dirty_for_bind_map(",
        "void anv_CmdBindPipeline(", "static void\nanv_cmd_buffer_bind_descriptor_set(",
        "void anv_CmdBindDescriptorSets2KHR(", "struct anv_state\nanv_cmd_buffer_merge_dynamic(",
        "struct anv_state\nanv_cmd_buffer_cs_push_constants(", "void anv_CmdPushConstants2KHR(",
    ]:
        common += function(command, signature)
    for signature in [
        "struct anv_address\nanv_cmd_buffer_surface_base_address(",
        "struct anv_state\nanv_cmd_buffer_alloc_binding_table(",
        "struct anv_state\nanv_cmd_buffer_alloc_surface_state(",
        "struct anv_state\nanv_cmd_buffer_alloc_dynamic_state(",
        "VkResult\nanv_cmd_buffer_new_binding_table_block(",
    ]:
        common += function(chain, signature)

    hardware = license_text + '''#include "anv_wddm.h"
#include "vk_format.h"
#include "vk_util.h"
#include "common/intel_l3_config.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"
static void genX(flush_pipeline_select)(struct anv_cmd_buffer *, uint32_t);
'''
    hardware += function(gen, "static enum anv_pipe_bits\nconvert_pc_to_bits(")
    hardware += gen[gen.index("#define anv_debug_dump_pc"):gen.index("static bool\nis_render_queue_cmd_buffer")]
    hardware += function(state, "void\ngenX(emit_l3_config)(")
    for signature in [
        "void\ngenX(cmd_buffer_emit_state_base_address)(", "static void\nadd_surface_reloc(",
        "static void\nadd_surface_state_relocs(", "void\ngenX(cmd_buffer_config_l3)(",
        "ALWAYS_INLINE enum anv_pipe_bits\ngenX(emit_apply_pipe_flushes)(",
        "ALWAYS_INLINE void\ngenX(cmd_buffer_apply_pipe_flushes)(",
        "static VkResult\nemit_binding_table(", "static VkResult\nemit_samplers(",
        "static uint32_t\nflush_descriptor_sets(", "static void\ngenX(cmd_buffer_flush_compute_state)(",
        "static void\nanv_cmd_buffer_push_base_group_id(", "static inline void\nemit_gpgpu_walker(",
        "static inline void\nemit_cs_walker(", "void genX(CmdDispatchBase)(",
        "static void\ngenX(flush_pipeline_select)(", "void\ngenX(flush_pipeline_select_gpgpu)(",
        "void\ngenX(flush_pipeline_select_3d)(",
    ]:
        body = function(gen, signature)
        if signature.startswith("ALWAYS_INLINE "):
            body = body.replace("ALWAYS_INLINE ", "", 1)
        if "genX(cmd_buffer_emit_state_base_address)(" in signature:
            old = "sba.GeneralStateBaseAddress = (struct anv_address) { NULL, 0 };"
            assert body.count(old) == 1
            body = body.replace(old, "sba.GeneralStateBaseAddress = (struct anv_address) { NULL, intel_canonical_address(device->wddm->origin) };")
        if "genX(cmd_buffer_apply_pipe_flushes)(" in signature:
            body, count = re.subn(r"   bool trace_flush =.*?trace_intel_begin_stall\(&cmd_buffer->trace\);\n", "", body, flags=re.S)
            assert count == 1
            body, count = re.subn(r"   if \(trace_flush\) \{.*?\n   }\n", "", body, flags=re.S)
            assert count == 1
        if "genX(CmdDispatchBase)(" in signature:
            for name in ["anv_measure_snapshot", "trace_intel_begin_compute", "trace_intel_end_compute"]:
                body, count = re.subn(r"\n   " + name + r"\([^;]*\);\n", "\n", body)
                assert count == 1
            body, count = re.subn(r"   if \(cmd_buffer->state.conditional_render_enabled\)\n      genX\(cmd_emit_conditional_render_predicate\)\(cmd_buffer\);\n", "", body)
            assert count == 1
        hardware += body
    return {
        "src/intel/wddm_compiler/anv_wddm_command_common.c": common,
        "src/intel/wddm_compiler/anv_wddm_command_gen8.c": hardware,
    }
