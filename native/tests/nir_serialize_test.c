/* Round-trips NIR through nir_serialize/nir_deserialize - the path of HasVK's
 * NIR cache (anv_device_upload_nir / anv_device_search_for_nir): the second
 * pipeline built from a shader module gets its NIR back from the cache.
 *
 * Checks the packed fields that outgrew their bits in Mesa 26.2.7: the jump
 * type (nir_jump_continue = 4 came back as nir_jump_return from two bits),
 * the texture opcode and the texture source type (the highest value of each).
 * Prints every value before and after; exit code 1 if any changed. */
#include <stdio.h>
#include "nir.h"
#include "nir_builder.h"
#include "nir_serialize.h"
#include "util/blob.h"
#include "compiler/glsl_types.h"

#define MAX_ITEMS 16

struct observed {
   int count;
   char kind[MAX_ITEMS];   /* 'j' jump type, 'o' tex opcode, 's' tex source type */
   int value[MAX_ITEMS];
};

static void observe(nir_shader *s, struct observed *o)
{
   o->count = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_jump && o->count < MAX_ITEMS) {
               o->kind[o->count] = 'j';
               o->value[o->count++] = nir_instr_as_jump(instr)->type;
            } else if (instr->type == nir_instr_type_tex) {
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               if (o->count < MAX_ITEMS) {
                  o->kind[o->count] = 'o';
                  o->value[o->count++] = tex->op;
               }
               for (unsigned i = 0; i < tex->num_srcs && o->count < MAX_ITEMS; i++) {
                  o->kind[o->count] = 's';
                  o->value[o->count++] = tex->src[i].src_type;
               }
            }
         }
      }
   }
}

int main(void)
{
   static const nir_shader_compiler_options options = {0};
   glsl_type_singleton_init_or_ref();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, &options, "serialize");
   nir_def *index = nir_load_local_invocation_index(&b);

   /* loop { if (index == 0) continue; break; } */
   nir_loop *loop = nir_push_loop(&b);
   nir_push_if(&b, nir_ieq_imm(&b, index, 0));
   nir_jump(&b, nir_jump_continue);
   nir_pop_if(&b, NULL);
   nir_jump(&b, nir_jump_break);
   nir_pop_loop(&b, loop);

   /* One texture instruction with the highest opcode and the highest source
    * type (serialization does not interpret either). */
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = (nir_texop)(nir_texop_sparse_residency_txf_intel);
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_uint32;
   tex->coord_components = 2;
   tex->src[0] = nir_tex_src_for_ssa((nir_tex_src_type)(nir_num_tex_src_types - 1), index);
   nir_def_init(&tex->instr, &tex->def, 1, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   struct blob blob;
   blob_init(&blob);
   nir_serialize(&blob, b.shader, false);
   struct blob_reader reader;
   blob_reader_init(&reader, blob.data, blob.size);
   nir_shader *copy = nir_deserialize(NULL, &options, &reader);

   struct observed before, after;
   observe(b.shader, &before);
   observe(copy, &after);
   /* continue, break, tex opcode, one tex source */
   int failed = before.count != after.count || before.count != 4;
   for (int i = 0; i < before.count && i < after.count; i++) {
      const char *what = before.kind[i] == 'j' ? "jump type" : before.kind[i] == 'o' ? "tex opcode" : "tex source type";
      printf("%-16s %3d -> %3d%s\n", what, before.value[i], after.value[i],
             before.value[i] != after.value[i] ? "  CHANGED" : "");
      failed |= before.kind[i] != after.kind[i] || before.value[i] != after.value[i];
   }
   printf("%s: NIR serialize round trip (%d values, %u bytes)\n", failed ? "FAIL" : "PASS",
          before.count, (unsigned)blob.size);
   blob_finish(&blob);
   ralloc_free(copy);
   ralloc_free(b.shader);
   glsl_type_singleton_decref();
   return failed;
}
