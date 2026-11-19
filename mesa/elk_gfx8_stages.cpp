/*
 * Copyright 2013-2015 Intel Corporation (code taken from elk_vec4.cpp,
 * elk_vec4_gs_visitor.cpp and elk_vec4_tcs.cpp)
 * SPDX-License-Identifier: MIT
 *
 * Vertex, geometry and tessellation control shader entry points of ELK for
 * the Gen8-only Windows build. On Gen8 ELK compiles every stage as a scalar
 * (SIMD8) program (elk_compiler.c: scalar_stage[] is true for ver >= 8), so
 * the vec4 backend and its Gen6 geometry shader code are not built; these are
 * the upstream functions with only their scalar branch. The tessellation
 * evaluation entry point stays in elk_shader.cpp (its vec4 branch removed by
 * mesa/apply-compiler-port.py).
 */

#include "../intel_nir.h"
#include "compiler/intel_prim.h"
#include "elk_eu.h"
#include "elk_fs.h"
#include "elk_nir.h"
#include "elk_private.h"
#include "elk_vec4_gs_visitor.h"   /* struct elk_gs_compile */
#include "dev/intel_debug.h"
#include "util/u_math.h"

using namespace elk;

/* Type sizes in vec4 / dvec4 slots, used by the NIR input/output lowering
 * (elk_nir.c); from elk_vec4_visitor.cpp, which is not built. */
static int
elk_type_size_xvec4(const struct glsl_type *type, bool as_vec4, bool bindless)
{
   unsigned int i;
   int size;

   switch (type->base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_BFLOAT16:
   case GLSL_TYPE_FLOAT_E4M3FN:
   case GLSL_TYPE_FLOAT_E5M2:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_DOUBLE:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
      if (glsl_type_is_matrix(type)) {
         const glsl_type *col_type = glsl_get_column_type(type);
         unsigned col_slots =
            (as_vec4 && glsl_type_is_dual_slot(col_type)) ? 2 : 1;
         return type->matrix_columns * col_slots;
      } else {
         /* Regardless of size of vector, it gets a vec4. This is bad
          * packing for things like floats, but otherwise arrays become a
          * mess.  Hopefully a later pass over the code can pack scalars
          * down if appropriate.
          */
         return (as_vec4 && glsl_type_is_dual_slot(type)) ? 2 : 1;
      }
   case GLSL_TYPE_ARRAY:
      assert(type->length > 0);
      return elk_type_size_xvec4(type->fields.array, as_vec4, bindless) *
             type->length;
   case GLSL_TYPE_STRUCT:
   case GLSL_TYPE_INTERFACE:
      size = 0;
      for (i = 0; i < type->length; i++) {
	 size += elk_type_size_xvec4(type->fields.structure[i].type, as_vec4,
                                 bindless);
      }
      return size;
   case GLSL_TYPE_SUBROUTINE:
      return 1;

   case GLSL_TYPE_SAMPLER:
   case GLSL_TYPE_TEXTURE:
      /* Samplers and textures take up no register space, since they're baked
       * in at link time.
       */
      return bindless ? 1 : 0;
   case GLSL_TYPE_ATOMIC_UINT:
      return 0;
   case GLSL_TYPE_IMAGE:
      return bindless ? 1 : DIV_ROUND_UP(ISL_IMAGE_PARAM_SIZE, 4);
   case GLSL_TYPE_VOID:
   case GLSL_TYPE_ERROR:
   case GLSL_TYPE_COOPERATIVE_MATRIX:
      UNREACHABLE("not reached");
   }

   return 0;
}

/**
 * Returns the minimum number of vec4 elements needed to pack a type.
 *
 * For simple types, it will return 1 (a single vec4); for matrices, the
 * number of columns; for array and struct, the sum of the vec4_size of
 * each of its elements; and for sampler and atomic, zero.
 *
 * This method is useful to calculate how much register space is needed to
 * store a particular type.
 */
extern "C" unsigned
elk_type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return elk_type_size_xvec4(type, true, bindless);
}

/**
 * Returns the minimum number of dvec4 elements needed to pack a type.
 *
 * For simple types, it will return 1 (a single dvec4); for matrices, the
 * number of columns; for array and struct, the sum of the dvec4_size of
 * each of its elements; and for sampler and atomic, zero.
 *
 * This method is useful to calculate how much register space is needed to
 * store a particular type.
 *
 * Measuring double-precision vertex inputs as dvec4 is required because
 * ARB_vertex_attrib_64bit states that these uses the same number of locations
 * than the single-precision version. That is, two consecutives dvec4 would be
 * located in location "x" and location "x+1", not "x+2".
 *
 * In order to map vec4/dvec4 vertex inputs in the proper ATTRs,
 * remap_vs_attrs() will take in account both the location and also if the
 * type fits in one or two vec4 slots.
 */
extern "C" unsigned   /* as declared in elk_nir.h */
elk_type_size_dvec4(const struct glsl_type *type, bool bindless)
{
   return elk_type_size_xvec4(type, false, bindless);
}

static const GLuint gl_prim_to_hw_prim[MESA_PRIM_TRIANGLE_STRIP_ADJACENCY + 1] = {
   [MESA_PRIM_POINTS] = _3DPRIM_POINTLIST,
   [MESA_PRIM_LINES] = _3DPRIM_LINELIST,
   [MESA_PRIM_LINE_LOOP] = _3DPRIM_LINELOOP,
   [MESA_PRIM_LINE_STRIP] = _3DPRIM_LINESTRIP,
   [MESA_PRIM_TRIANGLES] = _3DPRIM_TRILIST,
   [MESA_PRIM_TRIANGLE_STRIP] = _3DPRIM_TRISTRIP,
   [MESA_PRIM_TRIANGLE_FAN] = _3DPRIM_TRIFAN,
   [MESA_PRIM_QUADS] = _3DPRIM_QUADLIST,
   [MESA_PRIM_QUAD_STRIP] = _3DPRIM_QUADSTRIP,
   [MESA_PRIM_POLYGON] = _3DPRIM_POLYGON,
   [MESA_PRIM_LINES_ADJACENCY] = _3DPRIM_LINELIST_ADJ,
   [MESA_PRIM_LINE_STRIP_ADJACENCY] = _3DPRIM_LINESTRIP_ADJ,
   [MESA_PRIM_TRIANGLES_ADJACENCY] = _3DPRIM_TRILIST_ADJ,
   [MESA_PRIM_TRIANGLE_STRIP_ADJACENCY] = _3DPRIM_TRISTRIP_ADJ,
};

/* Patches to accumulate before a MULTI_PATCH thread is launched (see the
 * 3DSTATE_HS::Patch Count Threshold documentation; 0 = wait for 8). */
static int
get_patch_count_threshold(int input_control_points)
{
   if (input_control_points <= 4)
      return 0;
   else if (input_control_points <= 6)
      return 5;
   else if (input_control_points <= 8)
      return 4;
   else if (input_control_points <= 10)
      return 3;
   else if (input_control_points <= 14)
      return 2;
   return 1;   /* PATCHLIST_15 - PATCHLIST_32 */
}

extern "C" const unsigned *
elk_compile_vs(const struct elk_compiler *compiler,
               struct elk_compile_vs_params *params)
{
   struct nir_shader *nir = params->base.nir;
   const struct elk_vs_prog_key *key = params->key;
   struct elk_vs_prog_data *prog_data = params->prog_data;
   const bool debug_enabled =
      elk_should_print_shader(nir, params->base.debug_flag ?
                                   params->base.debug_flag : DEBUG_VS);

   assert(compiler->scalar_stage[MESA_SHADER_VERTEX]);
   prog_data->base.base.stage = MESA_SHADER_VERTEX;
   prog_data->base.base.total_scratch = 0;

   elk_nir_apply_key(nir, compiler, &key->base, 8);

   prog_data->inputs_read = nir->info.inputs_read;
   prog_data->double_inputs_read = nir->info.vs.double_inputs;

   elk_nir_lower_vs_inputs(nir, params->edgeflag_is_last, key->gl_attrib_wa_flags);
   elk_nir_lower_vue_outputs(nir);
   elk_postprocess_nir(nir, compiler, debug_enabled, key->base.robust_flags);

   prog_data->base.clip_distance_mask = ((1 << nir->info.clip_distance_array_size) - 1);
   prog_data->base.cull_distance_mask =
      ((1 << nir->info.cull_distance_array_size) - 1) << nir->info.clip_distance_array_size;

   unsigned nr_attribute_slots = util_bitcount64(prog_data->inputs_read);

   /* gl_VertexID and gl_InstanceID are system values, but arrive via an
    * incoming vertex attribute. So, add an extra slot. */
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FIRST_VERTEX) ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE) ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE) ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID))
      nr_attribute_slots++;

   /* gl_DrawID and IsIndexedDraw share their very own vec4 */
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID) ||
       BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_IS_INDEXED_DRAW))
      nr_attribute_slots++;

   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_IS_INDEXED_DRAW))
      prog_data->uses_is_indexed_draw = true;
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FIRST_VERTEX))
      prog_data->uses_firstvertex = true;
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE))
      prog_data->uses_baseinstance = true;
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE))
      prog_data->uses_vertexid = true;
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID))
      prog_data->uses_instanceid = true;
   if (BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID))
      prog_data->uses_drawid = true;

   /* "Vertex URB Entry Read Length" may be 0 in SIMD8 mode. */
   prog_data->base.urb_read_length = DIV_ROUND_UP(nr_attribute_slots, 2);
   prog_data->nr_attribute_slots = nr_attribute_slots;

   /* Vertex shaders reuse the same VUE entry for inputs and outputs
    * (overwriting the original contents): size it for the larger of the two. */
   const unsigned vue_entries = MAX2(nr_attribute_slots, (unsigned)prog_data->base.vue_map.num_slots);
   prog_data->base.urb_entry_size = DIV_ROUND_UP(vue_entries, 4);

   if (unlikely(debug_enabled)) {
      fprintf(stderr, "VS Output ");
      elk_print_vue_map(stderr, &prog_data->base.vue_map, MESA_SHADER_VERTEX);
   }

   const unsigned dispatch_width = 8;
   prog_data->base.dispatch_mode = INTEL_DISPATCH_MODE_SIMD8;

   elk_fs_visitor v(compiler, &params->base, &key->base, &prog_data->base.base, nir, dispatch_width,
                    params->base.stats != NULL, debug_enabled);
   if (!v.run_vs()) {
      params->base.error_str = ralloc_strdup(params->base.mem_ctx, v.fail_msg);
      return NULL;
   }

   assert(v.payload().num_regs % reg_unit(compiler->devinfo) == 0);
   prog_data->base.base.dispatch_grf_start_reg = v.payload().num_regs / reg_unit(compiler->devinfo);

   elk_fs_generator g(compiler, &params->base, &prog_data->base.base, v.runtime_check_aads_emit,
                      MESA_SHADER_VERTEX);
   if (unlikely(debug_enabled)) {
      const char *debug_name = ralloc_asprintf(params->base.mem_ctx, "%s vertex shader %s",
                                               nir->info.label ? nir->info.label : "unnamed", nir->info.name);
      g.enable_debug(debug_name);
   }
   g.generate_code(v.cfg, dispatch_width, v.shader_stats, v.performance_analysis.require(), params->base.stats);
   g.add_const_data(nir->constant_data, nir->constant_data_size);
   return g.get_assembly();
}

extern "C" const unsigned *
elk_compile_gs(const struct elk_compiler *compiler,
               struct elk_compile_gs_params *params)
{
   nir_shader *nir = params->base.nir;
   const struct elk_gs_prog_key *key = params->key;
   struct elk_gs_prog_data *prog_data = params->prog_data;

   struct elk_gs_compile c;
   memset(&c, 0, sizeof(c));
   c.key = *key;

   assert(compiler->scalar_stage[MESA_SHADER_GEOMETRY]);
   const bool debug_enabled = elk_should_print_shader(nir, DEBUG_GS);

   prog_data->base.base.stage = MESA_SHADER_GEOMETRY;
   prog_data->base.base.total_scratch = 0;

   /* For SSO pipelines a fixed VUE map layout based on variable locations is
    * used, so rendezvous-by-location works. */
   GLbitfield64 inputs_read = nir->info.inputs_read;
   elk_compute_vue_map(compiler->devinfo, &c.input_vue_map, inputs_read,
                       nir->info.separate_shader ? INTEL_VUE_LAYOUT_SEPARATE : INTEL_VUE_LAYOUT_FIXED, 1);

   elk_nir_apply_key(nir, compiler, &key->base, 8);
   elk_nir_lower_vue_inputs(nir, &c.input_vue_map);
   elk_nir_lower_vue_outputs(nir);
   elk_postprocess_nir(nir, compiler, debug_enabled, key->base.robust_flags);

   prog_data->base.clip_distance_mask = ((1 << nir->info.clip_distance_array_size) - 1);
   prog_data->base.cull_distance_mask =
      ((1 << nir->info.cull_distance_array_size) - 1) << nir->info.clip_distance_array_size;

   prog_data->include_primitive_id = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);
   prog_data->invocations = nir->info.gs.invocations;

   nir_gs_count_vertices_and_primitives(nir, &prog_data->static_vertex_count, nullptr, nullptr, 1u);

   if (nir->info.gs.output_primitive == MESA_PRIM_POINTS) {
      /* Points: data may go to multiple streams and EndPrimitive() has no
       * effect, so the control data is the stream ID (only needed with
       * non-zero streams). */
      prog_data->control_data_format = GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_SID;
      c.control_data_bits_per_vertex = nir->info.gs.active_stream_mask != (1 << 0) ? 2 : 0;
   } else {
      /* Strips: EndPrimitive() terminates the strip (cut bits), one stream. */
      prog_data->control_data_format = GFX7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT;
      c.control_data_bits_per_vertex = nir->info.gs.uses_end_primitive ? 1 : 0;
   }
   c.control_data_header_size_bits = nir->info.gs.vertices_out * c.control_data_bits_per_vertex;

   /* 1 HWORD = 32 bytes = 256 bits */
   prog_data->control_data_header_size_hwords = align(c.control_data_header_size_bits, 256) / 256;

   /* Output vertex size: a multiple of 32 bytes (2 vec4's), at most 62*16
    * bytes (3DSTATE_GS::Output Vertex Size); see elk_vec4_gs_visitor.cpp
    * upstream for the budget. */
   unsigned output_vertex_size_bytes = prog_data->base.vue_map.num_slots * 16;
   assert(output_vertex_size_bytes <= GFX7_MAX_GS_OUTPUT_VERTEX_SIZE_BYTES);
   prog_data->output_vertex_size_hwords = align(output_vertex_size_bytes, 32) / 32;

   /* One URB entry holds all the output: the control data header, the
    * vertices, and on Broadwell the "Vertex Count" (a full 8-DWord output
    * before the control header). The maximum entry size is 32k. */
   unsigned output_size_bytes = prog_data->output_vertex_size_hwords * 32 * nir->info.gs.vertices_out;
   output_size_bytes += 32 * prog_data->control_data_header_size_hwords;
   output_size_bytes += 32;
   if (output_size_bytes == 0)
      output_size_bytes = 1;   /* max_vertices = 0 */
   if (output_size_bytes > GFX7_MAX_GS_URB_ENTRY_SIZE_BYTES)
      return NULL;

   /* URB entry sizes are stored as a multiple of 64 bytes. */
   prog_data->base.urb_entry_size = align(output_size_bytes, 64) / 64;

   assert(nir->info.gs.output_primitive < ARRAY_SIZE(gl_prim_to_hw_prim));
   prog_data->output_topology = gl_prim_to_hw_prim[nir->info.gs.output_primitive];
   prog_data->vertices_in = nir->info.gs.vertices_in;

   /* GS inputs are read from the VUE 256 bits (2 vec4's) at a time. */
   prog_data->base.urb_read_length = (c.input_vue_map.num_slots + 1) / 2;

   if (unlikely(debug_enabled)) {
      fprintf(stderr, "GS Input ");
      elk_print_vue_map(stderr, &c.input_vue_map, MESA_SHADER_GEOMETRY);
      fprintf(stderr, "GS Output ");
      elk_print_vue_map(stderr, &prog_data->base.vue_map, MESA_SHADER_GEOMETRY);
   }

   elk_fs_visitor v(compiler, &params->base, &c, prog_data, nir, params->base.stats != NULL, debug_enabled);
   if (!v.run_gs()) {
      params->base.error_str = ralloc_strdup(params->base.mem_ctx, v.fail_msg);
      return NULL;
   }
   prog_data->base.dispatch_mode = INTEL_DISPATCH_MODE_SIMD8;

   assert(v.payload().num_regs % reg_unit(compiler->devinfo) == 0);
   prog_data->base.base.dispatch_grf_start_reg = v.payload().num_regs / reg_unit(compiler->devinfo);

   elk_fs_generator g(compiler, &params->base, &prog_data->base.base, false, MESA_SHADER_GEOMETRY);
   if (unlikely(debug_enabled)) {
      const char *label = nir->info.label ? nir->info.label : "unnamed";
      g.enable_debug(ralloc_asprintf(params->base.mem_ctx, "%s geometry shader %s", label, nir->info.name));
   }
   g.generate_code(v.cfg, v.dispatch_width, v.shader_stats, v.performance_analysis.require(), params->base.stats);
   g.add_const_data(nir->constant_data, nir->constant_data_size);
   return g.get_assembly();
}

extern "C" const unsigned *
elk_compile_tcs(const struct elk_compiler *compiler,
                struct elk_compile_tcs_params *params)
{
   const struct intel_device_info *devinfo = compiler->devinfo;
   nir_shader *nir = params->base.nir;
   const struct elk_tcs_prog_key *key = params->key;
   struct elk_tcs_prog_data *prog_data = params->prog_data;
   struct elk_vue_prog_data *vue_prog_data = &prog_data->base;

   assert(compiler->scalar_stage[MESA_SHADER_TESS_CTRL]);
   const bool debug_enabled = elk_should_print_shader(nir, DEBUG_TCS);

   vue_prog_data->base.stage = MESA_SHADER_TESS_CTRL;
   prog_data->base.base.total_scratch = 0;

   nir->info.outputs_written = key->outputs_written;
   nir->info.patch_outputs_written = key->patch_outputs_written;

   struct intel_vue_map input_vue_map;
   elk_compute_vue_map(devinfo, &input_vue_map, nir->info.inputs_read,
                       nir->info.separate_shader ? INTEL_VUE_LAYOUT_SEPARATE : INTEL_VUE_LAYOUT_FIXED, 1);
   elk_compute_tess_vue_map(&vue_prog_data->vue_map, nir->info.outputs_written, nir->info.patch_outputs_written);

   elk_nir_apply_key(nir, compiler, &key->base, 8);
   elk_nir_lower_vue_inputs(nir, &input_vue_map);
   elk_nir_lower_tcs_outputs(nir, &vue_prog_data->vue_map, key->_tes_primitive_mode);
   if (key->quads_workaround)
      intel_nir_apply_tcs_quads_workaround(nir);
   intel_nir_lower_patch_vertices_in(nir, key->input_vertices);

   elk_postprocess_nir(nir, compiler, debug_enabled, key->base.robust_flags);

   prog_data->patch_count_threshold = get_patch_count_threshold(key->input_vertices);

   const unsigned verts_per_thread = 8;
   vue_prog_data->dispatch_mode = INTEL_DISPATCH_MODE_TCS_SINGLE_PATCH;
   prog_data->instances = DIV_ROUND_UP(nir->info.tess.tcs_vertices_out, verts_per_thread);

   /* URB entry: the patch header (counted in num_per_patch_slots), per-patch
    * and per-vertex varyings; at most 32k. */
   const int num_per_patch_slots = vue_prog_data->vue_map.num_per_patch_slots;
   const int num_per_vertex_slots = vue_prog_data->vue_map.num_per_vertex_slots;
   unsigned output_size_bytes = num_per_patch_slots * 16;
   output_size_bytes += nir->info.tess.tcs_vertices_out * num_per_vertex_slots * 16;

   assert(output_size_bytes >= 1);
   if (output_size_bytes > GFX7_MAX_HS_URB_ENTRY_SIZE_BYTES)
      return NULL;

   /* URB entry sizes are stored as a multiple of 64 bytes. */
   vue_prog_data->urb_entry_size = align(output_size_bytes, 64) / 64;

   /* HS does not use the usual payload pushing from URB to GRFs. */
   vue_prog_data->urb_read_length = 0;

   if (unlikely(debug_enabled)) {
      fprintf(stderr, "TCS Input ");
      elk_print_vue_map(stderr, &input_vue_map, MESA_SHADER_TESS_CTRL);
      fprintf(stderr, "TCS Output ");
      elk_print_vue_map(stderr, &vue_prog_data->vue_map, MESA_SHADER_TESS_CTRL);
   }

   const unsigned dispatch_width = 8;
   elk_fs_visitor v(compiler, &params->base, &key->base, &prog_data->base.base, nir, dispatch_width,
                    params->base.stats != NULL, debug_enabled);
   if (!v.run_tcs()) {
      params->base.error_str = ralloc_strdup(params->base.mem_ctx, v.fail_msg);
      return NULL;
   }

   assert(v.payload().num_regs % reg_unit(devinfo) == 0);
   prog_data->base.base.dispatch_grf_start_reg = v.payload().num_regs / reg_unit(devinfo);

   elk_fs_generator g(compiler, &params->base, &prog_data->base.base, false, MESA_SHADER_TESS_CTRL);
   if (unlikely(debug_enabled)) {
      g.enable_debug(ralloc_asprintf(params->base.mem_ctx, "%s tessellation control shader %s",
                                     nir->info.label ? nir->info.label : "unnamed", nir->info.name));
   }
   g.generate_code(v.cfg, dispatch_width, v.shader_stats, v.performance_analysis.require(), params->base.stats);
   g.add_const_data(nir->constant_data, nir->constant_data_size);
   return g.get_assembly();
}
