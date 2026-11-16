/* Hardware resource bring-up using the real Mesa Gen8 BLORP executor.
 * No Vulkan entrypoints are implemented by this test executable. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <direct.h>
#include "compiler/glsl_types.h"
#include "compiler/elk/elk_eu.h"
#include "wddm/hasvk_wddm.h"
#include "blorp/blorp_genX_exec_elk.h"

struct pool {
   struct hasvk_wddm_bo *bo;
   uint32_t next, size;
   struct hasvk_wddm_readonly_pool *shadow;
   uint64_t address;
};
struct image {
   struct isl_surf surf;
   struct hasvk_wddm_bo *bo;
   uint32_t bytes;
};
struct probe {
   struct hasvk_wddm_device *device;
   struct intel_device_info devinfo;
   struct isl_device isl;
   struct elk_compiler *compiler;
   struct blorp_context blorp;
   struct blorp_batch batch;
   struct pool commands, dynamic, surfaces, code;
   struct hasvk_wddm_bo *entry, *scratch;
   uint64_t reservation, origin;
   void *memory;
   unsigned shaders;
   bool triangle;
   float triangle_z;
   struct image *triangle_depth;
};
static void require(bool valid, const char *message)
{
   if (!valid) { fprintf(stderr, "%s: %s\n", message, hasvk_wddm_error()); exit(1); }
}
static uint64_t canonical(uint64_t address)
{
   return address & (1ull << 47) ? address | 0xffff000000000000ull : address;
}
static void compiler_log(void *data, unsigned *id, const char *format, ...)
{
   (void)data; (void)id;
   va_list args; va_start(args, format); vfprintf(stdout, format, args); va_end(args);
}
static struct hasvk_wddm_bo *new_bo(struct probe *p, uint32_t bytes, uint64_t offset)
{
   struct hasvk_wddm_bo *bo = NULL;
   require(!hasvk_wddm_bo_create(p->device, bytes, p->origin + offset, 1, &bo), "Create WDDM BO");
   memset(hasvk_wddm_bo_map(bo), 0, bytes);
   return bo;
}
static void *pool_alloc(struct pool *pool, uint32_t size, uint32_t alignment, uint32_t *offset)
{
   uint32_t start = align(pool->next, alignment);
   require(start <= pool->size && size <= pool->size - start, "Test state pool exhausted");
   pool->next = start + size;
   *offset = start;
   if (pool->shadow) {
      void *map;
      require(!hasvk_wddm_readonly_pool_map(pool->shadow, start, size, &map), "Commit readonly pool");
      return map;
   }
   return (char *)hasvk_wddm_bo_map(pool->bo) + start;
}
static void *pool_map(struct pool *pool, uint32_t offset, uint32_t bytes)
{
   if (!pool->shadow) return (char *)hasvk_wddm_bo_map(pool->bo) + offset;
   void *map;
   require(!hasvk_wddm_readonly_pool_map(pool->shadow, offset, bytes, &map), "Map readonly pool");
   return map;
}
static void shadow_init(struct probe *p, struct pool *pool, uint64_t offset)
{
   pool->size = 1u << 30; pool->address = p->origin + offset;
   require(!hasvk_wddm_readonly_pool_create(p->device, pool->address, pool->size, &pool->shadow), "Create readonly pool");
   *(uint32_t *)pool_map(pool, 0, 4) = 0x13579bdf;
   pool->next = 65504; /* Force a new chunk and retain the first CPU pointer. */
}
static void *blorp_emit_dwords(struct blorp_batch *batch, unsigned n)
{
   struct probe *p = batch->driver_batch;
   uint32_t unused;
   return pool_alloc(&p->commands, n * 4, 4, &unused);
}
static uint64_t surface_address(struct blorp_batch *batch, struct blorp_address address)
{
   (void)batch;
   return canonical((address.buffer ? hasvk_wddm_bo_address(address.buffer) : 0) + address.offset);
}
static uint64_t blorp_emit_reloc(struct blorp_batch *batch, void *location,
                                struct blorp_address address, uint32_t delta)
{
   (void)location;
   return surface_address(batch, address) + delta;
}
static void blorp_measure_start(struct blorp_batch *b, const struct blorp_params *p) { (void)b; (void)p; }
static void blorp_measure_end(struct blorp_batch *b, const struct blorp_params *p) { (void)b; (void)p; }
static void *blorp_alloc_dynamic_state(struct blorp_batch *batch, uint32_t size,
                                       uint32_t alignment, uint32_t *offset)
{
   return pool_alloc(&((struct probe *)batch->driver_batch)->dynamic, size, alignment, offset);
}
static void *blorp_alloc_general_state(struct blorp_batch *batch, uint32_t size,
                                       uint32_t alignment, uint32_t *offset)
{
   return blorp_alloc_dynamic_state(batch, size, alignment, offset);
}
static void *blorp_alloc_vertex_buffer(struct blorp_batch *batch, uint32_t size, struct blorp_address *addr)
{
   uint32_t offset;
   void *map = blorp_alloc_dynamic_state(batch, size, 64, &offset);
   *addr = (struct blorp_address){.offset = canonical(((struct probe *)batch->driver_batch)->dynamic.address + offset), .mocs = 0x78};
   return map;
}
static void blorp_vf_invalidate_for_vb_48b_transitions(struct blorp_batch *batch,
   const struct blorp_address *addrs, uint32_t *sizes, unsigned num_vbs)
{
   (void)addrs; (void)sizes; (void)num_vbs;
   blorp_emit(batch, GENX(PIPE_CONTROL), pc) { pc.CommandStreamerStallEnable = true; }
   blorp_emit(batch, GENX(PIPE_CONTROL), pc) { pc.VFCacheInvalidationEnable = true; }
}
static struct blorp_address blorp_get_workaround_address(struct blorp_batch *batch)
{
   return (struct blorp_address){.buffer = ((struct probe *)batch->driver_batch)->scratch};
}
static bool blorp_alloc_binding_table(struct blorp_batch *batch, unsigned entries,
   unsigned state_size, unsigned state_alignment, uint32_t *bt_offset,
   uint32_t *surface_offsets, void **surface_maps)
{
   struct probe *p = batch->driver_batch;
   uint32_t *bt = pool_alloc(&p->surfaces, entries * 4, 32, bt_offset);
   for (unsigned i = 0; i < entries; ++i) {
      surface_maps[i] = pool_alloc(&p->surfaces, state_size, state_alignment, surface_offsets + i);
      bt[i] = surface_offsets[i];
   }
   return true;
}
static uint32_t blorp_binding_table_offset_to_pointer(struct blorp_batch *b, uint32_t o) { (void)b; return o; }
static void blorp_flush_range(struct blorp_batch *b, void *start, size_t size) { (void)b; (void)start; (void)size; }
static void blorp_surface_reloc(struct blorp_batch *b, uint32_t offset, struct blorp_address addr, uint32_t delta)
{
   struct probe *p = b->driver_batch;
   uint64_t value = surface_address(b, addr) + delta;
   require(offset <= p->surfaces.size - 8, "Surface relocation outside pool");
   memcpy(pool_map(&p->surfaces, offset, 8), &value, 8);
}
static struct blorp_address blorp_get_surface_base_address(struct blorp_batch *b)
{
   return (struct blorp_address){.offset = canonical(((struct probe *)b->driver_batch)->surfaces.address)};
}
static const struct intel_l3_config *blorp_get_l3_config(struct blorp_batch *b)
{
   return intel_get_default_l3_config(&((struct probe *)b->driver_batch)->devinfo);
}
static void blorp_pre_emit_urb_config(struct blorp_batch *b, struct intel_urb_config *c) { (void)b; (void)c; }
static void blorp_emit_urb_config(struct blorp_batch *b, struct intel_urb_config *c)
{
   (void)b; (void)c; require(false, "Gen8 must use its direct URB packets");
}
static void blorp_emit_pre_draw(struct blorp_batch *b, const struct blorp_params *p) { (void)b; (void)p; }
static void blorp_emit_post_draw(struct blorp_batch *b, const struct blorp_params *p) { (void)b; (void)p; }
static bool lookup_shader(struct blorp_batch *b, const void *key, uint32_t size, uint32_t *out, void *pd)
{
   (void)b; (void)key; (void)size; (void)out; (void)pd; return false;
}
static bool validate_variant(struct probe *p, const void *code, uint32_t bytes, uint32_t start)
{
   uint32_t end = start;
   while (end + 8 <= bytes) {
      const elk_inst *source = (const elk_inst *)((const char *)code + end);
      elk_inst decoded;
      unsigned size;
      if (elk_inst_cmpt_control(&p->devinfo, source)) {
         size = 8;
         elk_compact_inst compacted; memcpy(&compacted, source, sizeof(compacted));
         elk_uncompact_instruction(&p->compiler->isa, &decoded, &compacted);
      } else {
         size = 16;
         if (end + size > bytes) return false;
         memcpy(&decoded, source, sizeof(decoded));
      }
      end += size;
      unsigned opcode = elk_inst_opcode(&p->compiler->isa, &decoded);
      if ((opcode == ELK_OPCODE_SEND || opcode == ELK_OPCODE_SENDC) && elk_inst_eot(&p->devinfo, &decoded))
         return elk_validate_instructions(&p->compiler->isa, code, start, end, NULL);
   }
   return false;
}
static bool upload_shader(struct blorp_batch *batch, uint32_t stage, const void *key, uint32_t key_size,
   const void *kernel, uint32_t kernel_size, const void *data, uint32_t data_size, uint32_t *out, void *pd)
{
   (void)key; (void)key_size;
   struct probe *p = batch->driver_batch;
   bool valid = kernel && kernel_size;
   if (valid && stage == MESA_SHADER_FRAGMENT) {
      const struct elk_fs_prog_data *fs = data;
      if (fs->dispatch_8) valid &= validate_variant(p, kernel, kernel_size, 0);
      if (fs->dispatch_16) valid &= validate_variant(p, kernel, kernel_size, fs->prog_offset_16);
      if (fs->dispatch_32) valid &= validate_variant(p, kernel, kernel_size, fs->prog_offset_32);
   } else if (valid) {
      valid &= elk_validate_instructions(&p->compiler->isa, kernel, 0, kernel_size, NULL);
   }
   require(valid, "BLORP generated invalid ISA");
   const struct elk_stage_prog_data *base = data;
   require(!base->total_scratch && !base->nr_params, "Test does not implement shader scratch/push");
   memcpy(pool_alloc(&p->code, kernel_size, 64, out), kernel, kernel_size);
   *(void **)pd = ralloc_size(p->memory, data_size);
   require(*(void **)pd != NULL, "Program data allocation failed");
   memcpy(*(void **)pd, data, data_size);
   ++p->shaders;
   return true;
}
static void flush_caches(struct blorp_batch *b)
{
   blorp_emit(b, GENX(PIPE_CONTROL), pc) {
      pc.DepthCacheFlushEnable = pc.RenderTargetCacheFlushEnable = pc.DCFlushEnable = true;
      pc.CommandStreamerStallEnable = true;
   }
   blorp_emit(b, GENX(PIPE_CONTROL), pc) {
      pc.StateCacheInvalidationEnable = pc.TextureCacheInvalidationEnable = true;
      pc.ConstantCacheInvalidationEnable = pc.InstructionCacheInvalidateEnable = true;
   }
}
static void execute(struct blorp_batch *batch, const struct blorp_params *params)
{
   struct probe *p = batch->driver_batch;
   if (!p->triangle) { blorp_exec(batch, params); return; }
   /* Reuse Mesa's real Gen8 state emitters with TRIANGLELIST geometry. BLORP
    * normally emits RECTLIST. Vertex processing is its fixed-function path;
    * this proof does not yet exercise a SPIR-V vertex shader. */
   struct blorp_params draw = *params;
   struct blorp_surf depth = {.surf = &p->triangle_depth->surf, .aux_usage = ISL_AUX_USAGE_NONE,
      .addr = {.buffer = p->triangle_depth->bo, .mocs = 0x78}};
   blorp_surface_info_init(batch, &draw.depth, &depth, 0, 0, ISL_FORMAT_UNSUPPORTED, true);
   draw.depth_format = isl_format_get_depth_format(depth.surf->format, false);
   draw.z = p->triangle_z;
   blorp_emit_vertex_buffers(batch, &draw);
   blorp_emit_vertex_elements(batch, &draw);
   blorp_emit(batch, GENX(3DSTATE_VF_TOPOLOGY), topology) { topology.PrimitiveTopologyType = _3DPRIM_TRILIST; }
   blorp_emit_pipeline(batch, &draw);
   blorp_emit_btp(batch, blorp_setup_binding_table(batch, &draw));
   blorp_emit_depth_stencil_config(batch, &draw);
   blorp_emit(batch, GENX(3DSTATE_WM_DEPTH_STENCIL), ds) {
      ds.DepthTestEnable = ds.DepthBufferWriteEnable = true;
      ds.DepthTestFunction = COMPAREFUNCTION_LESS;
   }
   blorp_emit(batch, GENX(3DPRIMITIVE), primitive) {
      primitive.VertexAccessType = SEQUENTIAL;
      primitive.PrimitiveTopologyType = _3DPRIM_TRILIST;
      primitive.VertexCountPerInstance = 3;
      primitive.InstanceCount = 1;
   }
}
static void image_init(struct probe *p, struct image *image, enum isl_format format,
                        unsigned width, unsigned height, enum isl_tiling tiling, uint64_t offset, bool depth)
{
   require(isl_surf_init_s(&p->isl, &image->surf, &(struct isl_surf_init_info){.dim = ISL_SURF_DIM_2D, .format = format,
      .width = width, .height = height, .depth = 1, .levels = 1, .array_len = 1, .samples = 1,
      .usage = depth ? ISL_SURF_USAGE_DEPTH_BIT : ISL_SURF_USAGE_RENDER_TARGET_BIT | ISL_SURF_USAGE_TEXTURE_BIT,
      .tiling_flags = 1u << tiling}), "ISL layout rejected");
   image->bytes = align64(image->surf.size_B, 4096);
   image->bo = new_bo(p, image->bytes + 4096, offset);
   memset(hasvk_wddm_bo_map(image->bo), 0x5a, image->bytes + 4096);
}
static struct blorp_surf blorp_image(struct image *image)
{
   return (struct blorp_surf){.surf = &image->surf, .aux_usage = ISL_AUX_USAGE_NONE,
      .addr = {.buffer = image->bo, .mocs = 0x78}};
}
static void read_image(struct image *image, void *output, unsigned width, unsigned height, unsigned cpp)
{
   const char *source = hasvk_wddm_bo_map(image->bo);
   if (image->surf.tiling == ISL_TILING_LINEAR) {
      for (unsigned y = 0; y < height; ++y)
         memcpy((char *)output + y * width * cpp, source + y * image->surf.row_pitch_B, width * cpp);
   } else {
      isl_memcpy_tiled_to_linear(0, width * cpp, 0, height, output, source,
         width * cpp, image->surf.row_pitch_B, false, image->surf.tiling, ISL_MEMCPY);
   }
   for (uint32_t i = image->bytes; i < image->bytes + 4096; ++i)
      require((unsigned char)source[i] == 0x5a, "Image allocation guard corrupted");
}
static void dump(const wchar_t *directory, const wchar_t *name, const void *data, size_t bytes)
{
   wchar_t path[32768];
   require(swprintf(path, ARRAY_SIZE(path), L"%ls/%ls", directory, name) > 0, "Artifact path overflow");
   FILE *f = _wfopen(path, L"wb");
   require(f != NULL, "Cannot open artifact");
   require(fwrite(data, 1, bytes, f) == bytes && fclose(f) == 0, "Artifact write failed");
}
int wmain(int argc, wchar_t **argv)
{
   if (argc != 2 && argc != 5) return 2;
   unsigned width = 37, height = 23;
   enum isl_tiling tiling = ISL_TILING_Y0;
   if (argc == 5) {
      wchar_t *end;
      width = wcstoul(argv[2], &end, 10); if (*end) return 2;
      height = wcstoul(argv[3], &end, 10); if (*end) return 2;
      if (!wcscmp(argv[4], L"linear")) tiling = ISL_TILING_LINEAR;
      else if (!wcscmp(argv[4], L"x")) tiling = ISL_TILING_X;
      else if (wcscmp(argv[4], L"y")) return 2;
   }
   if (width < 8 || height < 8 || width > 513 || height > 513) return 2;
   _wmkdir(argv[1]);
   struct probe p = {0};
   process_intel_debug_variable(); glsl_type_singleton_init_or_ref();
   p.memory = ralloc_context(NULL);
   require(intel_get_device_info_for_build(0x162b, &p.devinfo), "No Broadwell PCI table");
   isl_device_init(&p.isl, &p.devinfo);
   p.compiler = elk_compiler_create(p.memory, &p.devinfo);
   require(p.compiler != NULL, "No ELK compiler");
   p.compiler->shader_debug_log = p.compiler->shader_perf_log = compiler_log;
   require(!hasvk_wddm_device_create(&p.device), "Create WDDM transport");
   require(!hasvk_wddm_reserve(p.device, 16ull << 30, 0, &p.reservation), "Reserve relocated HasVK layout");
   p.origin = align64(p.reservation, 1ull << 32);
   p.entry = new_bo(&p, 8192, 0x2c0000000);
   p.commands = (struct pool){new_bo(&p, 65536, 0x2c0010000), 0, 65536};
   shadow_init(&p, &p.dynamic, 0xc0000000);
   shadow_init(&p, &p.surfaces, 0x140000000);
   shadow_init(&p, &p.code, 0x180000000);
   /* Binding table pointers on Gen8 have 16 bits. Keep this initial table
    * at the surface base while leaving its state entries in the first chunk. */
   p.surfaces.next = 32;
   void *old_dynamic = pool_map(&p.dynamic, 0, 4), *old_code = pool_map(&p.code, 0, 4);
   void *invalid_map = NULL;
   require(hasvk_wddm_readonly_pool_map(p.dynamic.shadow, p.dynamic.size - 1, 2, &invalid_map) == -1 && !invalid_map,
      "Out-of-bounds pool map must fail");
   struct hasvk_wddm_bo *invalid_bo = NULL;
   require(hasvk_wddm_bo_create(p.device, 4096, p.dynamic.address + 0x100000, 1, &invalid_bo) == -1 && !invalid_bo,
      "Overlapping pool allocation must fail");
   p.scratch = new_bo(&p, 4096, 0x200000);
   blorp_init_elk(&p.blorp, &p, &p.isl, p.compiler, NULL);
   p.blorp.lookup_shader = lookup_shader; p.blorp.upload_shader = upload_shader;
   p.blorp.get_surface_address = surface_address; p.blorp.exec = execute;
   blorp_batch_init(&p.blorp, &p.batch, &p, 0);
   struct blorp_batch *b = &p.batch;
   blorp_emit(b, GENX(PIPELINE_SELECT), ps) { ps.PipelineSelection = _3D; }
   flush_caches(b);
   blorp_emit(b, GENX(3DSTATE_DRAWING_RECTANGLE), r) {
      r.ClippedDrawingRectangleXMax = r.ClippedDrawingRectangleYMax = UINT16_MAX;
   }
   blorp_emit(b, GENX(3DSTATE_AA_LINE_PARAMETERS), aa);
   blorp_emit(b, GENX(3DSTATE_WM_CHROMAKEY), ck);
   blorp_emit(b, GENX(3DSTATE_WM_HZ_OP), hz);
   blorp_emit(b, GENX(3DSTATE_SAMPLE_PATTERN), samples) { INTEL_SAMPLE_POS_1X(samples._1xSample); }
   const struct intel_l3_config *l3 = blorp_get_l3_config(b);
   struct GENX(L3CNTLREG) l3reg = {.URBAllocation = l3->n[INTEL_L3P_URB], .AllAllocation = l3->n[INTEL_L3P_ALL]};
   uint32_t l3bits; GENX(L3CNTLREG_pack)(NULL, &l3bits, &l3reg);
   blorp_emit(b, GENX(MI_LOAD_REGISTER_IMM), lri) { lri.RegisterOffset = GENX(L3CNTLREG_num); lri.DataDWord = l3bits; }
   blorp_emit(b, GENX(STATE_BASE_ADDRESS), sba) {
      sba.GeneralStateBaseAddressModifyEnable = true;
      sba.SurfaceStateBaseAddress = blorp_get_surface_base_address(b); sba.SurfaceStateBaseAddressModifyEnable = true;
      sba.DynamicStateBaseAddress = (struct blorp_address){.offset = canonical(p.dynamic.address)}; sba.DynamicStateBaseAddressModifyEnable = true;
      sba.InstructionBaseAddress = (struct blorp_address){.offset = canonical(p.code.address)}; sba.InstructionBaseAddressModifyEnable = true;
      sba.SurfaceStateMOCS = sba.DynamicStateMOCS = sba.InstructionMOCS = sba.StatelessDataPortAccessMOCS = 0x78;
      sba.GeneralStateMOCS = sba.IndirectObjectMOCS = 0x78;
      sba.DynamicStateBufferSize = sba.InstructionBufferSize = (1u << 30) / 4096;
      sba.DynamicStateBufferSizeModifyEnable = sba.InstructionBuffersizeModifyEnable = true;
   }
   flush_caches(b);
   struct image color, copied, depth, triangle, triangle_depth;
   image_init(&p, &color, ISL_FORMAT_B8G8R8A8_UNORM, width, height, tiling, 0x1c0000000, false);
   image_init(&p, &copied, ISL_FORMAT_R8G8B8A8_UNORM, width, height, ISL_TILING_LINEAR, 0x1c0400000, false);
   image_init(&p, &depth, ISL_FORMAT_R16_UNORM, width, height, ISL_TILING_Y0, 0x1c0800000, true);
   image_init(&p, &triangle, ISL_FORMAT_R8G8B8A8_UNORM, width, height, ISL_TILING_LINEAR, 0x1c0c00000, false);
   image_init(&p, &triangle_depth, ISL_FORMAT_R16_UNORM, width, height, ISL_TILING_Y0, 0x1c1000000, true);
   unsigned cx0 = width / 5, cy0 = height / 5, cx1 = width * 4 / 5, cy1 = height * 4 / 5;
   unsigned dx0 = width / 4, dy0 = height / 4, dx1 = width * 3 / 4, dy1 = height * 3 / 4;
   struct blorp_surf cs = blorp_image(&color), dst = blorp_image(&copied), ds = blorp_image(&depth);
   blorp_clear(b, &cs, color.surf.format, ISL_SWIZZLE_IDENTITY, 0, 0, 1, 0, 0, width, height,
               (union isl_color_value){.f32 = {1, 0, 0, 1}}, 0);
   flush_caches(b);
   blorp_clear(b, &cs, color.surf.format, ISL_SWIZZLE_IDENTITY, 0, 0, 1, cx0, cy0, cx1, cy1,
               (union isl_color_value){.f32 = {0, 1, 0, 1}}, 0);
   flush_caches(b);
   blorp_blit(b, &cs, 0, 0, color.surf.format, ISL_SWIZZLE_IDENTITY,
              &dst, 0, 0, copied.surf.format, ISL_SWIZZLE_IDENTITY,
              0, 0, width, height, 0, 0, width, height, BLORP_FILTER_NEAREST, false, false);
   flush_caches(b);
   blorp_clear_depth_stencil(b, &ds, NULL, 0, 0, 1, 0, 0, width, height, true, 1.0f, 0, 0);
   flush_caches(b);
   blorp_clear_depth_stencil(b, &ds, NULL, 0, 0, 1, dx0, dy0, dx1, dy1, true, 0.0f, 0, 0);
   flush_caches(b);
   struct blorp_surf ts = blorp_image(&triangle), td = blorp_image(&triangle_depth);
   blorp_clear(b, &ts, triangle.surf.format, ISL_SWIZZLE_IDENTITY, 0, 0, 1, 0, 0, width, height,
               (union isl_color_value){.f32 = {0, 0, 1, 1}}, 0);
   flush_caches(b);
   blorp_clear_depth_stencil(b, &td, NULL, 0, 0, 1, 0, 0, width, height, true, 1.0f, 0, 0);
   flush_caches(b);
   p.triangle = true; p.triangle_depth = &triangle_depth; p.triangle_z = 0.25f;
   blorp_blit(b, &cs, 0, 0, color.surf.format, ISL_SWIZZLE_IDENTITY,
              &ts, 0, 0, triangle.surf.format, ISL_SWIZZLE_IDENTITY,
              0, 0, width, height, 0, 0, width, height, BLORP_FILTER_NEAREST, false, false);
   flush_caches(b);
   /* Same geometry farther away, with changed red/blue channels. It must
    * leave both the near triangle's color and its depth untouched. */
   p.triangle_z = 0.75f;
   blorp_blit(b, &cs, 0, 0, color.surf.format, ISL_SWIZZLE(BLUE, GREEN, RED, ALPHA),
              &ts, 0, 0, triangle.surf.format, ISL_SWIZZLE_IDENTITY,
              0, 0, width, height, 0, 0, width, height, BLORP_FILTER_NEAREST, false, false);
   p.triangle = false;
   flush_caches(b);
   blorp_emit(b, GENX(PIPE_CONTROL), pc) {
      pc.CommandStreamerStallEnable = pc.RenderTargetCacheFlushEnable = pc.DepthCacheFlushEnable = pc.DCFlushEnable = true;
      pc.PostSyncOperation = WriteImmediateData; pc.Address = blorp_get_workaround_address(b); pc.ImmediateData = 1;
   }
   blorp_emit(b, GENX(MI_BATCH_BUFFER_END), end);
   if (p.commands.next & 7) { uint32_t ignored; *(uint32_t *)pool_alloc(&p.commands, 4, 4, &ignored) = 0; }
   struct GENX(MI_BATCH_BUFFER_START) jump = {GENX(MI_BATCH_BUFFER_START_header),
      .AddressSpaceIndicator = ASI_PPGTT, .BatchBufferStartAddress = {.buffer = p.commands.bo}};
   GENX(MI_BATCH_BUFFER_START_pack)(b, hasvk_wddm_bo_map(p.entry), &jump);
   dump(argv[1], L"batch.bin", hasvk_wddm_bo_map(p.commands.bo), p.commands.next);
   require(old_dynamic == pool_map(&p.dynamic, 0, 4) && old_code == pool_map(&p.code, 0, 4) &&
      *(uint32_t *)old_dynamic == 0x13579bdf && *(uint32_t *)old_code == 0x13579bdf, "CPU state pool pointers changed on growth");
   dump(argv[1], L"kernels.bin", pool_map(&p.code, 0, p.code.next), p.code.next);
   require(!hasvk_wddm_submit_wait(p.device, p.entry, 0, 16), "Submit BLORP graphics");
   require(*(uint64_t *)hasvk_wddm_bo_map(p.scratch) == 1, "Missing GPU post-sync write");
   uint32_t *a = malloc(width * height * 4), *c = malloc(width * height * 4);
   uint16_t *d = malloc(width * height * 2);
   uint32_t *t = malloc(width * height * 4); uint16_t *z = malloc(width * height * 2);
   require(a && c && d && t && z, "Readback allocation failed");
   read_image(&color, a, width, height, 4); read_image(&copied, c, width, height, 4); read_image(&depth, d, width, height, 2);
   read_image(&triangle, t, width, height, 4); read_image(&triangle_depth, z, width, height, 2);
   dump(argv[1], L"color-bgra.bin", a, width * height * 4); dump(argv[1], L"sampled-rgba.bin", c, width * height * 4); dump(argv[1], L"depth-d16.bin", d, width * height * 2);
   dump(argv[1], L"triangle-rgba.bin", t, width * height * 4); dump(argv[1], L"triangle-d16.bin", z, width * height * 2);
   for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
      bool green = x >= cx0 && x < cx1 && y >= cy0 && y < cy1;
      if (a[y * width + x] != (green ? 0xff00ff00 : 0xffff0000) ||
          c[y * width + x] != (green ? 0xff00ff00 : 0xff0000ff) ||
          d[y * width + x] != (x >= dx0 && x < dx1 && y >= dy0 && y < dy1 ? 0 : 65535)) {
         fprintf(stderr, "Pixel mismatch (%u,%u): %08x %08x %04x\n", x, y, a[y*width+x], c[y*width+x], d[y*width+x]); return 1;
      }
      bool inside = (2 * y + 1) * width > (2 * x + 1) * height;
      uint32_t triangle_color = inside ? (green ? 0xff00ff00 : 0xff0000ff) : 0xffff0000;
      uint16_t triangle_z = inside ? 16384 : 65535;
      if (t[y * width + x] != triangle_color || z[y * width + x] != triangle_z) {
         fprintf(stderr, "Triangle mismatch (%u,%u): color %08x expected %08x, depth %u expected %u\n",
            x, y, t[y * width + x], triangle_color, z[y * width + x], triangle_z); return 1;
      }
   }
   free(a); free(c); free(d); free(t); free(z);
   require(!hasvk_wddm_verify_no_umd(), "Verify runtime modules");
   require(hasvk_wddm_release(p.device, p.reservation) == -1, "Live reservation release must fail");
   require(hasvk_wddm_device_destroy(p.device) == -1, "Live device destruction must fail");
   blorp_batch_finish(b); blorp_finish(&p.blorp);
   struct hasvk_wddm_bo *bos[] = {color.bo, copied.bo, depth.bo, triangle.bo, triangle_depth.bo, p.entry, p.commands.bo, p.scratch};
   for (unsigned i = 0; i < ARRAY_SIZE(bos); ++i) require(!hasvk_wddm_bo_destroy(bos[i]), "Destroy BO");
   require(hasvk_wddm_release(p.device, p.reservation) == -1, "Reservation with live pools must remain reserved");
   require(!hasvk_wddm_readonly_pool_destroy(p.dynamic.shadow), "Destroy dynamic pool");
   require(!hasvk_wddm_readonly_pool_destroy(p.surfaces.shadow), "Destroy surface pool");
   require(!hasvk_wddm_readonly_pool_destroy(p.code.shadow), "Destroy instruction pool");
   require(!hasvk_wddm_release(p.device, p.reservation), "Release layout");
   require(!hasvk_wddm_device_destroy(p.device), "Destroy WDDM transport");
   char report[2048];
   int bytes = snprintf(report, sizeof(report),
      "{\"passed\":true,\"hardware\":\"8086:162b\",\"driverVersion\":\"20.19.15.5171\","
      "\"width\":%u,\"height\":%u,\"pixelsVerified\":%u,\"shadersCompiledAndIsaValidated\":%u,"
      "\"batchBytes\":%u,\"guardBytesVerified\":20480,\"colorBgraTiling\":%u,"
      "\"sampledRgbaLinear\":true,\"depthD16Ytiled\":true,\"auxSurfacesUsed\":false,"
      "\"capturedKernelStateOrAllocationMetadataUsed\":false,\"cleanupValidated\":true,\"forbiddenModules\":[],"
      "\"liveResourceDestructionRejected\":true,\"stableCpuReadonlyPoolsValidated\":true,"
      "\"texturedTriangleAndD16DepthTestValidated\":true,\"spirvVertexShaderValidated\":false,"
      "\"overlappingAllocationAndOutOfBoundsPoolMapRejected\":true,\"vulkanIcdImplemented\":false}",
      width, height, width * height * 5, p.shaders, p.commands.next, tiling);
   dump(argv[1], L"report.json", report, bytes);
   puts(report);
   ralloc_free(p.memory); glsl_type_singleton_decref();
   return 0;
}
