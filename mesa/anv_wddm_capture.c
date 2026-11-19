/* HASVK_WDDM_CAPTURE_DIR + HASVK_WDDM_CAPTURE_VS_INST=<n>: capture one
 * graphics pipeline whose vertex shader compiles to <n> ELK instructions,
 * and the data of its first draw, so that the draw can be replayed alone
 * (native/tests/replay_probe.cpp) to compare compiler variants quickly.
 * Written to the directory:
 *   capture.txt   pipeline state, layout, draw parameters, list of files
 *   vs.spv/fs.spv SPIR-V of the stages
 *   *.bin         push constants, buffer descriptor contents, vertex and
 *                 index buffers (only memory the CPU can read; device-only
 *                 memory is listed as unmapped)
 * Diagnostic only, wired in by anv_wddm_profile.c: needs
 * HASVK_WDDM_PROFILE_PASSES=1 as well (its entry point wrappers). */
#include "anv_wddm.h"
#include "vk_shader_module.h"
#include "vk_render_pass.h"
#include "vk_framebuffer.h"
#include "vk_format.h"
#include "util/u_atomic.h"
#include "util/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pass mode (HASVK_WDDM_CAPTURE_PASS=<n>): every graphics pipeline without
 * colour attachments is written as it is created (pipelines.txt,
 * p<i>-vs.spv, p<i>-fs.spv); HASVK_WDDM_CAPTURE_AT seconds (default 20) after
 * the first render pass with an n x n render area, the next such pass of a
 * primary command buffer is written draw by draw (pass.txt) with the buffers
 * its draws read (buf<i>.bin, each buffer range once). Images are not
 * captured (the replay substitutes opaque placeholders). */
/* dynamic_start: the layout's first dynamic offset per set, kept because a
 * layout may be destroyed while its pipelines live on. */
struct pipeline_entry { const void *pipeline; uint32_t index; uint32_t dynamic_start[MAX_SETS]; };
struct buffer_entry { const struct anv_buffer *buffer; uint64_t offset, size; uint32_t id; };

static struct {
   int enabled;              /* -1 unknown */
   char dir[512];
   uint32_t vs_instructions;
   const void *pipeline;     /* captured anv_graphics_pipeline */
   const struct anv_pipeline_layout *layout;
   uint32_t draw_done;
   mtx_t mutex;
   /* pass mode */
   uint32_t pass_size, pass_at_s, pass_draws, pass_executes;
   uint64_t pass_first_ms;
   bool pass_done;
   const struct anv_cmd_buffer *pass_cmd;
   FILE *pass_txt;
   struct pipeline_entry *pipelines; uint32_t pipeline_count, pipeline_capacity;
   struct buffer_entry *buffers; uint32_t buffer_count, buffer_capacity;
   uint64_t captured_bytes;
} c = {.enabled = -1};

static bool capture_enabled(void)
{
   if (c.enabled < 0) {
      const char *dir = getenv("HASVK_WDDM_CAPTURE_DIR"), *inst = getenv("HASVK_WDDM_CAPTURE_VS_INST");
      const char *pass = getenv("HASVK_WDDM_CAPTURE_PASS"), *at = getenv("HASVK_WDDM_CAPTURE_AT");
      c.enabled = dir && (inst || pass);
      if (c.enabled) {
         snprintf(c.dir, sizeof(c.dir), "%s", dir);
         c.vs_instructions = inst ? (uint32_t)strtoul(inst, NULL, 10) : 0;
         c.pass_size = pass ? (uint32_t)strtoul(pass, NULL, 10) : 0;
         c.pass_at_s = at ? (uint32_t)strtoul(at, NULL, 10) : 20;
         mtx_init(&c.mutex, mtx_plain);
      }
   }
   return c.enabled;
}

static FILE *open_out(const char *name, const char *mode)
{
   char path[700]; snprintf(path, sizeof(path), "%s\\%s", c.dir, name);
   return fopen(path, mode);
}

static void write_file(const char *name, const void *data, size_t size)
{
   FILE *f = open_out(name, "wb");
   if (f) { fwrite(data, 1, size, f); fclose(f); }
}

static void write_hex(FILE *f, const void *data, size_t size)
{
   for (size_t i = 0; i < size; ++i) fprintf(f, "%02x", ((const uint8_t *)data)[i]);
}

static void write_stage(FILE *txt, const VkPipelineShaderStageCreateInfo *stage, const char *file, const char *tag)
{
   const void *code = NULL; size_t size = 0;
   if (stage->module) {
      VK_FROM_HANDLE(vk_shader_module, module, stage->module);
      code = module->data; size = module->size;
   } else {
      const VkShaderModuleCreateInfo *info = vk_find_struct_const(stage->pNext, SHADER_MODULE_CREATE_INFO);
      if (info) { code = info->pCode; size = info->codeSize; }
   }
   if (!code) { fprintf(txt, "%s_module none\n", tag); return; }
   write_file(file, code, size);
   fprintf(txt, "%s_module %s %s\n", tag, file, stage->pName);
   const VkSpecializationInfo *spec = stage->pSpecializationInfo;
   if (spec) {
      for (uint32_t i = 0; i < spec->mapEntryCount; ++i)
         fprintf(txt, "%s_spec %u %u %zu\n", tag, spec->pMapEntries[i].constantID, spec->pMapEntries[i].offset, spec->pMapEntries[i].size);
      fprintf(txt, "%s_spec_data ", tag); write_hex(txt, spec->pData, spec->dataSize); fprintf(txt, "\n");
   }
}

static void write_pipeline(FILE *txt, const VkGraphicsPipelineCreateInfo *info, const struct anv_graphics_pipeline *pipeline,
                           const char *vs_file, const char *fs_file)
{
   for (uint32_t i = 0; i < info->stageCount; ++i) {
      if (info->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) write_stage(txt, &info->pStages[i], vs_file, "vs");
      if (info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) write_stage(txt, &info->pStages[i], fs_file, "fs");
   }
   util_dynarray_foreach(&pipeline->base.executables, struct anv_pipeline_executable, e)
      fprintf(txt, "stats %s simd%u %u instructions %u cycles %u sends\n", _mesa_shader_stage_to_abbrev(e->stage),
              e->stats.dispatch_width, e->stats.instructions, e->stats.cycles, e->stats.sends);
   const VkPipelineVertexInputStateCreateInfo *vi = info->pVertexInputState;
   if (vi) {
      for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; ++i)
         fprintf(txt, "vb_binding %u %u %u\n", vi->pVertexBindingDescriptions[i].binding, vi->pVertexBindingDescriptions[i].stride,
                 vi->pVertexBindingDescriptions[i].inputRate);
      for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i)
         fprintf(txt, "vb_attr %u %u %d %u\n", vi->pVertexAttributeDescriptions[i].location, vi->pVertexAttributeDescriptions[i].binding,
                 vi->pVertexAttributeDescriptions[i].format, vi->pVertexAttributeDescriptions[i].offset);
   }
   if (info->pInputAssemblyState) fprintf(txt, "topology %d\n", info->pInputAssemblyState->topology);
   if (info->pRasterizationState) {
      const VkPipelineRasterizationStateCreateInfo *r = info->pRasterizationState;
      fprintf(txt, "raster %u %u %u %u %g %g %g %u\n", r->cullMode, r->frontFace, r->polygonMode, r->depthBiasEnable,
              r->depthBiasConstantFactor, r->depthBiasSlopeFactor, r->depthBiasClamp, r->depthClampEnable);
   }
   if (info->pDepthStencilState)
      fprintf(txt, "depth %u %u %d\n", info->pDepthStencilState->depthTestEnable, info->pDepthStencilState->depthWriteEnable,
              info->pDepthStencilState->depthCompareOp);
   if (info->pDynamicState)
      for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; ++i) fprintf(txt, "dynamic %d\n", info->pDynamicState->pDynamicStates[i]);
   ANV_FROM_HANDLE(anv_pipeline_layout, layout, info->layout);
   for (uint32_t i = 0; layout && i < layout->num_sets; ++i)
      if (layout->set[i].layout) fprintf(txt, "set_layout %u dynamic_start %u\n", i, layout->set[i].dynamic_offset_start);
   for (uint32_t s = 0; layout && s < layout->num_sets; ++s) {
      const struct anv_descriptor_set_layout *set = layout->set[s].layout;
      for (uint32_t b = 0; set && b < set->binding_count; ++b)
         if (set->binding[b].array_size)
            fprintf(txt, "set %u binding %u type %d count %u stages 0x%x\n", s, b, set->binding[b].type, set->binding[b].array_size,
                    set->shader_stages);
   }
   if (layout) fprintf(txt, "set_count %u\n", layout->num_sets);
}

static void capture_pipeline(const VkGraphicsPipelineCreateInfo *info, const struct anv_graphics_pipeline *pipeline)
{
   FILE *txt = open_out("capture.txt", "w");
   if (!txt) return;
   write_pipeline(txt, info, pipeline, "vs.spv", "fs.spv");
   fclose(txt);
   ANV_FROM_HANDLE(anv_pipeline_layout, layout, info->layout);
   c.pipeline = pipeline; c.layout = layout;
   fprintf(stderr, "HasVK/WDDM capture: pipeline captured to %s\n", c.dir);
}

static void record_pass_pipeline(const VkGraphicsPipelineCreateInfo *info, const struct anv_graphics_pipeline *pipeline)
{
   if (info->pColorBlendState && info->pColorBlendState->attachmentCount) return;
   mtx_lock(&c.mutex);
   if (c.pipeline_count == c.pipeline_capacity) {
      c.pipeline_capacity = c.pipeline_capacity ? c.pipeline_capacity * 2 : 256;
      c.pipelines = realloc(c.pipelines, c.pipeline_capacity * sizeof(*c.pipelines));
   }
   const uint32_t index = c.pipeline_count;
   struct pipeline_entry *entry = &c.pipelines[c.pipeline_count++];
   *entry = (struct pipeline_entry){pipeline, index, {0}};
   ANV_FROM_HANDLE(anv_pipeline_layout, layout, info->layout);
   for (uint32_t s = 0; layout && s < layout->num_sets && s < MAX_SETS; ++s) entry->dynamic_start[s] = layout->set[s].dynamic_offset_start;
   FILE *txt = open_out("pipelines.txt", index ? "a" : "w");
   if (txt) {
      char vs[32], fs[32]; snprintf(vs, sizeof(vs), "p%u-vs.spv", index); snprintf(fs, sizeof(fs), "p%u-fs.spv", index);
      fprintf(txt, "pipeline %u\n", index);
      write_pipeline(txt, info, pipeline, vs, fs);
      fprintf(txt, "end\n");
      fclose(txt);
   }
   mtx_unlock(&c.mutex);
}

/* A recorded pipeline; the latest entry wins when a handle was reused. */
static const struct pipeline_entry *pass_pipeline(const void *pipeline)
{
   for (uint32_t i = c.pipeline_count; i-- > 0;)
      if (c.pipelines[i].pipeline == pipeline) return &c.pipelines[i];
   return NULL;
}

void anv_wddm_capture_pipelines(const VkGraphicsPipelineCreateInfo *infos, uint32_t count, const VkPipeline *pipelines)
{
   if (!capture_enabled()) return;
   if (c.pass_size) {
      for (uint32_t i = 0; i < count; ++i) {
         if (!pipelines[i]) continue;
         ANV_FROM_HANDLE(anv_pipeline, base, pipelines[i]);
         if (base->type == ANV_PIPELINE_GRAPHICS) record_pass_pipeline(&infos[i], anv_pipeline_to_graphics(base));
      }
   }
   if (!c.vs_instructions || c.pipeline) return;
   for (uint32_t i = 0; i < count; ++i) {
      if (!pipelines[i]) continue;
      ANV_FROM_HANDLE(anv_pipeline, base, pipelines[i]);
      if (base->type != ANV_PIPELINE_GRAPHICS) continue;
      util_dynarray_foreach(&base->executables, struct anv_pipeline_executable, e) {
         if (e->stage == MESA_SHADER_VERTEX && e->stats.instructions == c.vs_instructions) {
            mtx_lock(&c.mutex);
            if (!c.pipeline) capture_pipeline(&infos[i], anv_pipeline_to_graphics(base));
            mtx_unlock(&c.mutex);
            return;
         }
      }
   }
}

/* Memory the CPU cannot read (device-only BOs) is copied by the GPU into a
 * host-visible buffer at the end of the command buffer that drew (copies are
 * not allowed inside the render pass) and written about 2 s later. Vertex and
 * index buffers do not change within a frame, and a dynamic UBO region is
 * unique to its draw. */
struct pending { const struct anv_cmd_buffer *cmd; VkBuffer src, dst; VkDeviceMemory memory; void *map; uint64_t offset, size, recorded_ms;
                 char name[64]; bool recorded, written; };
static struct pending *pendings;
static uint32_t pending_count, pending_capacity;
#define CAPTURE_BYTES_MAX (768ull << 20)

/* Writes size bytes of buffer at offset now if the CPU can read it, else
 * queues a GPU copy; lists the file in capture.txt either way. */
static void capture_memory(FILE *txt, const char *what, const char *name, const struct anv_cmd_buffer *cmd,
                           struct anv_buffer *buffer, struct anv_address address, uint64_t offset, uint64_t size)
{
   if (size > (64u << 20)) size = 64u << 20;
   /* In pass mode buffers are always copied by the GPU when the command
    * buffer executes: applications may fill mapped (ring) buffers after
    * recording the draw that reads them, until the submit. */
   const bool gpu_copy = c.pass_size && cmd == c.pass_cmd && buffer;
   if (!gpu_copy && address.bo && address.bo->map) {
      write_file(name, (const char *)address.bo->map + address.offset, (size_t)size);
      fprintf(txt, "%s %s %llu\n", what, name, (unsigned long long)size);
   } else if (buffer && c.captured_bytes + size <= CAPTURE_BYTES_MAX) {
      if (pending_count == pending_capacity) {
         pending_capacity = pending_capacity ? pending_capacity * 2 : 64;
         pendings = realloc(pendings, pending_capacity * sizeof(*pendings));
      }
      c.captured_bytes += size;
      struct pending *p = &pendings[pending_count++];
      *p = (struct pending){.cmd = cmd, .src = anv_buffer_to_handle(buffer), .offset = offset, .size = size};
      snprintf(p->name, sizeof(p->name), "%s", name);
      fprintf(txt, "%s %s %llu\n", what, name, (unsigned long long)size);
   } else {
      fprintf(txt, "%s unmapped %llu\n", what, (unsigned long long)size);
   }
}

/* Sampled images of the captured pass: every mip level and layer of the colour
 * aspect, copied by the GPU at the end of the command buffer (layout as the
 * descriptor states it, back afterwards), written as image<i>.bin with mip
 * levels one after another, each tightly packed. */
struct image_pending { const struct anv_cmd_buffer *cmd; const struct anv_image *image; VkImageLayout layout;
                       VkBuffer dst; VkDeviceMemory memory; void *map; uint64_t size, recorded_ms; uint32_t id; bool recorded, written; };
static struct image_pending *image_pendings;
static uint32_t image_pending_count, image_pending_capacity;

static uint64_t image_bytes(const struct anv_image *image)
{
   const VkFormat format = image->vk.format;
   const uint32_t bw = vk_format_get_blockwidth(format), bh = vk_format_get_blockheight(format), bs = vk_format_get_blocksize(format);
   uint64_t size = 0;
   for (uint32_t l = 0; l < image->vk.mip_levels; ++l) {
      const uint32_t w = u_minify(image->vk.extent.width, l), h = u_minify(image->vk.extent.height, l);
      size += (uint64_t)DIV_ROUND_UP(w, bw) * DIV_ROUND_UP(h, bh) * bs * image->vk.array_layers * u_minify(image->vk.extent.depth, l);
   }
   return size;
}

/* Id of a captured image, queued on first use; -1 if it cannot be captured. */
static int pass_image(const struct anv_cmd_buffer *cmd, const struct anv_image *image, VkImageLayout layout)
{
   for (uint32_t i = 0; i < image_pending_count; ++i)
      if (image_pendings[i].image == image) return (int)image_pendings[i].id;
   if (!(image->vk.aspects & VK_IMAGE_ASPECT_COLOR_BIT) || image->vk.samples != 1 || image->n_planes != 1)
      return -1;
   const uint64_t size = image_bytes(image);
   if (!size || c.captured_bytes + size > CAPTURE_BYTES_MAX) return -1;
   if (image_pending_count == image_pending_capacity) {
      image_pending_capacity = image_pending_capacity ? image_pending_capacity * 2 : 64;
      image_pendings = realloc(image_pendings, image_pending_capacity * sizeof(*image_pendings));
   }
   c.captured_bytes += size;
   const uint32_t id = image_pending_count;
   image_pendings[image_pending_count++] = (struct image_pending){.cmd = cmd, .image = image, .layout = layout, .size = size, .id = id};
   fprintf(c.pass_txt, "image_data %u image%u.bin format %d extent %u %u %u mips %u layers %u type %d size %llu\n", id, id, image->vk.format,
           image->vk.extent.width, image->vk.extent.height, image->vk.extent.depth, image->vk.mip_levels, image->vk.array_layers,
           image->vk.image_type, (unsigned long long)size);
   return (int)id;
}

static void record_image_copies(struct anv_cmd_buffer *cmd)
{
   const VkDevice device = anv_device_to_handle(cmd->device);
   const struct vk_device_dispatch_table *d = &cmd->device->vk.dispatch_table;
   const VkCommandBuffer handle = anv_cmd_buffer_to_handle(cmd);
   for (uint32_t i = 0; i < image_pending_count; ++i) {
      struct image_pending *p = &image_pendings[i];
      if (p->cmd != cmd || p->recorded) continue;
      const struct anv_image *image = p->image;
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      if (d->CreateBuffer(device, &bi, NULL, &p->dst) != VK_SUCCESS) continue;
      VkMemoryRequirements req; d->GetBufferMemoryRequirements(device, p->dst, &req);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = 0};
      if (d->AllocateMemory(device, &ai, NULL, &p->memory) != VK_SUCCESS || d->BindBufferMemory(device, p->dst, p->memory, 0) != VK_SUCCESS ||
          d->MapMemory(device, p->memory, 0, VK_WHOLE_SIZE, 0, &p->map) != VK_SUCCESS)
         continue;
      const VkImage vk_image = anv_image_to_handle((struct anv_image *)image);
      const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
      VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = p->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = vk_image, .subresourceRange = range};
      d->CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      VkBufferImageCopy regions[16]; uint32_t n = 0; uint64_t offset = 0;
      const VkFormat format = image->vk.format;
      const uint32_t bw = vk_format_get_blockwidth(format), bh = vk_format_get_blockheight(format), bs = vk_format_get_blocksize(format);
      for (uint32_t l = 0; l < image->vk.mip_levels && n < ARRAY_SIZE(regions); ++l) {
         const uint32_t w = u_minify(image->vk.extent.width, l), h = u_minify(image->vk.extent.height, l), dd = u_minify(image->vk.extent.depth, l);
         regions[n++] = (VkBufferImageCopy){.bufferOffset = offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, image->vk.array_layers},
                                            .imageExtent = {w, h, dd}};
         offset += (uint64_t)DIV_ROUND_UP(w, bw) * DIV_ROUND_UP(h, bh) * bs * image->vk.array_layers * dd;
      }
      d->CmdCopyImageToBuffer(handle, vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->dst, n, regions);
      b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = p->layout;
      d->CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      d->CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      p->recorded = true; p->recorded_ms = os_time_get_nano() / 1000000;
   }
}

static void write_images(void)
{
   const uint64_t now = os_time_get_nano() / 1000000;
   for (uint32_t i = 0; i < image_pending_count; ++i) {
      struct image_pending *p = &image_pendings[i];
      if (!p->recorded || p->written || now - p->recorded_ms < 2000) continue;
      char name[32]; snprintf(name, sizeof(name), "image%u.bin", p->id);
      write_file(name, p->map, (size_t)p->size);
      p->written = true;
      fprintf(stderr, "HasVK/WDDM capture: %s written (%llu bytes)\n", name, (unsigned long long)p->size);
   }
}

void anv_wddm_capture_end_command_buffer(struct anv_cmd_buffer *cmd)
{
   if (!capture_enabled()) return;
   record_image_copies(cmd);
   const VkDevice device = anv_device_to_handle(cmd->device);
   const struct vk_device_dispatch_table *d = &cmd->device->vk.dispatch_table;
   const VkCommandBuffer handle = anv_cmd_buffer_to_handle(cmd);
   bool barrier = false;
   for (uint32_t i = 0; i < pending_count; ++i) {
      struct pending *p = &pendings[i];
      if (p->cmd != cmd || p->recorded) continue;
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      if (d->CreateBuffer(device, &bi, NULL, &p->dst) != VK_SUCCESS) continue;
      VkMemoryRequirements req; d->GetBufferMemoryRequirements(device, p->dst, &req);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = 0};
      if (d->AllocateMemory(device, &ai, NULL, &p->memory) != VK_SUCCESS || d->BindBufferMemory(device, p->dst, p->memory, 0) != VK_SUCCESS ||
          d->MapMemory(device, p->memory, 0, VK_WHOLE_SIZE, 0, &p->map) != VK_SUCCESS)
         continue;
      if (!barrier) {
         VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
         d->CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
         barrier = true;
      }
      const VkBufferCopy region = {p->offset, 0, p->size};
      d->CmdCopyBuffer(handle, p->src, p->dst, 1, &region);
      p->recorded = true;
      p->recorded_ms = os_time_get_nano() / 1000000;
   }
   if (barrier) {
      VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      d->CmdPipelineBarrier(handle, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   }
}

void anv_wddm_capture_poll(void)
{
   if (!capture_enabled()) return;
   write_images();
   const uint64_t now = os_time_get_nano() / 1000000;
   for (uint32_t i = 0; i < pending_count; ++i) {
      struct pending *p = &pendings[i];
      if (!p->recorded || p->written || now - p->recorded_ms < 2000) continue;
      write_file(p->name, p->map, (size_t)p->size);
      p->written = true;
      fprintf(stderr, "HasVK/WDDM capture: %s written (%llu bytes, copied by the GPU)\n", p->name, (unsigned long long)p->size);
   }
}
/* ---- pass mode ---- */

/* Id of a captured buffer range, captured (or queued for a GPU copy) on first
 * use. Vertex and index buffers are captured whole (they do not change within
 * a frame, and draws bind them at many offsets); uniform ranges one by one at
 * the time of the draw (ring buffers are rewritten during the frame). */
static int pass_buffer(const struct anv_cmd_buffer *cmd, struct anv_buffer *buffer, uint64_t offset, uint64_t size)
{
   for (uint32_t i = 0; i < c.buffer_count; ++i)
      if (c.buffers[i].buffer == buffer && c.buffers[i].offset == offset && c.buffers[i].size == size) return (int)c.buffers[i].id;
   if (c.buffer_count == c.buffer_capacity) {
      c.buffer_capacity = c.buffer_capacity ? c.buffer_capacity * 2 : 256;
      c.buffers = realloc(c.buffers, c.buffer_capacity * sizeof(*c.buffers));
   }
   const uint32_t id = c.buffer_count;
   c.buffers[c.buffer_count++] = (struct buffer_entry){buffer, offset, size, id};
   char what[32], name[32];
   snprintf(what, sizeof(what), "buffer %u", id); snprintf(name, sizeof(name), "buf%u.bin", id);
   capture_memory(c.pass_txt, what, name, cmd, buffer, anv_address_add(buffer->address, offset), offset, size);
   return (int)id;
}

/* A whole buffer, or the bound range when the buffer is too large to keep. */
static void whole_buffer(struct anv_buffer *buffer, uint64_t bound_offset, uint64_t bound_size, uint64_t *offset, uint64_t *size)
{
   if (buffer->vk.size <= (64u << 20)) { *offset = 0; *size = buffer->vk.size; }
   else { *offset = bound_offset; *size = MIN2(bound_size, buffer->vk.size - bound_offset); }
}

static void write_pass_draw(struct anv_cmd_buffer *cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                            int32_t vertex_offset, uint32_t first_instance)
{
   FILE *txt = c.pass_txt;
   mtx_lock(&c.mutex);
   const struct pipeline_entry *entry = pass_pipeline(cmd->state.gfx.pipeline);
   const struct pipeline_entry pentry = entry ? *entry : (struct pipeline_entry){NULL, 0, {0}};
   mtx_unlock(&c.mutex);
   const int pipeline = entry ? (int)pentry.index : -1;
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const struct anv_cmd_pipeline_state *state = &cmd->state.gfx.base;
   ++c.pass_draws;
   /* buffers first, so that the draw block refers to ids already listed */
   int vb_ids[MAX_VBS]; uint64_t vb_base[MAX_VBS];
   for (uint32_t b = 0; b < MAX_VBS; ++b) {
      const struct anv_vertex_binding *vb = &cmd->state.vertex_bindings[b];
      vb_ids[b] = -1;
      if (!vb->buffer) continue;
      uint64_t offset, size; whole_buffer(vb->buffer, vb->offset, vb->size, &offset, &size);
      vb_ids[b] = pass_buffer(cmd, vb->buffer, offset, size); vb_base[b] = offset;
   }
   int ib_id = -1; uint64_t ib_base = 0;
   if (indexed && cmd->state.gfx.index_buffer) {
      uint64_t offset, size;
      whole_buffer(cmd->state.gfx.index_buffer, cmd->state.gfx.index_offset, cmd->state.gfx.index_size, &offset, &size);
      ib_id = pass_buffer(cmd, cmd->state.gfx.index_buffer, offset, size); ib_base = offset;
   }
   /* descriptors: buffer ranges at their effective (dynamic) offsets; images and texel buffers described only */
   char descs[4096]; int n = 0;
   for (uint32_t s = 0; s < MAX_SETS; ++s) {
      const struct anv_descriptor_set *set = state->descriptors[s];
      if (!set) continue;
      for (uint32_t b = 0; b < set->layout->binding_count; ++b) {
         const struct anv_descriptor_set_binding_layout *binding = &set->layout->binding[b];
         for (uint32_t e = 0; e < binding->array_size && e < 16 && n < (int)sizeof(descs) - 128; ++e) {
            const struct anv_descriptor *d = &set->descriptors[binding->descriptor_index + e];
            const bool dynamic = d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            switch (d->type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
               if (!d->buffer) { n += snprintf(descs + n, sizeof(descs) - n, "null %u %u %u %d\n", s, b, e, d->type); break; }
               uint64_t offset = d->offset;
               if (dynamic) offset += state->push_constants.dynamic_offsets[pentry.dynamic_start[s] + binding->dynamic_offset_index + e];
               const uint64_t range = offset < d->buffer->vk.size ? MIN2(d->range, d->buffer->vk.size - offset) : 0;
               const int id = range ? pass_buffer(cmd, d->buffer, offset, range) : -1;
               n += snprintf(descs + n, sizeof(descs) - n, "buf %u %u %u %d %d %llu\n", s, b, e, d->type, id, (unsigned long long)range);
               break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               n += snprintf(descs + n, sizeof(descs) - n, "texel %u %u %u %d %llu\n", s, b, e, d->type,
                             d->buffer_view ? (unsigned long long)d->buffer_view->range : 0ull);
               break;
            default:
               if (d->image_view) {
                  const int image_id = pass_image(cmd, d->image_view->image, d->layout);
                  n += snprintf(descs + n, sizeof(descs) - n, "image %u %u %u %d %d %d %d %u %u %u %u\n", s, b, e, d->type,
                                d->image_view->vk.view_type, d->image_view->vk.format, image_id, d->image_view->vk.base_mip_level,
                                d->image_view->vk.level_count, d->image_view->vk.base_array_layer, d->image_view->vk.layer_count);
               }
               else
                  n += snprintf(descs + n, sizeof(descs) - n, "null %u %u %u %d\n", s, b, e, d->type);
            }
         }
      }
   }
   fprintf(txt, "draw %d %u %u %u %u %d %u\n", pipeline, indexed, count, instances, first, vertex_offset, first_instance);
   fputs(descs, txt);
   fprintf(txt, "viewport %g %g %g %g %g %g\n", dyn->vp.viewports[0].x, dyn->vp.viewports[0].y, dyn->vp.viewports[0].width,
           dyn->vp.viewports[0].height, dyn->vp.viewports[0].minDepth, dyn->vp.viewports[0].maxDepth);
   fprintf(txt, "scissor %d %d %u %u\n", dyn->vp.scissors[0].offset.x, dyn->vp.scissors[0].offset.y, dyn->vp.scissors[0].extent.width,
           dyn->vp.scissors[0].extent.height);
   fprintf(txt, "bias %g %g %g\n", dyn->rs.depth_bias.constant_factor, dyn->rs.depth_bias.clamp, dyn->rs.depth_bias.slope_factor);
   fprintf(txt, "push "); write_hex(txt, state->push_constants.client_data, 128); fprintf(txt, "\n");
   for (uint32_t b = 0; b < MAX_VBS; ++b)
      if (vb_ids[b] >= 0) fprintf(txt, "vb %u %d %llu\n", b, vb_ids[b], (unsigned long long)(cmd->state.vertex_bindings[b].offset - vb_base[b]));
   if (ib_id >= 0) fprintf(txt, "ib %d %llu %u\n", ib_id, (unsigned long long)(cmd->state.gfx.index_offset - ib_base), cmd->state.gfx.index_type);
}

void anv_wddm_capture_pass_begin(struct anv_cmd_buffer *cmd, const VkRenderPassBeginInfo *info)
{
   if (!capture_enabled() || !c.pass_size || c.pass_done || cmd->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return;
   if (info->renderArea.extent.width != c.pass_size || info->renderArea.extent.height != c.pass_size) return;
   const uint64_t now = os_time_get_nano() / 1000000;
   mtx_lock(&c.mutex);
   if (!c.pass_first_ms) c.pass_first_ms = now;
   const bool take = !c.pass_cmd && !c.pass_done && now - c.pass_first_ms >= (uint64_t)c.pass_at_s * 1000;
   if (take) c.pass_cmd = cmd;
   mtx_unlock(&c.mutex);
   if (!take) return;
   c.pass_txt = open_out("pass.txt", "w");
   if (!c.pass_txt) { c.pass_cmd = NULL; return; }
   fprintf(c.pass_txt, "pass %u %u\n", info->renderArea.extent.width, info->renderArea.extent.height);
   VK_FROM_HANDLE(vk_framebuffer, fb, info->framebuffer);
   VK_FROM_HANDLE(vk_render_pass, pass, info->renderPass);
   for (uint32_t i = 0; pass && i < pass->attachment_count; ++i) {
      const struct vk_render_pass_attachment *a = &pass->attachments[i];
      const VkClearValue *clear = i < info->clearValueCount ? &info->pClearValues[i] : NULL;
      uint32_t layers = 1;
      if (fb && !(fb->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && i < fb->attachment_count) {
         ANV_FROM_HANDLE(anv_image_view, view, fb->attachments[i]);
         layers = view->image->vk.array_layers;
      }
      fprintf(c.pass_txt, "attachment %u format %d load %d final %d clear %g %u layers %u\n", i, a->format, a->load_op, a->final_layout,
              clear ? clear->depthStencil.depth : 0.0f, clear ? clear->depthStencil.stencil : 0u, layers);
   }
   fprintf(stderr, "HasVK/WDDM capture: recording a %ux%u render pass\n", c.pass_size, c.pass_size);
}

void anv_wddm_capture_pass_end(struct anv_cmd_buffer *cmd)
{
   if (!c.pass_size || cmd != c.pass_cmd) return;
   fprintf(c.pass_txt, "end_pass draws %u executes %u\n", c.pass_draws, c.pass_executes);
   fclose(c.pass_txt); c.pass_txt = NULL;
   c.pass_cmd = NULL; c.pass_done = true;
   fprintf(stderr, "HasVK/WDDM capture: render pass recorded (%u draws, %u secondary executes, %u buffers, %llu MiB queued for GPU copies)\n",
           c.pass_draws, c.pass_executes, c.buffer_count, (unsigned long long)(c.captured_bytes >> 20));
}

void anv_wddm_capture_execute(struct anv_cmd_buffer *cmd, uint32_t count)
{
   if (c.pass_size && cmd == c.pass_cmd) c.pass_executes += count;
}

void anv_wddm_capture_draw(struct anv_cmd_buffer *cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                           int32_t vertex_offset, uint32_t first_instance)
{
   if (!capture_enabled()) return;
   if (c.pass_size && cmd == c.pass_cmd && c.pass_txt) write_pass_draw(cmd, indexed, count, instances, first, vertex_offset, first_instance);
   if (!c.pipeline || (const void *)cmd->state.gfx.pipeline != c.pipeline) return;
   if (p_atomic_cmpxchg(&c.draw_done, 0, 1) != 0) return;
   FILE *txt = open_out("capture.txt", "a");
   if (!txt) return;
   fprintf(txt, "draw %u %u %u %u %d %u\n", indexed, count, instances, first, vertex_offset, first_instance);
   const struct anv_cmd_pipeline_state *state = &cmd->state.gfx.base;
   write_file("push.bin", state->push_constants.client_data, sizeof(state->push_constants.client_data));
   fprintf(txt, "push push.bin %zu\n", sizeof(state->push_constants.client_data));
   for (uint32_t s = 0; c.layout && s < c.layout->num_sets; ++s) {
      const struct anv_descriptor_set *set = state->descriptors[s];
      if (!set) { fprintf(txt, "set_unbound %u\n", s); continue; }
      const struct anv_descriptor_set_layout *layout = set->layout;
      for (uint32_t b = 0; b < layout->binding_count; ++b) {
         const struct anv_descriptor_set_binding_layout *binding = &layout->binding[b];
         for (uint32_t e = 0; e < binding->array_size && e < 64; ++e) {
            const struct anv_descriptor *d = &set->descriptors[binding->descriptor_index + e];
            char what[96], name[64];
            snprintf(what, sizeof(what), "desc %u %u %u %d", s, b, e, d->type);
            snprintf(name, sizeof(name), "set%u-b%u-e%u.bin", s, b, e);
            switch (d->type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
               if (!d->buffer) { fprintf(txt, "%s null\n", what); break; }
               uint64_t offset = d->offset;
               if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                  offset += state->push_constants.dynamic_offsets[c.layout->set[s].dynamic_offset_start + binding->dynamic_offset_index + e];
               const uint64_t range = MIN2(d->range, d->buffer->vk.size - offset);
               capture_memory(txt, what, name, cmd, d->buffer, anv_address_add(d->buffer->address, offset), offset, range);
               break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
               if (!d->buffer_view) { fprintf(txt, "%s null\n", what); break; }
               capture_memory(txt, what, name, cmd, NULL, d->buffer_view->address, 0, d->buffer_view->range);
               break;
            default:
               if (d->image_view)
                  fprintf(txt, "%s image %d %ux%u\n", what, d->image_view->vk.format, d->image_view->vk.extent.width,
                          d->image_view->vk.extent.height);
               else
                  fprintf(txt, "%s other\n", what);
            }
         }
      }
   }
   for (uint32_t b = 0; b < MAX_VBS; ++b) {
      const struct anv_vertex_binding *vb = &cmd->state.vertex_bindings[b];
      if (!vb->buffer) continue;
      char what[32], name[32]; snprintf(what, sizeof(what), "vbuf %u", b); snprintf(name, sizeof(name), "vb%u.bin", b);
      capture_memory(txt, what, name, cmd, vb->buffer, anv_address_add(vb->buffer->address, vb->offset), vb->offset,
                     MIN2(vb->size, vb->buffer->vk.size - vb->offset));
   }
   if (indexed && cmd->state.gfx.index_buffer) {
      fprintf(txt, "index_type %u\n", cmd->state.gfx.index_type);
      capture_memory(txt, "ibuf", "index.bin", cmd, cmd->state.gfx.index_buffer,
                     anv_address_add(cmd->state.gfx.index_buffer->address, cmd->state.gfx.index_offset), cmd->state.gfx.index_offset,
                     cmd->state.gfx.index_size);
   }
   fclose(txt);
   fprintf(stderr, "HasVK/WDDM capture: draw captured (%s %u x %u)\n", indexed ? "indexed" : "draw", count, instances);
}
