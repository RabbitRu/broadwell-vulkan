/* Windows lifetime and submission backing for HasVK command buffers.
 * Packet emission, descriptor binding and push packing use upstream HasVK.
 *
 * A command buffer is a chain of BOs (8 KiB, then 64 KiB) linked by MI_BATCH_BUFFER_START.
 * Every BO ends in a 256-byte guard region; the first one also holds the GPU
 * completion marker written by the last PIPE_CONTROL of a primary buffer.
 * Secondary command buffers are copied into the primary at
 * vkCmdExecuteCommands (HasVK's "emit" execution mode). */
#include "anv_wddm.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

int64_t anv_wddm_stat_command_buffers, anv_wddm_stat_command_bos;

/* The first BO of a command buffer is small: most command buffers of a game
 * hold a few KiB (Dota 2: ~4400 of them, 1-10 KiB each, thousands recorded at
 * once), and 64 KiB each ran the driver's own memory (ANV_WDDM_INTERNAL_BYTES)
 * out. A buffer that outgrows it continues in 64 KiB BOs. The guard region
 * and the completion marker sit at the end of whichever BO it is. */
enum { first_command_bytes = 8192, command_bytes = 65536, guard_bytes = 256, chain_bytes = 16, marker_from_end = 64 };
static inline uint32_t marker_offset(const struct anv_bo *bo) { return (uint32_t)bo->size - marker_from_end; }

/* 16 bytes: MI_BATCH_BUFFER_START to next (jump) or MI_BATCH_BUFFER_END, padded with MI_NOOP. */
static void write_tail(uint32_t *tail, struct anv_address next, bool jump)
{
   memset(tail, 0, 16);
   if (jump) {
      struct GENX(MI_BATCH_BUFFER_START) start = { GENX(MI_BATCH_BUFFER_START_header),
         .AddressSpaceIndicator = ASI_PPGTT, .BatchBufferStartAddress = next };
      GENX(MI_BATCH_BUFFER_START_pack)(NULL, tail, &start);
   } else {
      struct GENX(MI_BATCH_BUFFER_END) end = { GENX(MI_BATCH_BUFFER_END_header) };
      GENX(MI_BATCH_BUFFER_END_pack)(NULL, tail, &end);
   }
}
static void command_use_bo(struct anv_cmd_buffer *cmd, struct anv_bo *bo)
{
   memset((char *)bo->map + bo->size - guard_bytes, 0x6d, guard_bytes);
   anv_batch_set_storage(&cmd->batch, (struct anv_address){bo, 0}, bo->map, bo->size - guard_bytes - chain_bytes);
}
/* anv_batch_emit_dwords found no room: close this BO with a jump to a new one. */
static VkResult command_extend(struct anv_batch *batch, void *data)
{
   struct anv_cmd_buffer *cmd = data;
   struct anv_wddm_cmd_state *s = cmd->wddm;
   /* No storage at all: recording was not begun, or its first BO could not be allocated. */
   if (!s->chunk_count) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (s->chunk_count == s->chunk_capacity) {
      const uint32_t capacity = MAX2(8, s->chunk_capacity * 2);
      struct anv_wddm_cmd_chunk *chunks = vk_realloc(&cmd->vk.pool->alloc, s->chunks, capacity * sizeof(*chunks), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!chunks) return VK_ERROR_OUT_OF_HOST_MEMORY;
      s->chunks = chunks; s->chunk_capacity = capacity;
   }
   struct anv_bo *bo = NULL;
   struct anv_wddm_device_state *w = cmd->device->wddm;
   mtx_lock(&w->alloc_mutex);
   if (w->command_spare_count) bo = w->command_spares[--w->command_spare_count];   /* from an earlier recording */
   mtx_unlock(&w->alloc_mutex);
   if (!bo) {
      VkResult result = anv_device_alloc_bo(cmd->device, "command", command_bytes, ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_CAPTURE, 0, &bo);
      if (result != VK_SUCCESS) return result;
      p_atomic_inc(&anv_wddm_stat_command_bos);
   }
   s->chunks[s->chunk_count - 1].length = (uint32_t)((char *)batch->next - (char *)batch->start);
   /* chain_bytes past batch->end are reserved for this jump. */
   uint32_t *jump = batch->next;
   struct GENX(MI_BATCH_BUFFER_START) start = { GENX(MI_BATCH_BUFFER_START_header),
      .AddressSpaceIndicator = ASI_PPGTT, .BatchBufferStartAddress = (struct anv_address){bo, 0} };
   batch->end = (char *)batch->end + chain_bytes;   /* the packer checks that the location is inside the batch */
   GENX(MI_BATCH_BUFFER_START_pack)(batch, jump, &start);
   jump[GENX(MI_BATCH_BUFFER_START_length)] = 0; /* MI_NOOP */
   s->chunks[s->chunk_count++] = (struct anv_wddm_cmd_chunk){bo, 0};
   command_use_bo(cmd, bo);
   return batch->status;
}
static void command_resources_finish(struct anv_cmd_buffer *cmd)
{
#ifdef HASVK_WDDM_GRAPHICS
   anv_wddm_cmd_state_finish(cmd);
#endif
   struct anv_state *block;
   while ((block = u_vector_remove(&cmd->bt_block_states)))
      anv_state_pool_free(anv_binding_table_pool(cmd->device), *block);
   anv_state_stream_finish(&cmd->surface_state_stream);
   anv_state_stream_finish(&cmd->dynamic_state_stream);
   anv_state_stream_finish(&cmd->general_state_stream);
   anv_reloc_list_finish(&cmd->surface_relocs, &cmd->vk.pool->alloc);
   anv_reloc_list_finish(cmd->batch.relocs, &cmd->vk.pool->alloc);
   /* Extra chunks go to the device's spares for the next recording of any
    * command buffer; beyond ANV_WDDM_COMMAND_SPARES idle ones they are released. */
   struct anv_wddm_cmd_state *s = cmd->wddm;
   struct anv_wddm_device_state *w = cmd->device->wddm;
   for (uint32_t i = 1; i < s->chunk_count; ++i) {
      mtx_lock(&w->alloc_mutex);
      const bool kept = w->command_spare_count < ANV_WDDM_COMMAND_SPARES;
      if (kept) w->command_spares[w->command_spare_count++] = s->chunks[i].bo;
      mtx_unlock(&w->alloc_mutex);
      if (!kept) { anv_device_release_bo(cmd->device, s->chunks[i].bo); p_atomic_dec(&anv_wddm_stat_command_bos); }
   }
   for (uint32_t i = 0; i < cmd->wddm->wait_count; ++i) vk_free(&cmd->vk.pool->alloc, cmd->wddm->waits[i].events);
   cmd->wddm->wait_count = 0;
   cmd->wddm->chunk_count = cmd->wddm->bo ? 1 : 0; cmd->wddm->chunks[0].length = 0;
   cmd->wddm->tail = NULL;
}
/* Storage for a recording: the first BO is allocated by the first recording
 * and kept until the command buffer is destroyed. */
static VkResult command_storage_begin(struct anv_cmd_buffer *cmd)
{
   struct anv_wddm_cmd_state *s = cmd->wddm;
   if (!s->bo) {
      VkResult result = anv_device_alloc_bo(cmd->device, "command", first_command_bytes, ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_CAPTURE, 0, &s->bo);
      if (result != VK_SUCCESS) { s->bo = NULL; return result; }
      p_atomic_inc(&anv_wddm_stat_command_bos);
      s->chunks[0] = (struct anv_wddm_cmd_chunk){s->bo, 0}; s->chunk_count = 1;
   }
   command_use_bo(cmd, s->bo);
   return VK_SUCCESS;
}
static void command_resources_init(struct anv_cmd_buffer *cmd)
{
   anv_state_stream_init(&cmd->surface_state_stream, &cmd->device->surface_state_pool, 4096);
   anv_state_stream_init(&cmd->dynamic_state_stream, &cmd->device->dynamic_state_pool, 16384);
   anv_state_stream_init(&cmd->general_state_stream, &cmd->device->general_state_pool, 16384);
   anv_reloc_list_init(&cmd->surface_relocs, &cmd->vk.pool->alloc);
   anv_reloc_list_init(cmd->batch.relocs, &cmd->vk.pool->alloc);
   memset(&cmd->state, 0, sizeof(cmd->state));
   cmd->state.current_pipeline = UINT32_MAX;
   cmd->state.gfx.restart_index = UINT32_MAX;
   cmd->vk.dynamic_graphics_state.ms.sample_locations = &cmd->state.gfx.sample_locations;
   cmd->batch.status = VK_SUCCESS;
   cmd->batch.extend_cb = command_extend; cmd->batch.user_data = cmd;
   /* Allocated or reset but not being recorded: no storage and no binding
    * table block. Each took 64 KiB of the driver's own memory per command
    * buffer, and 10000 idle command buffers (CTS
    * api.command_buffers.allocate_many_*) ran past ANV_WDDM_INTERNAL_BYTES. */
   anv_batch_set_storage(&cmd->batch, ANV_NULL_ADDRESS, NULL, 0);
   if (!cmd->wddm->recording) return;
   VkResult result = command_storage_begin(cmd);
   if (result != VK_SUCCESS) { anv_batch_set_error(&cmd->batch, result); return; }
#ifdef HASVK_WDDM_GRAPHICS
   /* As anv_cmd_buffer_reset_batch_bo_chain: every recording starts with one binding table block. */
   result = anv_cmd_buffer_new_binding_table_block(cmd);
   if (result != VK_SUCCESS) anv_batch_set_error(&cmd->batch, result);
#endif
}
static VkResult create_command(struct vk_command_pool *pool, VkCommandBufferLevel level, struct vk_command_buffer **out)
{
   *out = NULL;
#ifndef HASVK_WDDM_GRAPHICS
   if (level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return VK_ERROR_FEATURE_NOT_PRESENT;
#endif
   struct anv_device *device = container_of(pool->base.device, struct anv_device, vk);
   struct anv_cmd_buffer *cmd = vk_zalloc(&pool->alloc, sizeof(*cmd), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = vk_command_buffer_init(pool, &cmd->vk, &anv_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) { vk_free(&pool->alloc, cmd); return result; }
   cmd->device = device; cmd->queue_family = &device->physical->queue.families[pool->queue_family_index];
   cmd->wddm = vk_zalloc(&pool->alloc, sizeof(*cmd->wddm), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   cmd->batch.relocs = vk_zalloc(&pool->alloc, sizeof(*cmd->batch.relocs), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd->wddm || !cmd->batch.relocs) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
   cmd->wddm->chunks = vk_zalloc(&pool->alloc, 8 * sizeof(*cmd->wddm->chunks), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd->wddm->chunks) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
   cmd->wddm->chunk_capacity = 8;
   /* No BO yet: command_storage_begin allocates it for the first recording. */
   if (!u_vector_init(&cmd->bt_block_states, 8, sizeof(struct anv_state))) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
   cmd->batch.alloc = &pool->alloc;
   command_resources_init(cmd);
   p_atomic_inc(&anv_wddm_stat_command_buffers);
   *out = &cmd->vk;
   return VK_SUCCESS;
fail:
   if (cmd->wddm) vk_free(&pool->alloc, cmd->wddm->chunks);
   vk_free(&pool->alloc, cmd->wddm); vk_free(&pool->alloc, cmd->batch.relocs);
   vk_command_buffer_finish(&cmd->vk); vk_free(&pool->alloc, cmd);
   return result;
}
void anv_cmd_buffer_reset(struct vk_command_buffer *base, VkCommandBufferResetFlags flags)
{
   (void)flags;
   struct anv_cmd_buffer *cmd = container_of(base, struct anv_cmd_buffer, vk);
   vk_command_buffer_reset(base);
   command_resources_finish(cmd); command_resources_init(cmd);
}
static void destroy_command(struct vk_command_buffer *base)
{
   struct anv_cmd_buffer *cmd = container_of(base, struct anv_cmd_buffer, vk);
   command_resources_finish(cmd); u_vector_finish(&cmd->bt_block_states);
   if (cmd->wddm->bo) { anv_device_release_bo(cmd->device, cmd->wddm->bo); p_atomic_dec(&anv_wddm_stat_command_bos); }
   p_atomic_dec(&anv_wddm_stat_command_buffers);
   vk_free(&base->pool->alloc, cmd->wddm->chunks); vk_free(&base->pool->alloc, cmd->wddm->waits);
   vk_free(&base->pool->alloc, cmd->batch.relocs); vk_free(&base->pool->alloc, cmd->wddm);
   vk_command_buffer_finish(base); vk_free(&base->pool->alloc, cmd);
}
void anv_wddm_command_spares_finish(struct anv_device *device)
{
   struct anv_wddm_device_state *w = device->wddm;
   while (w->command_spare_count) {
      anv_device_release_bo(device, w->command_spares[--w->command_spare_count]);
      p_atomic_dec(&anv_wddm_stat_command_bos);
   }
}
const struct vk_command_buffer_ops anv_cmd_buffer_ops = {
   .create = create_command, .reset = anv_cmd_buffer_reset, .destroy = destroy_command,
};
/* HASVK_WDDM_DUMP_REGISTERS=1 prints these context registers after each submission. */
static const uint32_t anv_wddm_dumped_registers[] = {0x7004 /* CACHE_MODE_1 */, 0x7000 /* CACHE_MODE_0 */, 0x7008 /* GT_MODE */,
   0x7300 /* HDC_CHICKEN0 */, 0x7010 /* HIZ_CHICKEN */, 0x7014 /* COMMON_SLICE_CHICKEN2 */, 0xe184 /* HALF_SLICE_CHICKEN3 */,
   0xe4f0 /* ROW_CHICKEN */, 0xe4f4 /* ROW_CHICKEN2 */, 0x20c0 /* INSTPM */, 0xe180 /* HALF_SLICE_CHICKEN2 */, 0x7018 /* SLICE_COMMON_ECO_CHICKEN1 */,
   0x7034 /* GEN8_L3CNTLREG */, 0xb100 /* GEN8_L3SQCREG1 */, 0xb118 /* GEN8_L3SQCREG4 */, 0xb010 /* L3 LRA */, 0x2358 /* TIMESTAMP, control */};
static bool anv_wddm_dump_registers(void)
{
   static int enabled = -1;
   if (enabled < 0) enabled = getenv("HASVK_WDDM_DUMP_REGISTERS") != NULL;
   return enabled;
}
/* End of recording. A primary buffer flushes its caches and writes the
 * completion marker that the submit path checks; a secondary just ends. */
void anv_cmd_buffer_end_batch_buffer(struct anv_cmd_buffer *cmd)
{
   if (!cmd->wddm->chunk_count) {   /* the recording never got its storage */
      if (cmd->batch.status == VK_SUCCESS) anv_batch_set_error(&cmd->batch, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }
   if (cmd->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
#ifdef HASVK_WDDM_GRAPHICS
      anv_add_pending_pipe_bits(cmd, ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_CACHE_FLUSH_BIT,
         "graphics completion");
#endif
      anv_add_pending_pipe_bits(cmd, ANV_PIPE_DATA_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT | ANV_PIPE_END_OF_PIPE_SYNC_BIT,
         "command buffer completion");
      genX(cmd_buffer_apply_pipe_flushes)(cmd);
      if (anv_wddm_dump_registers()) {   /* diagnostics: context register values into the guard region */
         for (unsigned i = 0; i < ARRAY_SIZE(anv_wddm_dumped_registers); ++i)
            anv_batch_emit(&cmd->batch, GENX(MI_STORE_REGISTER_MEM), store) {
               store.RegisterAddress = anv_wddm_dumped_registers[i];
               store.MemoryAddress = (struct anv_address){cmd->wddm->bo, cmd->wddm->bo->size - guard_bytes + 4 * i};
            }
      }
      anv_batch_emit(&cmd->batch, GENX(PIPE_CONTROL), flush) {
         flush.DCFlushEnable = true; flush.CommandStreamerStallEnable = true;
         flush.PostSyncOperation = WriteImmediateData;
         flush.Address = (struct anv_address){cmd->wddm->bo, marker_offset(cmd->wddm->bo)}; flush.ImmediateData = 1;
      }
      uint32_t *tail = anv_batch_emit_dwords(&cmd->batch, 4);
      if (tail) { cmd->wddm->tail = tail; write_tail(tail, ANV_NULL_ADDRESS, false); }
   }
   struct anv_wddm_cmd_state *s = cmd->wddm;
   s->chunks[s->chunk_count - 1].length = (uint32_t)((char *)cmd->batch.next - (char *)cmd->batch.start);
}
#ifdef HASVK_WDDM_GRAPHICS
static void command_add_wait(struct anv_cmd_buffer *cmd, struct anv_address resume, uint32_t count, struct anv_event *const *events)
{
   struct anv_wddm_cmd_state *s = cmd->wddm;
   if (s->wait_count == s->wait_capacity) {
      const uint32_t capacity = MAX2(4, s->wait_capacity * 2);
      struct anv_wddm_wait *waits = vk_realloc(&cmd->vk.pool->alloc, s->waits, capacity * sizeof(*waits), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!waits) { anv_batch_set_error(&cmd->batch, VK_ERROR_OUT_OF_HOST_MEMORY); return; }
      s->waits = waits; s->wait_capacity = capacity;
   }
   struct anv_event **copy = vk_alloc(&cmd->vk.pool->alloc, count * sizeof(*copy), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!copy) { anv_batch_set_error(&cmd->batch, VK_ERROR_OUT_OF_HOST_MEMORY); return; }
   memcpy(copy, events, count * sizeof(*copy));
   s->waits[s->wait_count++] = (struct anv_wddm_wait){resume, count, copy};
}
/* vkCmdWaitEvents. Inside a render pass the events were set earlier in
 * submission order (host-set-after-submit is not allowed there,
 * VUID-vkCmdWaitEvents-pEvents-01163), and this queue executes strictly in
 * order, so only the memory barrier is needed. Elsewhere the batch ends here,
 * after all preceding work, and resumes once the CPU has seen every event set. */
void anv_wddm_cmd_wait_events(struct anv_cmd_buffer *cmd, uint32_t count, const VkEvent *handles)
{
   /* Inside rendering: legacy render passes also go through CmdBeginRendering,
    * which sets layer_count; CmdEndRendering resets it to 0. */
   if (!count || cmd->vk.render_pass || cmd->state.gfx.layer_count ||
       (cmd->usage_flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) return;
   struct anv_event *events[16]; struct anv_event **list = events;
   if (count > ARRAY_SIZE(events)) {
      list = vk_alloc(&cmd->vk.pool->alloc, count * sizeof(*list), 8, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (!list) { anv_batch_set_error(&cmd->batch, VK_ERROR_OUT_OF_HOST_MEMORY); return; }
   }
   for (uint32_t i = 0; i < count; ++i) list[i] = anv_event_from_handle(handles[i]);
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_CS_STALL_BIT | ANV_PIPE_END_OF_PIPE_SYNC_BIT | ANV_PIPE_DATA_CACHE_FLUSH_BIT |
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_CACHE_FLUSH_BIT, "split batch for vkCmdWaitEvents");
   genX(cmd_buffer_apply_pipe_flushes)(cmd);
   anv_batch_emit(&cmd->batch, GENX(MI_BATCH_BUFFER_END), end);
   anv_batch_emit(&cmd->batch, GENX(MI_NOOP), noop);
   command_add_wait(cmd, anv_batch_address(&cmd->batch, cmd->batch.next), count, list);
   if (list != events) vk_free(&cmd->vk.pool->alloc, list);
   /* The next segment is a new WDDM submission: do not rely on any GPU state
    * from the previous one (draws after a resume hung without this). Same
    * reset as upstream after vkCmdExecuteCommands, plus all derived state. */
   cmd->state.current_pipeline = UINT32_MAX;
   cmd->state.current_l3_config = NULL;
   cmd->state.current_hash_scale = 0;
   cmd->state.gfx.push_constant_stages = 0;
   cmd->state.gfx.dirty = ~(anv_cmd_dirty_mask_t)0;
   cmd->state.gfx.vb_dirty = ~0u;
   cmd->state.compute.pipeline_dirty = true;
   cmd->state.descriptors_dirty |= VK_SHADER_STAGE_ALL;
   cmd->state.push_constants_dirty |= VK_SHADER_STAGE_ALL;
   vk_dynamic_graphics_state_dirty_all(&cmd->vk.dynamic_graphics_state);
   genX(cmd_buffer_emit_state_base_address)(cmd);
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_VF_CACHE_INVALIDATE_BIT | ANV_PIPE_STATE_CACHE_INVALIDATE_BIT |
      ANV_PIPE_CONSTANT_CACHE_INVALIDATE_BIT | ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT | ANV_PIPE_INSTRUCTION_CACHE_INVALIDATE_BIT,
      "resume after vkCmdWaitEvents");
}
/* Context workaround the i915 kernel driver applies to every Broadwell context:
 * Wa4x4STCOptimizationDisable:bdw (CACHE_MODE_1 bit 6, masked register).
 * Read back as set in our context; kept so it does not depend on whoever
 * created the context. It did not affect the mip-layout corruption, which was
 * HALIGN (see isl_gfx8.c in apply-compiler-port.py). */
static void anv_wddm_emit_context_workarounds(struct anv_cmd_buffer *cmd)
{
   anv_batch_emit(&cmd->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
      lri.RegisterOffset = 0x7004; /* CACHE_MODE_1 */
      lri.DataDWord = (1u << (16 + 6)) | (1u << 6);
   }
}
/* vkCmdExecuteCommands: commands only reference pinned addresses, so the
 * secondary's chunks are copied into the primary without their chain jumps. */
void anv_cmd_buffer_add_secondary(struct anv_cmd_buffer *primary, struct anv_cmd_buffer *secondary)
{
   const struct anv_wddm_cmd_state *s = secondary->wddm;
   for (uint32_t i = 0; i < s->chunk_count; ++i) {
      struct anv_batch chunk = {.start = s->chunks[i].bo->map, .next = (char *)s->chunks[i].bo->map + s->chunks[i].length,
         .relocs = secondary->batch.relocs};
      anv_batch_emit_batch(&primary->batch, &chunk);
      if (primary->batch.status != VK_SUCCESS) return;
      /* The chunk now sits contiguously right before primary->batch.next. */
      const struct anv_address copied = anv_batch_address(&primary->batch, (char *)primary->batch.next - s->chunks[i].length);
      for (uint32_t w = 0; w < s->wait_count; ++w) if (s->waits[w].resume.bo == s->chunks[i].bo)
         command_add_wait(primary, anv_address_add(copied, s->waits[w].resume.offset), s->waits[w].event_count, s->waits[w].events);
   }
   if (secondary->batch.status != VK_SUCCESS) anv_batch_set_error(&primary->batch, secondary->batch.status);
}
/* Upstream genX(BeginCommandBuffer)/(EndCommandBuffer) do the recording work;
 * these wrappers add the Mesa runtime's command-buffer state tracking. */
VKAPI_ATTR VkResult VKAPI_CALL anv_BeginCommandBuffer(VkCommandBuffer handle, const VkCommandBufferBeginInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   /* genX(BeginCommandBuffer) starts with anv_cmd_buffer_reset: with the flag
    * set that reset gives the batch its storage (command_resources_init). */
   cmd->wddm->recording = true;
   VkResult result = genX(BeginCommandBuffer)(handle, info);
   cmd->wddm->recording = false;
   anv_wddm_emit_context_workarounds(cmd);
   vk_command_buffer_begin(&cmd->vk, info);
   return result != VK_SUCCESS ? result : cmd->batch.status;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_EndCommandBuffer(VkCommandBuffer handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   genX(EndCommandBuffer)(handle);
   if (cmd->batch.status != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, cmd->batch.status);
   return vk_command_buffer_end(&cmd->vk);
}
#else
VKAPI_ATTR VkResult VKAPI_CALL anv_BeginCommandBuffer(VkCommandBuffer handle, const VkCommandBufferBeginInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   vk_command_buffer_begin(&cmd->vk, info);
   VkResult result = cmd->batch.start ? VK_SUCCESS : command_storage_begin(cmd);
   if (result != VK_SUCCESS) return result;
   result = anv_cmd_buffer_new_binding_table_block(cmd);
   if (result != VK_SUCCESS) return result;
   /* Context state is restored by WDDM, but each command buffer establishes
    * its pipeline and cache state independently before selecting GPGPU. */
   genX(flush_pipeline_select_3d)(cmd);
   genX(cmd_buffer_emit_state_base_address)(cmd);
   cmd->usage_flags = info->flags & ~VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
   return cmd->batch.status;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_EndCommandBuffer(VkCommandBuffer handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   anv_cmd_buffer_end_batch_buffer(cmd);
   if (cmd->batch.status != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, cmd->batch.status);
   return vk_command_buffer_end(&cmd->vk);
}
#endif
/* Graphics build: upstream HasVK's barrier (flush and invalidate bits from the
 * stage and access masks, image layout transitions). The compute-only build
 * keeps a conservative full flush. */
VKAPI_ATTR void VKAPI_CALL anv_CmdPipelineBarrier2(VkCommandBuffer handle, const VkDependencyInfo *info)
{
#ifdef HASVK_WDDM_GRAPHICS
   genX(CmdPipelineBarrier2)(handle, info);
#else
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
      ANV_FROM_HANDLE(anv_image, image, info->pImageMemoryBarriers[i].image);
      if (image->planes[0].aux_usage != ISL_AUX_USAGE_NONE) {
         anv_batch_set_error(&cmd->batch, VK_ERROR_FEATURE_NOT_PRESENT); return;
      }
   }
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_DATA_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT | ANV_PIPE_END_OF_PIPE_SYNC_BIT |
      ANV_PIPE_STATE_CACHE_INVALIDATE_BIT | ANV_PIPE_CONSTANT_CACHE_INVALIDATE_BIT | ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT,
      "conservative compute memory barrier");
   genX(cmd_buffer_apply_pipe_flushes)(cmd);
#endif
}
/* A free slot of the entry ring (its previous submission completed). */
static int take_slot(struct anv_wddm_device_state *w, uint32_t *slot)
{
   *slot = w->next_slot++ % ANV_WDDM_ENTRY_SLOTS;
   if (w->slot_seq[*slot] && hasvk_wddm_wait_submission(w->transport, w->slot_seq[*slot])) return -1;
   w->slot_seq[*slot] = 0;
   return 0;
}
/* One kernel submission from a slot whose jump goes to start. Waits for the
 * GPU unless seq is given (non-blocking: *seq receives its number). */
/* HASVK_WDDM_TIMELINE: TIMESTAMP (low 32 bits, 0x2358) into the entry slot at
 * offset, by MI_STORE_REGISTER_MEM (4 dwords at dw). */
static void store_timestamp(struct anv_wddm_device_state *w, uint32_t *dw, uint32_t slot, uint32_t offset)
{
   struct GENX(MI_STORE_REGISTER_MEM) store = { GENX(MI_STORE_REGISTER_MEM_header), .RegisterAddress = 0x2358,
      .MemoryAddress = {NULL, hasvk_wddm_bo_address(w->entry) + slot * 128 + offset} };
   GENX(MI_STORE_REGISTER_MEM_pack)(NULL, dw, &store);
}
static int submit_run(struct anv_wddm_device_state *w, uint32_t slot, struct anv_address start, uint64_t present_token,
                      uint64_t *seq)
{
   uint32_t *entry = (uint32_t *)hasvk_wddm_bo_map(w->entry) + slot * 32;
   if (anv_wddm_timeline_on) {   /* the GPU's start time of this kernel submission, then the jump */
      store_timestamp(w, entry, slot, ANV_WDDM_TIMELINE_START);
      write_tail(entry + 4, start, true);
   } else
      write_tail(entry, start, true);
   w->last_slot = slot;
   const uint32_t offset = slot * 128;
   const int failed = seq ? (present_token ? hasvk_wddm_submit_present_async(w->transport, w->entry, offset, 16, present_token, seq)
                                           : hasvk_wddm_submit_async(w->transport, w->entry, offset, 16, seq))
                    : present_token ? hasvk_wddm_submit_present(w->transport, w->entry, offset, 16, present_token)
                    : hasvk_wddm_submit_wait(w->transport, w->entry, offset, 16);
   if (failed) {
      /* A refused present is retried without it by the caller: report it once. */
      static unsigned refused_reports;
      if (!present_token || refused_reports++ < 2) fprintf(stderr, "WDDM command submission: %s\n", hasvk_wddm_error());
      return -1;
   }
   if (seq) w->slot_seq[slot] = *seq;
   ++w->runs;
   return 0;
}
static struct anv_cmd_buffer *command_at(struct vk_command_buffer *const *commands, uint32_t i)
{
   return container_of(commands[i], struct anv_cmd_buffer, vk);
}
static int check_completed(struct anv_cmd_buffer *cmd);
int anv_wddm_check_completed(struct anv_cmd_buffer *cmd) { return check_completed(cmd); }
void anv_wddm_wait_verified(struct anv_device *device, uint64_t seq)
{
   struct anv_wddm_device_state *w = device->wddm;
   mtx_lock(&w->mutex);
   while (w->verified_seq < seq && !vk_device_is_lost_no_report(&device->vk))
      u_cnd_monotonic_wait(&w->condition, &w->mutex);
   mtx_unlock(&w->mutex);
}
int anv_wddm_execute_commands(struct anv_device *device, struct vk_command_buffer *const *commands, uint32_t count,
                              uint32_t serial, uint64_t present_token, bool async, uint64_t *seq)
{
   struct anv_wddm_device_state *w = device->wddm;
   /* A buffer still owned by an unverified non-blocking execution must not
    * have its tail and marker rewritten yet. */
   for (uint32_t i = 0; i < count; ++i) {
      struct anv_cmd_buffer *cmd = command_at(commands, i);
      if (cmd->wddm->seq) { anv_wddm_wait_verified(device, cmd->wddm->seq); cmd->wddm->seq = 0; }
   }
   for (uint32_t i = 0; i < count; ++i) {
      const struct anv_cmd_buffer *cmd = command_at(commands, i);
      const bool runnable = cmd->vk.state == MESA_VK_COMMAND_BUFFER_STATE_PENDING ||
         (present_token && cmd->vk.state == MESA_VK_COMMAND_BUFFER_STATE_EXECUTABLE);
      if (!runnable || cmd->batch.status != VK_SUCCESS || cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !cmd->wddm->tail) {
         fprintf(stderr, "WDDM command rejected: runtime state=%u, batch status=%d\n", cmd->vk.state, cmd->batch.status);
         return -1;
      }
   }
   if (!count && !serial) return 0;
   if (count && anv_wddm_flush_state_pools(device)) return -1;
   /* The last kernel submission's slot also holds the completion write. */
   uint32_t final_slot;
   if (take_slot(w, &final_slot)) return -1;
   uint32_t *completion = (uint32_t *)hasvk_wddm_bo_map(w->entry) + final_slot * 32 + 16;
   if (anv_wddm_timeline_on) {   /* the GPU's end time (after the last buffer's CS stall), then the write */
      store_timestamp(w, completion, final_slot, ANV_WDDM_TIMELINE_END);
      completion += 4;
   }
   const uint64_t counter = intel_canonical_address(hasvk_wddm_bo_address(w->completion));
   completion[0] = 0x10000002; completion[1] = (uint32_t)counter; completion[2] = counter >> 32; completion[3] = serial;
   completion[4] = 0x05000000; completion[5] = 0;   /* MI_STORE_DATA_IMM serial, MI_BATCH_BUFFER_END */
   const struct anv_address done = {NULL, serial ? hasvk_wddm_bo_address(w->entry) + final_slot * 128 + 64 : 0};
   /* The pending kernel submission is run number w->runs + 1. A buffer is
    * chained to its successor unless the successor's final segment is already
    * part of that run (a repeated buffer has only one tail). */
   struct anv_address start = count ? (struct anv_address){command_at(commands, 0)->wddm->bo, 0} : done;
   uint32_t first = 0;   /* first command buffer not yet verified */
   for (uint32_t i = 0; i < count; ++i) {
      struct anv_cmd_buffer *cmd = command_at(commands, i);
      struct anv_wddm_cmd_state *s = cmd->wddm;
      struct anv_cmd_buffer *next = i + 1 < count ? command_at(commands, i + 1) : NULL;
      /* After its event waits this buffer's final segment starts a run of its own. */
      const bool chain = next && next != cmd && (s->wait_count || next->wddm->run != w->runs + 1);
      if (next) write_tail(s->tail, (struct anv_address){next->wddm->bo, 0}, chain);
      else write_tail(s->tail, done, serial != 0);
      *(uint64_t *)((char *)s->bo->map + marker_offset(s->bo)) = 0;
      /* Command BOs are not flushed by the transport: write back exactly this
       * command buffer's chunks (with its tail and marker) now. */
      for (uint32_t c = 0; c < s->chunk_count; ++c) {
         if (hasvk_wddm_bo_flush(s->chunks[c].bo->wddm_bo)) return -1;
         w->stats_command_bytes += s->chunks[c].length;
      }
      ++w->stats_command_buffers;
      s->run = w->runs + 1;
      for (uint32_t k = 0; k < s->wait_count; ++k) {
         uint32_t slot;
         if (take_slot(w, &slot) || submit_run(w, slot, start, 0, NULL)) return -1;
         for (; first < i; ++first) if (check_completed(command_at(commands, first))) return -1;
         const struct anv_wddm_wait *wait = &s->waits[k];
         for (uint32_t e = 0; e < wait->event_count; ++e) {
            /* An event set by a still-running earlier submission becomes
             * visible only after its cache line is re-read. */
            const uint32_t line = (uint32_t)wait->events[e]->state.offset & ~63u;
            while (*(volatile uint64_t *)wait->events[e]->state.map != VK_EVENT_SET) {
               if (vk_device_is_lost_no_report(&device->vk)) return -1;
               Sleep(0);
               hasvk_wddm_bo_flush_range(w->event_bo->wddm_bo, line, 64);
            }
         }
         start = wait->resume; s->run = w->runs + 1;
      }
      if (next && !chain) {
         uint32_t slot;
         if (take_slot(w, &slot) || submit_run(w, slot, start, 0, NULL)) return -1;
         for (; first <= i; ++first) if (check_completed(command_at(commands, first))) return -1;
         start = (struct anv_address){next->wddm->bo, 0};
      }
   }
   if (async) {
      /* Not waited for: the caller verifies [first, count) after completion. */
      if (submit_run(w, final_slot, start, present_token, seq)) {
         if (!present_token) return -1;
         /* The kernel refused the present (STATUS_GRAPHICS_PRESENT_OCCLUDED
          * while the display sleeps): run the same commands without it and
          * let WSI show the next frame through GDI. */
         if (submit_run(w, final_slot, start, 0, seq)) return -1;
         for (uint32_t i = 0; i < count; ++i)
            if (command_at(commands, i)->wddm->present_refused) p_atomic_set(command_at(commands, i)->wddm->present_refused, 1);
      }
      for (uint32_t i = first; i < count; ++i) command_at(commands, i)->wddm->seq = *seq;
      return 0;
   }
   if (submit_run(w, final_slot, start, present_token, NULL)) return -1;
   for (; first < count; ++first) if (check_completed(command_at(commands, first))) return -1;
   return 0;
}
int anv_wddm_execute_command(struct anv_cmd_buffer *cmd, uint64_t present_token)
{
   struct vk_command_buffer *one = &cmd->vk;
   return anv_wddm_execute_commands(cmd->device, &one, 1, 0, present_token, false, NULL);
}
/* After the GPU ran a command buffer to its end: completion marker and guards. */
static int check_completed(struct anv_cmd_buffer *cmd)
{
   struct anv_wddm_device_state *w = cmd->device->wddm;
   if (hasvk_wddm_bo_flush_range(cmd->wddm->bo->wddm_bo, marker_offset(cmd->wddm->bo) & ~63u, 64)) return -1;
   const uint64_t marker = *(volatile uint64_t *)((char *)cmd->wddm->bo->map + marker_offset(cmd->wddm->bo));
   if (marker != 1) {
      fprintf(stderr, "WDDM command completion: marker=%llu; %s\n", (unsigned long long)marker, hasvk_wddm_error());
      return -1;
   }
   for (uint32_t c = 0; c < cmd->wddm->chunk_count; ++c) {
      const uint8_t *tail = (const uint8_t *)cmd->wddm->chunks[c].bo->map + cmd->wddm->chunks[c].bo->size - guard_bytes;
      if (!c && anv_wddm_dump_registers()) {
         static bool printed;
         if (!printed) { printed = true; const uint32_t *values = (const uint32_t *)tail;
            for (unsigned i = 0; i < ARRAY_SIZE(anv_wddm_dumped_registers); ++i) fprintf(stderr, "WDDM context register 0x%04x = 0x%08x\n", anv_wddm_dumped_registers[i], values[i]); }
         continue;
      }
      for (unsigned i = 0; i < guard_bytes; ++i) if ((c || i < 192 || i >= 200) && tail[i] != 0x6d) return -1;
   }
   ++w->command_batches_completed;
   return 0;
}
PUBLIC int hasvk_wddm_icd_query_command_buffer(VkCommandBuffer handle, const void **bytes, uint32_t *length)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd || !bytes || !length || !cmd->wddm->bo) return -1;
   /* First chunk only; a chained buffer continues in further BOs. */
   *bytes = cmd->wddm->bo->map;
   *length = cmd->wddm->chunk_count > 1 ? cmd->wddm->chunks[0].length : (uint32_t)((char *)cmd->batch.next - (char *)cmd->batch.start);
   return 0;
}
/* Diagnostics: number of BOs in the command buffer's chain. */
PUBLIC int hasvk_wddm_icd_query_command_chunks(VkCommandBuffer handle, uint32_t *chunks)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd || !chunks) return -1;
   *chunks = cmd->wddm->chunk_count;
   return 0;
}
