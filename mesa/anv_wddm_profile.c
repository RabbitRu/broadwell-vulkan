/* HASVK_WDDM_PROFILE_PASSES=1: GPU time per render pass, dispatch and
 * transfer command, for finding where an application's frame time goes.
 *
 * The device dispatch table is wrapped before vk_device_init, so the loader
 * hands the application these entry points. Each profiled command is bracketed
 * by two bottom-of-pipe timestamps (which serialize the GPU around it, so the
 * numbers are the command's own time) in a ring of query pairs. submit()
 * calls anv_wddm_profile_report, which collects every pair the GPU has
 * written, resets it from the host and every 5 s prints the most expensive
 * buckets (render pass + extent, dispatch size, transfer kind) per frame.
 * Render passes also carry pipeline statistics (vertices, primitives, VS/FS
 * invocations), their attachments' aux usage (HiZ) and how often the bound
 * pipeline's kill_pixel changed (each change toggles the Gfx8 PMA fix).
 * The report adds the shaders of depth-only pipelines and the pipe
 * flush/stall requests by reason (anv_add_pending_pipe_bits).
 * HASVK_WDDM_PROFILE_DRAWS=1 additionally times every draw, by pipeline.
 * Diagnostic only: one profiled device per process. */
#include "anv_wddm.h"
#include "vk_render_pass.h"
#include "vk_framebuffer.h"
#include "util/u_atomic.h"
#include "util/os_time.h"
#include <stdio.h>
#include <stdlib.h>

#define PAIRS 16384u
#define BUCKETS 512u
#define STATS_FLAGS (VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | \
   VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | \
   VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT)

enum kind { KIND_PASS = 1, KIND_DISPATCH, KIND_COPY, KIND_BLIT, KIND_CLEAR, KIND_RESOLVE, KIND_FILL, KIND_DRAW };
static const char *const kind_names[] = {"", "pass", "dispatch", "copy", "blit", "clear", "resolve", "fill/update", "draw"};

struct meta {
   uint32_t kind, width, height, draws;
   uint64_t object;   /* pass: VkRenderPass; draw: anv_graphics_pipeline */
   uint32_t aux;      /* pass: isl_aux_usage of attachment i in bits 4i..4i+3 */
   uint32_t kill_switches;   /* pass: binds that changed pipeline->kill_pixel (PMA fix toggles) */
};
struct bucket {
   struct meta key;
   uint64_t count, ticks, draws, kill_switches;
   uint64_t stats[6];   /* IA vertices, IA primitives, VS, clipper in, clipper out, FS invocations */
   char text[200];   /* described when created: the render pass may be destroyed later */
};
static void describe(const struct meta *m, char *out, size_t size);

/* Graphics pipelines seen at bind time: shader statistics, and binds of the
 * ones without colour writes (depth-only: shadow maps). Looked up by address,
 * never dereferenced later (the pipeline may be gone). */
struct depth_pipeline { const void *pipeline; uint64_t binds; bool depth_only; char text[200]; };
#define DEPTH_PIPELINES 1024u

static struct {
   struct vk_device_dispatch_table orig;
   struct depth_pipeline depth_pipelines[DEPTH_PIPELINES];
   uint32_t depth_pipeline_count;
   struct anv_device *device;
   VkQueryPool pool, stats_pool;
   mtx_t mutex;   /* pool creation and reports */
   uint32_t next;
   uint32_t state[PAIRS];   /* 0 free, 1 recorded */
   struct meta meta[PAIRS];
   struct bucket buckets[BUCKETS];
   uint32_t bucket_count;
   uint64_t frames, window_start_ms, skipped;
} g;

/* anv_add_pending_pipe_bits requests, by reason (a string literal, so its
 * address identifies it). */
bool anv_wddm_profile_pipe_bits_enabled;
#define PIPE_REASONS 128u
static struct { const char *reason; uint32_t stall_bits; uint64_t count; } pipe_reasons[PIPE_REASONS];
static uint32_t pipe_reason_count;
static mtx_t pipe_mutex;
void anv_wddm_profile_pipe_bits(uint32_t bits, const char *reason)
{
   mtx_lock(&pipe_mutex);
   uint32_t i = 0;
   while (i < pipe_reason_count && pipe_reasons[i].reason != reason) ++i;
   if (i == pipe_reason_count && i < PIPE_REASONS) { pipe_reasons[i].reason = reason; pipe_reasons[i].stall_bits = 0; ++pipe_reason_count; }
   if (i < pipe_reason_count) { ++pipe_reasons[i].count; pipe_reasons[i].stall_bits |= bits; }
   mtx_unlock(&pipe_mutex);
}

static int draws_enabled(void)
{
   static int on = -1;
   if (on < 0) on = getenv("HASVK_WDDM_PROFILE_DRAWS") != NULL;
   return on;
}

static int enabled(void)
{
   static int on = -1;
   if (on < 0) on = getenv("HASVK_WDDM_PROFILE_PASSES") != NULL;
   return on;
}

static VkDevice device_handle(struct anv_device *device) { return anv_device_to_handle(device); }

/* Returns slot + 1, or 0 if this command is not profiled. */
static uint32_t begin(struct anv_cmd_buffer *cmd, const struct meta *meta)
{
   if (!cmd->wddm || (g.device && g.device != cmd->device)) return 0;
   if (!g.pool) {
      mtx_lock(&g.mutex);
      if (!g.pool && !g.device) {
         VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PAIRS * 2};
         VkQueryPool pool;
         if (g.orig.CreateQueryPool(device_handle(cmd->device), &info, NULL, &pool) == VK_SUCCESS) {
            g.orig.ResetQueryPool(device_handle(cmd->device), pool, 0, PAIRS * 2);
            info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; info.queryCount = PAIRS;
            info.pipelineStatistics = STATS_FLAGS;
            VkQueryPool stats_pool;
            if (g.orig.CreateQueryPool(device_handle(cmd->device), &info, NULL, &stats_pool) == VK_SUCCESS) {
               g.orig.ResetQueryPool(device_handle(cmd->device), stats_pool, 0, PAIRS);
               g.stats_pool = stats_pool; g.pool = pool;
            }
         }
         g.device = cmd->device;
      }
      mtx_unlock(&g.mutex);
      if (!g.pool || g.device != cmd->device) return 0;
   }
   const uint32_t slot = p_atomic_inc_return(&g.next) % PAIRS;
   if (p_atomic_cmpxchg(&g.state[slot], 0, 1) != 0) { p_atomic_inc(&g.skipped); return 0; }
   g.meta[slot] = *meta;
   g.orig.CmdWriteTimestamp(anv_cmd_buffer_to_handle(cmd), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pool, slot * 2);
   if (meta->kind == KIND_PASS) g.orig.CmdBeginQuery(anv_cmd_buffer_to_handle(cmd), g.stats_pool, slot, 0);
   return slot + 1;
}

static void end(struct anv_cmd_buffer *cmd, uint32_t slot)
{
   if (slot && g.meta[slot - 1].kind == KIND_PASS) g.orig.CmdEndQuery(anv_cmd_buffer_to_handle(cmd), g.stats_pool, slot - 1);
   if (slot) g.orig.CmdWriteTimestamp(anv_cmd_buffer_to_handle(cmd), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.pool, (slot - 1) * 2 + 1);
}

/* ---- render passes ---- */
static void pass_begin(VkCommandBuffer handle, const VkRenderPassBeginInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd->wddm || cmd->wddm->profile_slot) return;
   struct meta meta = {KIND_PASS, info->renderArea.extent.width, info->renderArea.extent.height, 0,
                       (uint64_t)(uintptr_t)info->renderPass, 0};
   VK_FROM_HANDLE(vk_framebuffer, fb, info->framebuffer);
   if (fb && !(fb->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
      for (uint32_t i = 0; i < fb->attachment_count && i < 8; ++i) {
         ANV_FROM_HANDLE(anv_image_view, view, fb->attachments[i]);
         meta.aux |= ((uint32_t)view->image->planes[0].aux_usage & 15) << (4 * i);
      }
   cmd->wddm->profile_slot = begin(cmd, &meta);
   cmd->wddm->profile_framebuffer = (uint64_t)(uintptr_t)info->framebuffer;
   cmd->wddm->profile_render_pass = (uint64_t)(uintptr_t)info->renderPass;
   cmd->wddm->profile_width = info->renderArea.extent.width; cmd->wddm->profile_height = info->renderArea.extent.height;
}
/* HASVK_WDDM_DUMP_DIR, HASVK_WDDM_DUMP_AT (seconds after the first 1536x1536
 * pass, default 20): the depth attachment of every 1536x1536 pass recorded in
 * one frame is copied (after the pass: final layout -> TRANSFER_SRC, copy,
 * back) into a host-visible buffer and written as raw rows about 2 s later
 * (<dir>\vulkan-shadow-<n>.r16). The buffers are kept: the command buffer may
 * be submitted again. */
struct dump { void *map; uint64_t bytes; uint64_t recorded_ms; uint32_t width, height, index; bool written; };
static struct dump dumps[8];
static uint32_t dump_count;
static uint64_t dump_first_ms, dump_frame = UINT64_MAX;
static void dump_after_pass(struct anv_cmd_buffer *cmd)
{
   static const char *dir = (const char *)1;
   if (dir == (const char *)1) dir = getenv("HASVK_WDDM_DUMP_DIR");
   if (!dir || cmd->wddm->profile_width != 1536 || cmd->wddm->profile_height != 1536 || !cmd->wddm->profile_framebuffer) return;
   const uint64_t now = os_time_get_nano() / 1000000;
   mtx_lock(&g.mutex);
   if (!dump_first_ms) dump_first_ms = now;
   const char *at = getenv("HASVK_WDDM_DUMP_AT");
   const uint64_t wait = (at ? strtoull(at, NULL, 10) : 20) * 1000;
   if (dump_frame == UINT64_MAX && now - dump_first_ms >= wait) dump_frame = p_atomic_read(&g.frames);
   const bool take = dump_frame == p_atomic_read(&g.frames) && dump_count < ARRAY_SIZE(dumps);
   const uint32_t index = take ? dump_count++ : 0;
   mtx_unlock(&g.mutex);
   if (!take) return;
   VK_FROM_HANDLE(vk_framebuffer, fb, (VkFramebuffer)(uintptr_t)cmd->wddm->profile_framebuffer);
   VK_FROM_HANDLE(vk_render_pass, pass, (VkRenderPass)(uintptr_t)cmd->wddm->profile_render_pass);
   if (!fb || !pass || (fb->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) return;
   for (uint32_t i = 0; i < fb->attachment_count && i < pass->attachment_count; ++i) {
      ANV_FROM_HANDLE(anv_image_view, view, fb->attachments[i]);
      const struct anv_image *image = view->image;
      if (!(image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) continue;
      const VkDevice device = device_handle(cmd->device);
      const uint32_t cpp = image->vk.format == VK_FORMAT_D16_UNORM ? 2 : 4;
      const uint64_t bytes = (uint64_t)1536 * 1536 * cpp;
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      VkBuffer buffer; VkDeviceMemory memory; void *map;
      if (g.orig.CreateBuffer(device, &bi, NULL, &buffer) != VK_SUCCESS) return;
      VkMemoryRequirements req; g.orig.GetBufferMemoryRequirements(device, buffer, &req);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = 0};
      if (g.orig.AllocateMemory(device, &ai, NULL, &memory) != VK_SUCCESS || g.orig.BindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
          g.orig.MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
         return;
      const VkImageLayout final = pass->attachments[i].final_layout;
      VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = final, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = anv_image_to_handle((struct anv_image *)image),
         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, view->vk.base_mip_level, 1, view->vk.base_array_layer, 1}};
      const VkCommandBuffer handle = anv_cmd_buffer_to_handle(cmd);
      g.orig.CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, view->vk.base_mip_level, view->vk.base_array_layer, 1},
                                  .imageExtent = {1536, 1536, 1}};
      g.orig.CmdCopyImageToBuffer(handle, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
      b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = final;
      g.orig.CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      mtx_lock(&g.mutex);
      dumps[index] = (struct dump){map, bytes, now, 1536, 1536, index, false};
      mtx_unlock(&g.mutex);
      fprintf(stderr, "HasVK/WDDM profile: shadow map %u recorded for dumping (format %d, final layout %d)\n", index, image->vk.format, final);
      return;
   }
}
static void write_dumps(void)
{
   const char *dir = getenv("HASVK_WDDM_DUMP_DIR");
   if (!dir) return;
   const uint64_t now = os_time_get_nano() / 1000000;
   for (uint32_t i = 0; i < ARRAY_SIZE(dumps); ++i) {
      struct dump *d = &dumps[i];
      if (!d->map || d->written || now - d->recorded_ms < 2000) continue;
      char path[1024]; snprintf(path, sizeof(path), "%s\\vulkan-shadow-%u.r16", dir, d->index);
      FILE *out = fopen(path, "wb");
      if (out) { fwrite(d->map, 1, d->bytes, out); fclose(out); }
      fprintf(stderr, "HasVK/WDDM profile: shadow map %u written to %s\n", d->index, path);
      d->written = true;
   }
}
static void pass_end(VkCommandBuffer handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd->wddm || !cmd->wddm->profile_slot) return;
   end(cmd, cmd->wddm->profile_slot);
   cmd->wddm->profile_slot = 0;
   dump_after_pass(cmd);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdBeginRenderPass(VkCommandBuffer c, const VkRenderPassBeginInfo *info, VkSubpassContents contents)
{
   pass_begin(c, info);
   g.orig.CmdBeginRenderPass(c, info, contents);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdBeginRenderPass2(VkCommandBuffer c, const VkRenderPassBeginInfo *info, const VkSubpassBeginInfo *sub)
{
   pass_begin(c, info);
   g.orig.CmdBeginRenderPass2(c, info, sub);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdEndRenderPass(VkCommandBuffer c)
{
   g.orig.CmdEndRenderPass(c);
   pass_end(c);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdEndRenderPass2(VkCommandBuffer c, const VkSubpassEndInfo *info)
{
   g.orig.CmdEndRenderPass2(c, info);
   pass_end(c);
}

/* ---- draws (counted into the current pass; secondaries add theirs when executed) ---- */
static void count_draw(VkCommandBuffer handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd->wddm) return;
   ++cmd->wddm->profile_draws;
   if (cmd->wddm->profile_slot) ++g.meta[cmd->wddm->profile_slot - 1].draws;
}
/* HASVK_WDDM_PROFILE_DRAWS=1: every draw timed on its own, by pipeline. */
static uint32_t draw_begin(VkCommandBuffer handle, uint64_t vertices)
{
   count_draw(handle);
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!draws_enabled() || !cmd->wddm || !cmd->state.gfx.pipeline) return 0;
   const struct meta meta = {KIND_DRAW, 0, 0, (uint32_t)MIN2(vertices, UINT32_MAX), (uint64_t)(uintptr_t)cmd->state.gfx.pipeline, 0, 0};
   return begin(cmd, &meta);
}
static void draw_end(VkCommandBuffer handle, uint32_t slot)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   end(cmd, slot);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{ anv_wddm_capture_draw(anv_cmd_buffer_from_handle(c), false, a, b, d, 0, e);
  uint32_t s = draw_begin(c, (uint64_t)a * b); g.orig.CmdDraw(c, a, b, d, e); draw_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f)
{ anv_wddm_capture_draw(anv_cmd_buffer_from_handle(c), true, a, b, d, e, f);
  uint32_t s = draw_begin(c, (uint64_t)a * b); g.orig.CmdDrawIndexed(c, a, b, d, e, f); draw_end(c, s); }
static VKAPI_ATTR VkResult VKAPI_CALL wrap_EndCommandBuffer(VkCommandBuffer c)
{
   anv_wddm_capture_end_command_buffer(anv_cmd_buffer_from_handle(c));
   return g.orig.EndCommandBuffer(c);
}
static VKAPI_ATTR VkResult VKAPI_CALL wrap_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
   const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *alloc, VkPipeline *pipelines)
{
   const VkResult r = g.orig.CreateGraphicsPipelines(d, cache, n, infos, alloc, pipelines);
   anv_wddm_capture_pipelines(infos, n, pipelines);
   return r;
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{ uint32_t t = draw_begin(c, 0); g.orig.CmdDrawIndirect(c, b, o, n, s); draw_end(c, t); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{ uint32_t t = draw_begin(c, 0); g.orig.CmdDrawIndexedIndirect(c, b, o, n, s); draw_end(c, t); }
static void note_depth_pipeline(const struct anv_graphics_pipeline *pipeline)
{
   bool depth_only = true;
   for (uint32_t i = 0; i < MAX_RTS; ++i)
      if (pipeline->color_comp_writes[i]) depth_only = false;
   mtx_lock(&g.mutex);
   uint32_t i = 0;
   while (i < g.depth_pipeline_count && g.depth_pipelines[i].pipeline != pipeline) ++i;
   if (i == g.depth_pipeline_count && i < DEPTH_PIPELINES) {
      struct depth_pipeline *d = &g.depth_pipelines[g.depth_pipeline_count++];
      d->pipeline = pipeline; d->binds = 0; d->depth_only = depth_only;
      int n = snprintf(d->text, sizeof(d->text), "kill %d", pipeline->kill_pixel);
      util_dynarray_foreach(&pipeline->base.executables, struct anv_pipeline_executable, e)
         n += snprintf(d->text + n, sizeof(d->text) - n, " | %s SIMD%u %u inst %u cycles %u sends %u spills",
                       _mesa_shader_stage_to_abbrev(e->stage), e->stats.dispatch_width, e->stats.instructions,
                       e->stats.cycles, e->stats.sends, e->stats.spills);
   }
   if (i < g.depth_pipeline_count && depth_only) ++g.depth_pipelines[i].binds;
   mtx_unlock(&g.mutex);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline handle)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, c);
   ANV_FROM_HANDLE(anv_pipeline, pipeline, handle);
   if (cmd->wddm && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      const uint32_t kill = anv_pipeline_to_graphics(pipeline)->kill_pixel ? 2 : 1;
      if (cmd->wddm->profile_kill && cmd->wddm->profile_kill != kill) {
         ++cmd->wddm->profile_kill_switches;   /* secondaries: added to the pass that executes them */
         if (cmd->wddm->profile_slot) ++g.meta[cmd->wddm->profile_slot - 1].kill_switches;
      }
      cmd->wddm->profile_kill = kill;
      note_depth_pipeline(anv_pipeline_to_graphics(pipeline));
   }
   g.orig.CmdBindPipeline(c, point, handle);
}
static VKAPI_ATTR VkResult VKAPI_CALL wrap_BeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, c);
   const VkResult result = g.orig.BeginCommandBuffer(c, info);
   if (cmd->wddm) { cmd->wddm->profile_slot = 0; cmd->wddm->profile_draws = 0; cmd->wddm->profile_kill = 0; cmd->wddm->profile_kill_switches = 0; }
   return result;
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdExecuteCommands(VkCommandBuffer c, uint32_t count, const VkCommandBuffer *secondaries)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, c);
   if (cmd->wddm && cmd->wddm->profile_slot)
      for (uint32_t i = 0; i < count; ++i) {
         ANV_FROM_HANDLE(anv_cmd_buffer, secondary, secondaries[i]);
         if (secondary->wddm) {
            g.meta[cmd->wddm->profile_slot - 1].draws += secondary->wddm->profile_draws;
            g.meta[cmd->wddm->profile_slot - 1].kill_switches += secondary->wddm->profile_kill_switches;
         }
      }
   g.orig.CmdExecuteCommands(c, count, secondaries);
}

/* ---- commands outside render passes ---- */
static uint32_t outside(VkCommandBuffer handle, uint32_t kind, uint32_t width, uint32_t height)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   if (!cmd->wddm || cmd->wddm->profile_slot) return 0;
   const struct meta meta = {kind, width, height, 0, 0};
   return begin(cmd, &meta);
}
static void outside_end(VkCommandBuffer handle, uint32_t slot)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd, handle);
   end(cmd, slot);
}
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z)
{ uint32_t s = outside(c, KIND_DISPATCH, x, y * z); g.orig.CmdDispatch(c, x, y, z); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o)
{ uint32_t s = outside(c, KIND_DISPATCH, 0, 0); g.orig.CmdDispatchIndirect(c, b, o); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdCopyBuffer(VkCommandBuffer c, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy *r)
{ uint32_t s = outside(c, KIND_COPY, 0, 0); g.orig.CmdCopyBuffer(c, a, b, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdCopyImage(VkCommandBuffer c, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageCopy *r)
{ uint32_t s = outside(c, KIND_COPY, n ? r[0].extent.width : 0, n ? r[0].extent.height : 0); g.orig.CmdCopyImage(c, a, al, b, bl, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdCopyBufferToImage(VkCommandBuffer c, VkBuffer a, VkImage b, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r)
{ uint32_t s = outside(c, KIND_COPY, n ? r[0].imageExtent.width : 0, n ? r[0].imageExtent.height : 1); g.orig.CmdCopyBufferToImage(c, a, b, l, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdCopyImageToBuffer(VkCommandBuffer c, VkImage a, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r)
{ uint32_t s = outside(c, KIND_COPY, n ? r[0].imageExtent.width : 0, n ? r[0].imageExtent.height : 2); g.orig.CmdCopyImageToBuffer(c, a, l, b, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdBlitImage(VkCommandBuffer c, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageBlit *r, VkFilter f)
{ uint32_t s = outside(c, KIND_BLIT, n ? (uint32_t)abs(r[0].dstOffsets[1].x - r[0].dstOffsets[0].x) : 0,
                       n ? (uint32_t)abs(r[0].dstOffsets[1].y - r[0].dstOffsets[0].y) : 0);
  g.orig.CmdBlitImage(c, a, al, b, bl, n, r, f); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdResolveImage(VkCommandBuffer c, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageResolve *r)
{ uint32_t s = outside(c, KIND_RESOLVE, n ? r[0].extent.width : 0, n ? r[0].extent.height : 0); g.orig.CmdResolveImage(c, a, al, b, bl, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r)
{ uint32_t s = outside(c, KIND_CLEAR, 0, 0); g.orig.CmdClearColorImage(c, i, l, v, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r)
{ uint32_t s = outside(c, KIND_CLEAR, 1, 0); g.orig.CmdClearDepthStencilImage(c, i, l, v, n, r); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdFillBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize size, uint32_t d)
{ uint32_t s = outside(c, KIND_FILL, 0, 0); g.orig.CmdFillBuffer(c, b, o, size, d); outside_end(c, s); }
static VKAPI_ATTR void VKAPI_CALL wrap_CmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkDeviceSize size, const void *d)
{ uint32_t s = outside(c, KIND_FILL, 1, 0); g.orig.CmdUpdateBuffer(c, b, o, size, d); outside_end(c, s); }

static VKAPI_ATTR VkResult VKAPI_CALL wrap_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info)
{
   p_atomic_inc(&g.frames);
   return g.orig.QueuePresentKHR(q, info);
}

void
anv_wddm_profile_install(struct vk_device_dispatch_table *d)
{
   if (!enabled()) return;
   static int installed;
   if (installed++) { fprintf(stderr, "HasVK/WDDM profile: only the first device is profiled\n"); return; }
   mtx_init(&g.mutex, mtx_plain);
   mtx_init(&pipe_mutex, mtx_plain);
   anv_wddm_profile_pipe_bits_enabled = true;
   g.orig = *d;
#define WRAP(name) if (d->name) d->name = wrap_##name
   WRAP(CmdBeginRenderPass); WRAP(CmdBeginRenderPass2); WRAP(CmdEndRenderPass); WRAP(CmdEndRenderPass2);
   WRAP(CmdDraw); WRAP(CmdDrawIndexed); WRAP(CmdDrawIndirect); WRAP(CmdDrawIndexedIndirect);
   WRAP(BeginCommandBuffer); WRAP(CmdExecuteCommands); WRAP(CmdBindPipeline); WRAP(CreateGraphicsPipelines); WRAP(EndCommandBuffer);
   WRAP(CmdDispatch); WRAP(CmdDispatchIndirect); WRAP(CmdCopyBuffer); WRAP(CmdCopyImage);
   WRAP(CmdCopyBufferToImage); WRAP(CmdCopyImageToBuffer); WRAP(CmdBlitImage); WRAP(CmdResolveImage);
   WRAP(CmdClearColorImage); WRAP(CmdClearDepthStencilImage); WRAP(CmdFillBuffer); WRAP(CmdUpdateBuffer);
   WRAP(QueuePresentKHR);
#undef WRAP
   fprintf(stderr, "HasVK/WDDM profile: timing render passes, dispatches and transfers\n");
}

static int compare_buckets(const void *a, const void *b)
{
   const struct bucket *x = a, *y = b;
   return x->ticks < y->ticks ? 1 : x->ticks > y->ticks ? -1 : 0;
}

static void add(const struct meta *m, uint64_t ticks, const uint64_t *stats)
{
   uint32_t i = 0;
   for (; i < g.bucket_count; ++i) {
      const struct meta *k = &g.buckets[i].key;
      if (k->kind == m->kind && k->object == m->object && k->width == m->width && k->height == m->height) break;
   }
   if (i == g.bucket_count) {
      if (i == BUCKETS) return;
      g.buckets[g.bucket_count++] = (struct bucket){.key = *m};
      g.buckets[i].key.draws = 0; g.buckets[i].key.kill_switches = 0;
      describe(m, g.buckets[i].text, sizeof(g.buckets[i].text));
   }
   ++g.buckets[i].count; g.buckets[i].ticks += ticks; g.buckets[i].draws += m->draws; g.buckets[i].kill_switches += m->kill_switches;
   if (stats) for (int s = 0; s < 6; ++s) g.buckets[i].stats[s] += stats[s];
}

static void describe(const struct meta *m, char *out, size_t size)
{
   if (m->kind == KIND_DRAW) {
      const char *text = "?"; bool depth_only = false;
      for (uint32_t i = 0; i < g.depth_pipeline_count; ++i)
         if (g.depth_pipelines[i].pipeline == (const void *)(uintptr_t)m->object) { text = g.depth_pipelines[i].text; depth_only = g.depth_pipelines[i].depth_only; }
      snprintf(out, size, "draw %s %s", depth_only ? "depth-only" : "colour    ", text);
      return;
   }
   int n = snprintf(out, size, "%-11s %4ux%-4u", kind_names[m->kind], m->width, m->height);
   if (m->kind != KIND_PASS || !m->object) return;
   VK_FROM_HANDLE(vk_render_pass, pass, (VkRenderPass)(uintptr_t)m->object);
   n += snprintf(out + n, size - n, " %u subpass, att:", pass->subpass_count);
   for (uint32_t i = 0; i < pass->attachment_count && n < (int)size - 24; ++i)
      n += snprintf(out + n, size - n, " %d%s%s%s", pass->attachments[i].format, i < 8 && (m->aux >> (4 * i)) & 15 ? "/aux" : "",
                    pass->attachments[i].samples > 1 ? "xMS" : "",
                    pass->attachments[i].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR ? "/clr" :
                    pass->attachments[i].load_op == VK_ATTACHMENT_LOAD_OP_LOAD ? "/load" : "");
}

void
anv_wddm_profile_report(struct anv_device *device)
{
   if (!enabled() || !g.pool || g.device != device) return;
   if (mtx_trylock(&g.mutex) != thrd_success) return;
   write_dumps();
   anv_wddm_capture_poll();
   const VkDevice handle = device_handle(device);
   const uint64_t mask = device->physical->info.timestamp_frequency ? (1ull << 36) - 1 : ~0ull;
   for (uint32_t slot = 0; slot < PAIRS; ++slot) {
      if (p_atomic_read(&g.state[slot]) != 1) continue;
      uint64_t r[4];
      if (g.orig.GetQueryPoolResults(handle, g.pool, slot * 2, 2, sizeof(r), r, 16,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
          !r[1] || !r[3])
         continue;
      uint64_t stats[7];
      const bool pass = g.meta[slot].kind == KIND_PASS;
      if (pass && (g.orig.GetQueryPoolResults(handle, g.stats_pool, slot, 1, sizeof(stats), stats, sizeof(stats),
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
                   !stats[6]))
         continue;
      add(&g.meta[slot], (r[2] - r[0]) & mask, pass ? stats : NULL);
      g.orig.ResetQueryPool(handle, g.pool, slot * 2, 2);
      if (pass) g.orig.ResetQueryPool(handle, g.stats_pool, slot, 1);
      p_atomic_set(&g.state[slot], 0);
   }
   const uint64_t now = os_time_get_nano() / 1000000;
   if (!g.window_start_ms) g.window_start_ms = now;
   if (now - g.window_start_ms >= 5000) {
      const uint64_t frames = p_atomic_xchg(&g.frames, 0);
      const double ms_per_tick = 1000.0 / device->physical->info.timestamp_frequency;
      const double divisor = frames ? (double)frames : 1.0;
      qsort(g.buckets, g.bucket_count, sizeof(g.buckets[0]), compare_buckets);
      double total = 0;
      for (uint32_t i = 0; i < g.bucket_count; ++i) total += g.buckets[i].ticks * ms_per_tick;
      fprintf(stderr, "HasVK/WDDM profile: %llu frames in %.1f s, profiled GPU time %.2f ms/frame (%llu skipped)\n",
              (unsigned long long)frames, (now - g.window_start_ms) / 1000.0, total / divisor, (unsigned long long)g.skipped);
      for (uint32_t i = 0; i < g.bucket_count && i < 25; ++i) {
         const struct bucket *b = &g.buckets[i];
         fprintf(stderr, "  %6.2f ms/frame %5.1f/frame avg %6.3f ms draws %5.0f kill switches %4.0f | %s\n", b->ticks * ms_per_tick / divisor,
                 b->count / divisor, b->ticks * ms_per_tick / b->count, (double)b->draws / b->count,
                 (double)b->kill_switches / b->count, b->text);
         if (b->key.kind == KIND_PASS && i < 8)
            fprintf(stderr, "      per pass: %.0fk IA vertices, %.0fk prims, %.0fk VS, %.0fk clip in, %.0fk clip out, %.0fk FS\n",
                    b->stats[0] / 1e3 / b->count, b->stats[1] / 1e3 / b->count, b->stats[2] / 1e3 / b->count,
                    b->stats[3] / 1e3 / b->count, b->stats[4] / 1e3 / b->count, b->stats[5] / 1e3 / b->count);
      }
      for (uint32_t i = 0; i < g.depth_pipeline_count; ++i)
         for (uint32_t j = i + 1; j < g.depth_pipeline_count; ++j)
            if (g.depth_pipelines[j].binds > g.depth_pipelines[i].binds) {
               struct depth_pipeline t = g.depth_pipelines[i]; g.depth_pipelines[i] = g.depth_pipelines[j]; g.depth_pipelines[j] = t;
            }
      for (uint32_t i = 0; i < g.depth_pipeline_count && i < 8; ++i)
         fprintf(stderr, "  depth-only pipeline %5.1f binds/frame: %s\n", g.depth_pipelines[i].binds / divisor, g.depth_pipelines[i].text);
      for (uint32_t i = 0; i < g.depth_pipeline_count; ++i) g.depth_pipelines[i].binds = 0;
      mtx_lock(&pipe_mutex);
      for (uint32_t i = 0; i < pipe_reason_count; ++i)
         for (uint32_t j = i + 1; j < pipe_reason_count; ++j)
            if (pipe_reasons[j].count > pipe_reasons[i].count) {
               const char *r = pipe_reasons[i].reason; uint32_t b = pipe_reasons[i].stall_bits; uint64_t n = pipe_reasons[i].count;
               pipe_reasons[i] = pipe_reasons[j]; pipe_reasons[j].reason = r; pipe_reasons[j].stall_bits = b; pipe_reasons[j].count = n;
            }
      for (uint32_t i = 0; i < pipe_reason_count && i < 12; ++i)
         if (pipe_reasons[i].count)
            fprintf(stderr, "  pipe bits %7.1f/frame bits 0x%08x: %s\n", pipe_reasons[i].count / divisor, pipe_reasons[i].stall_bits, pipe_reasons[i].reason);
      for (uint32_t i = 0; i < pipe_reason_count; ++i) pipe_reasons[i].count = 0;
      mtx_unlock(&pipe_mutex);
      g.bucket_count = 0; g.skipped = 0; g.window_start_ms = now;
   }
   mtx_unlock(&g.mutex);
}
