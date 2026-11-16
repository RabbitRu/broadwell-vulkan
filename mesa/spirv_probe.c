/* Deliberately bounded frontend test: Vulkan compute, set 0/binding 0 SSBO,
 * fixed 1D workgroups. Full HasVK pipeline-layout lowering remains a separate step. */
#include <stdio.h>
#include "spirv_probe.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"

static void spirv_debug(void *data, enum nir_spirv_debug_level level,
                        size_t word, const char *message)
{
   (void)data;
   printf("SPIR-V[%d] at word %zu: %s\n", level, word, message);
}

static bool lower_single_ssbo(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   bool *valid = data;
   switch (intrinsic->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      if (nir_intrinsic_desc_set(intrinsic) != 0 || nir_intrinsic_binding(intrinsic) != 0 ||
          !nir_src_is_const(intrinsic->src[0]) || nir_src_as_uint(intrinsic->src[0]) != 0) {
         *valid = false; return false;
      }
      break;
   case nir_intrinsic_vulkan_resource_reindex:
      if (!nir_src_is_const(intrinsic->src[1]) || nir_src_as_uint(intrinsic->src[1]) != 0) {
         *valid = false; return false;
      }
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      break;
   default:
      return false;
   }
   if (nir_intrinsic_desc_type(intrinsic) != nir_descriptor_type_storage_buffer) {
      *valid = false; return false;
   }
   b->cursor = nir_before_instr(&intrinsic->instr);
   nir_def *zero = nir_imm_zero(b, intrinsic->def.num_components, intrinsic->def.bit_size);
   nir_def_rewrite_uses(&intrinsic->def, zero);
   nir_instr_remove(&intrinsic->instr);
   return true;
}

nir_shader *hasvk_spirv_probe_read(void *memory, const wchar_t *path,
                                  const nir_shader_compiler_options *options)
{
   FILE *input = _wfopen(path, L"rb");
   if (!input) return NULL;
   if (fseek(input, 0, SEEK_END)) { fclose(input); return NULL; }
   long bytes = ftell(input);
   if (bytes < 20 || bytes > 1024 * 1024 || (bytes & 3) || fseek(input, 0, SEEK_SET)) {
      fclose(input); return NULL;
   }
   uint32_t *words = ralloc_array(memory, uint32_t, bytes / 4);
   bool read_ok = words && fread(words, 1, bytes, input) == (size_t)bytes;
   fclose(input);
   if (!read_ok || words[0] != 0x07230203) return NULL;
   const struct spirv_capabilities capabilities = {.Shader = true, .Matrix = true};
   const struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &capabilities,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ubo_alignment = 16, .min_ssbo_alignment = 16,
      .debug = {.func = spirv_debug},
      .skip_os_break_in_debug_build = true,
      .ignore_replacement = true,
   };
   nir_shader *nir = spirv_to_nir(words, bytes / 4, NULL, MESA_SHADER_COMPUTE,
                                   "main", &spirv_options, options);
   if (!nir) return NULL;
   ralloc_steal(memory, nir);
   if (nir->info.workgroup_size_variable || !nir->info.workgroup_size[0] ||
       nir->info.workgroup_size[0] > 64 ||
       nir->info.workgroup_size[1] != 1 || nir->info.workgroup_size[2] != 1)
      return NULL;
   nir_foreach_variable_with_modes(var, nir, nir_var_uniform | nir_var_mem_ubo |
                                   nir_var_mem_push_const | nir_var_mem_shared) {
      (void)var;
      return NULL;
   }
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ssbo) {
      if (var->data.descriptor_set != 0 || var->data.binding != 0) return NULL;
   }
   bool valid = true;
   nir_shader_intrinsics_pass(nir, lower_single_ssbo, nir_metadata_control_flow, &valid);
   if (!valid) return NULL;
   nir_lower_explicit_io(nir, nir_var_mem_ssbo, nir_address_format_32bit_index_offset);
   nir_opt_copy_prop(nir);
   nir_opt_constant_folding(nir);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "after single-SSBO Vulkan descriptor lowering");
   return nir;
}
