/* Offline compiler smoke test. This does not create or advertise a Vulkan ICD. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include "compiler/glsl_types.h"
#include "compiler/nir/nir_builder.h"
#include "intel/compiler/elk/elk_compiler.h"
#include "intel/compiler/elk/elk_eu.h"
#include "intel/compiler/elk/elk_nir.h"
#include "intel/dev/intel_device_info.h"
#include "spirv_probe.h"
extern bool hasvk_elk_cpp_abi_check(const struct elk_reg *, struct elk_reg *, unsigned);

static void compiler_log(void *data, unsigned *id, const char *format, ...)
{
   (void)data; (void)id;
   va_list arguments;
   va_start(arguments, format);
   fputs("ELK: ", stdout);
   vfprintf(stdout, format, arguments);
   va_end(arguments);
}

static bool lower_zero_dispatch_base(nir_builder *b, nir_intrinsic_instr *intrinsic, void *data)
{
   (void)data;
   if (intrinsic->intrinsic != nir_intrinsic_load_base_workgroup_id) return false;
   b->cursor = nir_before_instr(&intrinsic->instr);
   nir_def_replace(&intrinsic->def, nir_imm_zero(b, 3, 32));
   return true;
}

int wmain(int argc, wchar_t **argv)
{
   bool spirv_input = argc > 1 && !wcscmp(argv[1], L"--spirv");
   int first_output = spirv_input ? 3 : 1;
   if (argc != first_output + 2 && argc != first_output + 3) {
      fprintf(stderr, "Usage: IrisElkCompile.exe [--spirv INPUT_SPV] KERNEL_BIN REPORT_JSON [UINT32_VALUE]\n");
      return 2;
   }
   uint32_t value = 0x51c0ffee;
   if (argc == first_output + 3) {
      wchar_t *end;
      unsigned long long parsed = wcstoull(argv[first_output + 2], &end, 0);
      if (*end || parsed > UINT32_MAX) return 2;
      value = (uint32_t)parsed;
   }
   process_intel_debug_variable();
   struct elk_reg from_c, from_cpp;
   memset(&from_c, 0, sizeof(from_c)); memset(&from_cpp, 0, sizeof(from_cpp));
   from_c.file = VGRF; from_c.type = ELK_REGISTER_TYPE_UW;
   from_c.subnr = 2; from_c.nr = 17; from_c.negate = from_c.abs = 1;
   if (!hasvk_elk_cpp_abi_check(&from_c, &from_cpp, sizeof(from_c)) ||
       from_cpp.file != BAD_FILE || from_cpp.type != ELK_REGISTER_TYPE_UD ||
       from_cpp.subnr != 31 || from_cpp.nr != 99 || from_cpp.writemask != 15) {
      fprintf(stderr, "ELK C/C++ register ABI mismatch\n");
      return 1;
   }
   struct intel_device_info devinfo;
   if (!intel_get_device_info_for_build(0x162b, &devinfo) || devinfo.ver != 8) return 1;
   glsl_type_singleton_init_or_ref();
   void *memory = ralloc_context(NULL);
   struct elk_compiler *compiler = elk_compiler_create(memory, &devinfo);
   if (!compiler) return 1;
   compiler->shader_debug_log = compiler_log;
   compiler->shader_perf_log = compiler_log;
   nir_shader *shader;
   if (spirv_input) {
      shader = hasvk_spirv_probe_read(memory, argv[2], compiler->nir_options[MESA_SHADER_COMPUTE]);
      if (!shader) { fprintf(stderr, "SPIR-V rejected by the single-SSBO compute test frontend\n"); return 1; }
   } else {
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      compiler->nir_options[MESA_SHADER_COMPUTE], "wddm_constant_store");
   ralloc_steal(memory, b.shader);
   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   nir_store_ssbo(&b, nir_imm_int(&b, value), nir_imm_int(&b, 0), nir_imm_int(&b, 0),
                  .write_mask = 1, .align_mul = 4, .align_offset = 0);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   shader = b.shader;
   }
   struct elk_nir_compiler_opts options = {0};
   elk_preprocess_nir(compiler, shader, &options);
   /* This bounded launcher implements Dispatch, with base group (0,0,0).
    * HasVK will instead lower the Vulkan base through its push layout. */
   nir_shader_intrinsics_pass(shader, lower_zero_dispatch_base, nir_metadata_control_flow, NULL);
   struct elk_cs_prog_key key = {0};
   struct elk_cs_prog_data program = {0};
   elk_nir_lower_cs_intrinsics(shader, &devinfo, &program);
   struct elk_compile_cs_params params = {
      .base = {.mem_ctx = memory, .nir = shader},
      .key = &key, .prog_data = &program,
   };
   const unsigned *code = elk_compile_cs(compiler, &params);
   if (!code || !program.base.program_size) {
      fprintf(stderr, "ELK compilation failed: %s\n", params.base.error_str ? params.base.error_str : "unknown error");
      return 1;
   }
   if (!elk_validate_instructions(&compiler->isa, code, 0, program.base.program_size, NULL)) {
      fprintf(stderr, "ELK ISA validation failed\n");
      return 1;
   }
   const struct intel_cs_dispatch_info dispatch = elk_cs_get_dispatch_info(&devinfo, &program, NULL);
   FILE *binary = _wfopen(argv[first_output], L"wb");
   if (!binary) return 1;
   bool written = fwrite(code, 1, program.base.program_size, binary) == program.base.program_size;
   if (fclose(binary) || !written) return 1;
   FILE *report = _wfopen(argv[first_output + 1], L"wb");
   if (!report) return 1;
   fprintf(report,
      "{\"schema\":1,\"compiler\":\"Mesa 26.2.7 ELK\",\"gen\":8,"
      "\"input\":\"%s\",\"isSpirvCompilerProof\":%s,\"isaValidated\":true,\"registerAbiValidated\":true,"
      "\"expectedValue\":%u,\"programBytes\":%u,\"progMask\":%u,"
      "\"programOffsets\":[%u,%u,%u],\"dispatchGrfStartReg\":%u,"
      "\"scratchBytes\":%u,\"sharedBytes\":%u,\"nrParams\":%u,"
      "\"crossThreadRegs\":%u,\"perThreadRegs\":%u,\"pushSubgroupIdOnly\":%s,"
      "\"localSize\":[%u,%u,%u],\"dispatchThreads\":%u,\"rightMask\":%u,\"usesBarrier\":%s,"
      "\"maxCsThreads\":%u,\"subsliceTotal\":%u,\"passed\":true}",
      spirv_input ? "SPIR-V" : "NIR builder", spirv_input ? "true" : "false",
      value, program.base.program_size, program.prog_mask,
      program.prog_offset[0], program.prog_offset[1], program.prog_offset[2], program.base.dispatch_grf_start_reg,
      program.base.total_scratch, program.base.total_shared, program.base.nr_params,
      program.push.cross_thread.regs, program.push.per_thread.regs,
      program.base.nr_params == 1 && program.base.param[0] == ELK_PARAM_BUILTIN_SUBGROUP_ID ? "true" : "false",
      program.local_size[0], program.local_size[1], program.local_size[2], dispatch.threads,
      dispatch.right_mask, program.uses_barrier ? "true" : "false",
      devinfo.max_cs_threads, devinfo.subslice_total);
   int status = fclose(report);
   printf("ELK generated %u bytes of Gen8 ISA, mask %u\n", program.base.program_size, program.prog_mask);
   ralloc_free(memory);
   glsl_type_singleton_decref();
   return status ? 1 : 0;
}
