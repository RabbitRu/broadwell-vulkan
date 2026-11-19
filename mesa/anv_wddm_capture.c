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
#include "util/u_atomic.h"
#include "util/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
   int enabled;              /* -1 unknown */
   char dir[512];
   uint32_t vs_instructions;
   const void *pipeline;     /* captured anv_graphics_pipeline */
   const struct anv_pipeline_layout *layout;
   uint32_t draw_done;
   mtx_t mutex;
} c = {.enabled = -1};

static bool capture_enabled(void)
{
   if (c.enabled < 0) {
      const char *dir = getenv("HASVK_WDDM_CAPTURE_DIR"), *inst = getenv("HASVK_WDDM_CAPTURE_VS_INST");
      c.enabled = dir && inst;
      if (c.enabled) {
         snprintf(c.dir, sizeof(c.dir), "%s", dir);
         c.vs_instructions = (uint32_t)strtoul(inst, NULL, 10);
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

static void capture_pipeline(const VkGraphicsPipelineCreateInfo *info, const struct anv_graphics_pipeline *pipeline)
{
   FILE *txt = open_out("capture.txt", "w");
   if (!txt) return;
   for (uint32_t i = 0; i < info->stageCount; ++i) {
      if (info->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) write_stage(txt, &info->pStages[i], "vs.spv", "vs");
      if (info->pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) write_stage(txt, &info->pStages[i], "fs.spv", "fs");
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
   for (uint32_t s = 0; layout && s < layout->num_sets; ++s) {
      const struct anv_descriptor_set_layout *set = layout->set[s].layout;
      for (uint32_t b = 0; set && b < set->binding_count; ++b)
         if (set->binding[b].array_size)
            fprintf(txt, "set %u binding %u type %d count %u stages 0x%x\n", s, b, set->binding[b].type, set->binding[b].array_size,
                    set->shader_stages);
   }
   fclose(txt);
   c.pipeline = pipeline; c.layout = layout;
   fprintf(stderr, "HasVK/WDDM capture: pipeline captured to %s\n", c.dir);
}

void anv_wddm_capture_pipelines(const VkGraphicsPipelineCreateInfo *infos, uint32_t count, const VkPipeline *pipelines)
{
   if (!capture_enabled() || c.pipeline) return;
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
static struct pending pendings[32];
static uint32_t pending_count;

/* Writes size bytes of buffer at offset now if the CPU can read it, else
 * queues a GPU copy; lists the file in capture.txt either way. */
static void capture_memory(FILE *txt, const char *what, const char *name, const struct anv_cmd_buffer *cmd,
                           struct anv_buffer *buffer, struct anv_address address, uint64_t offset, uint64_t size)
{
   if (size > (64u << 20)) size = 64u << 20;
   if (address.bo && address.bo->map) {
      write_file(name, (const char *)address.bo->map + address.offset, (size_t)size);
      fprintf(txt, "%s %s %llu\n", what, name, (unsigned long long)size);
   } else if (buffer && pending_count < ARRAY_SIZE(pendings)) {
      struct pending *p = &pendings[pending_count++];
      *p = (struct pending){.cmd = cmd, .src = anv_buffer_to_handle(buffer), .offset = offset, .size = size};
      snprintf(p->name, sizeof(p->name), "%s", name);
      fprintf(txt, "%s %s %llu\n", what, name, (unsigned long long)size);
   } else {
      fprintf(txt, "%s unmapped %llu\n", what, (unsigned long long)size);
   }
}

void anv_wddm_capture_end_command_buffer(struct anv_cmd_buffer *cmd)
{
   if (!capture_enabled()) return;
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
   const uint64_t now = os_time_get_nano() / 1000000;
   for (uint32_t i = 0; i < pending_count; ++i) {
      struct pending *p = &pendings[i];
      if (!p->recorded || p->written || now - p->recorded_ms < 2000) continue;
      write_file(p->name, p->map, (size_t)p->size);
      p->written = true;
      fprintf(stderr, "HasVK/WDDM capture: %s written (%llu bytes, copied by the GPU)\n", p->name, (unsigned long long)p->size);
   }
}
void anv_wddm_capture_draw(struct anv_cmd_buffer *cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                           int32_t vertex_offset, uint32_t first_instance)
{
   if (!capture_enabled() || !c.pipeline || (const void *)cmd->state.gfx.pipeline != c.pipeline) return;
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
