/* Windows lifetime and submission backing for HasVK compute command buffers.
 * Packet emission, descriptor binding and push packing use upstream HasVK. */
#include "anv_wddm.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

enum { command_bytes = 65536, marker_offset = command_bytes - 64 };

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
   memset((char *)cmd->wddm->bo->map + command_bytes - 256, 0x6d, 256);
   anv_batch_set_storage(&cmd->batch, (struct anv_address){cmd->wddm->bo, 0}, cmd->wddm->bo->map, command_bytes - 256);
}
static VkResult create_command(struct vk_command_pool *pool, VkCommandBufferLevel level, struct vk_command_buffer **out)
{
   *out = NULL;
   if (level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return VK_ERROR_FEATURE_NOT_PRESENT;
   struct anv_device *device = container_of(pool->base.device, struct anv_device, vk);
   struct anv_cmd_buffer *cmd = vk_zalloc(&pool->alloc, sizeof(*cmd), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd) return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = vk_command_buffer_init(pool, &cmd->vk, &anv_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) { vk_free(&pool->alloc, cmd); return result; }
   cmd->device = device; cmd->queue_family = &device->physical->queue.families[pool->queue_family_index];
   cmd->wddm = vk_zalloc(&pool->alloc, sizeof(*cmd->wddm), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   cmd->batch.relocs = vk_zalloc(&pool->alloc, sizeof(*cmd->batch.relocs), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!cmd->wddm || !cmd->batch.relocs) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
   result = anv_device_alloc_bo(device, "command", command_bytes, ANV_BO_ALLOC_MAPPED | ANV_BO_ALLOC_CAPTURE, 0, &cmd->wddm->bo);
   if (result != VK_SUCCESS) goto fail;
   if (!u_vector_init(&cmd->bt_block_states, 8, sizeof(struct anv_state))) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail_bo; }
   cmd->batch.alloc = &pool->alloc;
   command_resources_init(cmd);
   *out = &cmd->vk;
   return VK_SUCCESS;
fail_bo:
   anv_device_release_bo(device, cmd->wddm->bo);
fail:
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
   anv_device_release_bo(cmd->device, cmd->wddm->bo);
   vk_free(&base->pool->alloc, cmd->batch.relocs); vk_free(&base->pool->alloc, cmd->wddm);
   vk_command_buffer_finish(base); vk_free(&base->pool->alloc, cmd);
}
const struct vk_command_buffer_ops anv_cmd_buffer_ops = {
   .create = create_command, .reset = anv_cmd_buffer_reset, .destroy = destroy_command,
};
VKAPI_ATTR VkResult VKAPI_CALL anv_BeginCommandBuffer(VkCommandBuffer handle, const VkCommandBufferBeginInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   vk_command_buffer_begin(&cmd->vk, info);
   VkResult result = anv_cmd_buffer_new_binding_table_block(cmd);
   if (result != VK_SUCCESS) return result;
   /* Context state is restored by WDDM, but each command buffer establishes
    * its pipeline and cache state independently before selecting GPGPU. */
   genX(flush_pipeline_select_3d)(cmd);
   genX(cmd_buffer_emit_state_base_address)(cmd);
   cmd->usage_flags = info->flags & ~VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
#ifdef HASVK_WDDM_GRAPHICS
   cmd->state.push_constants_dirty |= VK_SHADER_STAGE_ALL_GRAPHICS;
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_VF_CACHE_INVALIDATE_BIT, "new command buffer");
#endif
   return cmd->batch.status;
}
VKAPI_ATTR VkResult VKAPI_CALL anv_EndCommandBuffer(VkCommandBuffer handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
#ifdef HASVK_WDDM_GRAPHICS
   genX(cmd_buffer_enable_pma_fix)(cmd, false);
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_CACHE_FLUSH_BIT,
      "graphics completion");
#endif
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_DATA_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT | ANV_PIPE_END_OF_PIPE_SYNC_BIT,
      "command buffer completion");
   genX(cmd_buffer_apply_pipe_flushes)(cmd);
   anv_batch_emit(&cmd->batch, GENX(PIPE_CONTROL), flush) {
      flush.DCFlushEnable = true; flush.CommandStreamerStallEnable = true;
      flush.PostSyncOperation = WriteImmediateData;
      flush.Address = (struct anv_address){cmd->wddm->bo, marker_offset}; flush.ImmediateData = 1;
   }
   anv_batch_emit(&cmd->batch, GENX(MI_BATCH_BUFFER_END), end);
   if (cmd->batch.status != VK_SUCCESS) vk_command_buffer_set_error(&cmd->vk, cmd->batch.status);
   return vk_command_buffer_end(&cmd->vk);
}
VKAPI_ATTR void VKAPI_CALL anv_CmdPipelineBarrier2(VkCommandBuffer handle, const VkDependencyInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
#ifdef HASVK_WDDM_GRAPHICS
   anv_add_pending_pipe_bits(cmd, ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_DEPTH_CACHE_FLUSH_BIT,
      "graphics memory barrier");
#endif
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
}
int anv_wddm_execute_command(struct anv_cmd_buffer *cmd)
{
   struct anv_wddm_device_state *w = cmd->device->wddm;
   if (cmd->vk.state != MESA_VK_COMMAND_BUFFER_STATE_PENDING || cmd->batch.status != VK_SUCCESS) {
      fprintf(stderr, "WDDM command rejected: runtime state=%u, batch status=%d\n", cmd->vk.state, cmd->batch.status);
      return -1;
   }
   uint64_t *marker = (uint64_t *)((char *)cmd->wddm->bo->map + marker_offset); *marker = 0;
   struct anv_batch entry = {.start = hasvk_wddm_bo_map(w->entry), .next = hasvk_wddm_bo_map(w->entry),
      .end = (char *)hasvk_wddm_bo_map(w->entry) + 8192, .alloc = &cmd->device->vk.alloc, .relocs = cmd->batch.relocs};
   anv_batch_emit(&entry, GENX(MI_BATCH_BUFFER_START), jump) {
      jump.AddressSpaceIndicator = ASI_PPGTT; jump.BatchBufferStartAddress = (struct anv_address){cmd->wddm->bo, 0};
   }
   anv_batch_emit(&entry, GENX(MI_NOOP), noop);
   if (entry.status != VK_SUCCESS || hasvk_wddm_submit_wait(w->transport, w->entry, 0, 16) || *marker != 1) {
      fprintf(stderr, "WDDM command completion: marker=%llu; %s\n", (unsigned long long)*marker, hasvk_wddm_error());
      return -1;
   }
   const uint8_t *tail = (const uint8_t *)cmd->wddm->bo->map + command_bytes - 256;
   for (unsigned i = 0; i < 256; ++i) if ((i < 192 || i >= 200) && tail[i] != 0x6d) return -1;
   ++w->command_batches_completed;
   return 0;
}
PUBLIC int hasvk_wddm_icd_query_command_buffer(VkCommandBuffer handle, const void **bytes, uint32_t *length)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd || !bytes || !length) return -1;
   *bytes = cmd->wddm->bo->map; *length = (char *)cmd->batch.next - (char *)cmd->batch.start;
   return 0;
}
