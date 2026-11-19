/* Replays one draw captured by the driver (HASVK_WDDM_CAPTURE_DIR, see
 * mesa/anv_wddm_capture.c) on this ICD and times it with GPU timestamps, so
 * that compiler or driver variants can be compared in seconds instead of game
 * runs. The pipeline is rebuilt from capture.txt (stages, specialization,
 * vertex input, layout, raster and depth state); buffers get the captured
 * contents; images (not captured) are opaque placeholders. The target is a
 * D16 1536x1536 depth buffer cleared to 0 (the captured shadow pass uses
 * reverse Z).
 * Usage: replay_probe.exe <capture dir> [repeats per pass, default 20]
 *        [vs = vertex stage only | fs = with the fragment stage, default vs]
 *        [replacement vertex SPIR-V, e.g. to measure the fixed-function floor] */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

static std::vector<char> load(const std::string &path)
{
   std::ifstream in(path, std::ios::binary);
   return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static std::vector<uint8_t> unhex(const std::string &s)
{
   std::vector<uint8_t> out;
   for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back((uint8_t)strtoul(s.substr(i, 2).c_str(), nullptr, 16));
   return out;
}

struct Stage { std::string file, entry; std::vector<VkSpecializationMapEntry> map; std::vector<uint8_t> data; };
struct Binding { uint32_t set, binding, type, count, stages; };
struct Desc { uint32_t set, binding, element, type; std::string file; uint64_t size; };

int main(int argc, char **argv)
{
   if (argc < 2) { printf("usage: replay_probe <capture dir> [repeats] [vs|fs]\n"); return 2; }
   const std::string dir = argv[1];
   const int repeats = argc > 2 ? atoi(argv[2]) : 20;
   const bool with_fs = argc > 3 && !strcmp(argv[3], "fs");
   const char *vs_override = argc > 4 ? argv[4] : nullptr;

   /* ---- capture.txt ---- */
   std::ifstream txt(dir + "\\capture.txt");
   if (!txt) { printf("no capture.txt in %s\n", dir.c_str()); return 2; }
   std::map<std::string, Stage> stages;
   std::vector<VkVertexInputBindingDescription> vbs;
   std::vector<VkVertexInputAttributeDescription> attrs;
   std::vector<Binding> bindings;
   std::vector<Desc> descs;
   std::map<uint32_t, std::pair<std::string, uint64_t>> vbufs;
   std::string ibuf, push_file;
   uint32_t topology = 3, index_type = 1;
   uint32_t cull = 0, front = 0, polygon = 0, bias_enable = 0, clamp = 0;
   float bias_const = 0, bias_slope = 0, bias_clamp = 0;
   uint32_t depth_test = 1, depth_write = 1, depth_op = 6;
   uint32_t indexed = 0, count = 0, instances = 1, first = 0, first_instance = 0; int32_t vertex_offset = 0;
   for (std::string line; std::getline(txt, line);) {
      std::istringstream s(line); std::string key; s >> key;
      if (key == "vs_module" || key == "fs_module") { Stage &st = stages[key.substr(0, 2)]; s >> st.file >> st.entry; }
      else if (key == "vs_spec" || key == "fs_spec") { VkSpecializationMapEntry e; size_t size; s >> e.constantID >> e.offset >> size; e.size = size; stages[key.substr(0, 2)].map.push_back(e); }
      else if (key == "vs_spec_data" || key == "fs_spec_data") { std::string hex; s >> hex; stages[key.substr(0, 2)].data = unhex(hex); }
      else if (key == "vb_binding") { VkVertexInputBindingDescription b; uint32_t rate; s >> b.binding >> b.stride >> rate; b.inputRate = (VkVertexInputRate)rate; vbs.push_back(b); }
      else if (key == "vb_attr") { VkVertexInputAttributeDescription a; int fmt; s >> a.location >> a.binding >> fmt >> a.offset; a.format = (VkFormat)fmt; attrs.push_back(a); }
      else if (key == "topology") s >> topology;
      else if (key == "raster") s >> cull >> front >> polygon >> bias_enable >> bias_const >> bias_slope >> bias_clamp >> clamp;
      else if (key == "depth") s >> depth_test >> depth_write >> depth_op;
      else if (key == "set") { Binding b; std::string w; s >> b.set >> w >> b.binding >> w >> b.type >> w >> b.count >> w >> std::hex >> b.stages; bindings.push_back(b); }
      else if (key == "draw") s >> indexed >> count >> instances >> first >> vertex_offset >> first_instance;
      else if (key == "push") s >> push_file;
      else if (key == "desc") { Desc d; s >> d.set >> d.binding >> d.element >> d.type >> d.file >> d.size; descs.push_back(d); }
      else if (key == "vbuf") { uint32_t b; std::string f; uint64_t size; s >> b >> f >> size; vbufs[b] = {f, size}; }
      else if (key == "ibuf") { uint64_t size; s >> ibuf >> size; }
      else if (key == "index_type") s >> index_type;
   }
   if (!stages.count("vs") || !count) { printf("capture.txt lacks the vertex stage or the draw\n"); return 2; }

   /* ---- device ---- */
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   VkPhysicalDeviceProperties props; ((PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties"))(pd, &props);
   VkPhysicalDeviceFeatures features{}; ((PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures"))(pd, &features);
   VkPhysicalDeviceFeatures enable{}; enable.depthClamp = features.depthClamp; enable.depthBiasClamp = features.depthBiasClamp;
   enable.pipelineStatisticsQuery = features.pipelineStatisticsQuery; enable.textureCompressionBC = features.textureCompressionBC;
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q; dci.pEnabledFeatures = &enable;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) auto name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory); F(vkGetBufferMemoryRequirements);
   F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateGraphicsPipelines);
   F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets);
   F(vkCmdPushConstants); F(vkCmdDrawIndexed); F(vkCmdDraw); F(vkCreateQueryPool); F(vkCmdResetQueryPool); F(vkCmdWriteTimestamp);
   F(vkGetQueryPoolResults); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateRenderPass); F(vkCreateFramebuffer); F(vkCmdBeginRenderPass);
   F(vkCmdEndRenderPass); F(vkCreateImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory); F(vkCreateImageView); F(vkCreateSampler);
   F(vkCmdBindVertexBuffers); F(vkCmdBindIndexBuffer); F(vkCmdSetViewport); F(vkCmdSetScissor); F(vkCmdSetDepthBias); F(vkCmdPipelineBarrier);
   F(vkCmdClearColorImage); F(vkCmdBeginQuery); F(vkCmdEndQuery);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkPhysicalDeviceMemoryProperties mprops;
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mprops);
   /* REPLAY_DEVICE_LOCAL=1: vertex, index and uniform buffers in a memory
    * type without HOST_VISIBLE (as the game allocates them), filled by a GPU
    * copy from a host-visible staging buffer. */
   int device_local_type = -1;
   for (uint32_t i = 0; i < mprops.memoryTypeCount; ++i) {
      if (getenv("REPLAY_LIST_MEMORY")) printf("memory type %u: flags 0x%x heap %u\n", i, mprops.memoryTypes[i].propertyFlags, mprops.memoryTypes[i].heapIndex);
      if (device_local_type < 0 && (mprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
          !(mprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         device_local_type = (int)i;
   }
   const bool use_device_local = getenv("REPLAY_DEVICE_LOCAL") && device_local_type >= 0;
   if (getenv("REPLAY_DEVICE_LOCAL") && device_local_type < 0) printf("no device-local-only memory type\n");

   struct Upload { VkBuffer src, dst; uint64_t size; };
   std::vector<Upload> uploads;
   auto make_buffer = [&](const std::vector<char> &data, uint64_t size, VkBufferUsageFlags usage, VkBuffer *buffer) -> VkResult {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size ? size : 16; bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkResult r = vkCreateBuffer(d, &bi, nullptr, buffer); if (r) return r;
      VkMemoryRequirements req; vkGetBufferMemoryRequirements(d, *buffer, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; VkDeviceMemory mem;
      if (use_device_local && (req.memoryTypeBits & (1u << device_local_type))) {
         ai.memoryTypeIndex = (uint32_t)device_local_type;
         r = vkAllocateMemory(d, &ai, nullptr, &mem); if (r) return r;
         r = vkBindBufferMemory(d, *buffer, mem, 0); if (r) return r;
         Upload u{VK_NULL_HANDLE, *buffer, bi.size};
         VkBufferCreateInfo si = bi; si.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
         r = vkCreateBuffer(d, &si, nullptr, &u.src); if (r) return r;
         vkGetBufferMemoryRequirements(d, u.src, &req); ai.allocationSize = req.size; ai.memoryTypeIndex = 0;
         r = vkAllocateMemory(d, &ai, nullptr, &mem); if (r) return r;
         r = vkBindBufferMemory(d, u.src, mem, 0); if (r) return r;
         uploads.push_back(u);
      } else {
         r = vkAllocateMemory(d, &ai, nullptr, &mem); if (r) return r;
         r = vkBindBufferMemory(d, *buffer, mem, 0); if (r) return r;
      }
      void *map; r = vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &map); if (r) return r;
      memset(map, 0, bi.size); memcpy(map, data.data(), std::min<size_t>(data.size(), bi.size));
      return VK_SUCCESS;
   };

   /* ---- layout ---- */
   uint32_t set_count = 0;
   for (auto &b : bindings) set_count = std::max(set_count, b.set + 1);
   std::vector<VkDescriptorSetLayout> set_layouts(set_count);
   for (uint32_t s = 0; s < set_count; ++s) {
      std::vector<VkDescriptorSetLayoutBinding> lb;
      for (auto &b : bindings) if (b.set == s) lb.push_back({b.binding, (VkDescriptorType)b.type, b.count, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = (uint32_t)lb.size(); li.pBindings = lb.data();
      CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layouts[s]));
   }
   VkPushConstantRange push_range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = set_count; pli.pSetLayouts = set_layouts.data();
   pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &push_range;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));

   /* ---- descriptors ---- */
   VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 8; dpi.poolSizeCount = 4; dpi.pPoolSizes = sizes;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   std::vector<VkDescriptorSet> sets(set_count);
   if (set_count) {
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = set_count;
      dai.pSetLayouts = set_layouts.data(); CHECK(vkAllocateDescriptorSets(d, &dai, sets.data()));
   }
   /* Placeholder texture: 256x256 RGBA8, opaque. */
   VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
   ii.extent = {256, 256, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
   ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   VkImage texture; CHECK(vkCreateImage(d, &ii, nullptr, &texture));
   { VkMemoryRequirements req; vkGetImageMemoryRequirements(d, texture, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
     ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, texture, mem, 0)); }
   VkImageViewCreateInfo tvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; tvi.image = texture; tvi.viewType = VK_IMAGE_VIEW_TYPE_2D; tvi.format = ii.format;
   tvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageView texture_view; CHECK(vkCreateImageView(d, &tvi, nullptr, &texture_view));
   VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sci.maxLod = 16;
   VkSampler sampler; CHECK(vkCreateSampler(d, &sci, nullptr, &sampler));
   std::vector<VkBuffer> keep;
   std::vector<uint32_t> dynamic_offsets;
   for (auto &b : bindings) {
      for (uint32_t e = 0; e < b.count && e < 64; ++e) {
         const Desc *desc = nullptr;
         for (auto &x : descs) if (x.set == b.set && x.binding == b.binding && x.element == e) desc = &x;
         VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = sets[b.set]; w.dstBinding = b.binding; w.dstArrayElement = e;
         w.descriptorCount = 1; w.descriptorType = (VkDescriptorType)b.type;
         VkDescriptorBufferInfo bi{}; VkDescriptorImageInfo ii2{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
             b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
            std::vector<char> data; uint64_t size = 256;
            if (desc && desc->file != "unmapped" && desc->file != "null") { data = load(dir + "\\" + desc->file); size = desc->size; }
            VkBuffer buffer; CHECK(make_buffer(data, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buffer));
            keep.push_back(buffer); bi = {buffer, 0, size ? size : 16}; w.pBufferInfo = &bi;
            if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) dynamic_offsets.push_back(0);
         } else if (b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            w.pImageInfo = &ii2;
         } else {
            continue;
         }
         vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
      }
   }

   /* ---- vertex and index buffers ---- */
   std::vector<VkBuffer> vbuffers; std::vector<VkDeviceSize> voffsets; std::vector<uint32_t> vbindings;
   for (auto &vb : vbs) {
      std::vector<char> data; uint64_t size = 1 << 20;
      auto it = vbufs.find(vb.binding);
      if (it != vbufs.end() && it->second.first != "unmapped") { data = load(dir + "\\" + it->second.first); size = it->second.second; }
      VkBuffer buffer; CHECK(make_buffer(data, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &buffer));
      vbuffers.push_back(buffer); voffsets.push_back(0); vbindings.push_back(vb.binding);
   }
   VkBuffer index_buffer = VK_NULL_HANDLE;
   if (indexed) { std::vector<char> data = load(dir + "\\" + ibuf); CHECK(make_buffer(data, data.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &index_buffer)); }
   std::vector<char> push = load(dir + "\\" + push_file); push.resize(128);

   /* ---- render pass and pipeline ---- */
   const uint32_t size = 1536;
   VkImageCreateInfo di = ii; di.format = VK_FORMAT_D16_UNORM; di.extent = {size, size, 1};
   di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImage depth; CHECK(vkCreateImage(d, &di, nullptr, &depth));
   { VkMemoryRequirements req; vkGetImageMemoryRequirements(d, depth, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
     ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, depth, mem, 0)); }
   VkImageViewCreateInfo dvi = tvi; dvi.image = depth; dvi.format = di.format; dvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   VkImageView depth_view; CHECK(vkCreateImageView(d, &dvi, nullptr, &depth_view));
   VkAttachmentDescription att{0, VK_FORMAT_D16_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.pDepthStencilAttachment = &ref;
   VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &att; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
   VkRenderPass pass; CHECK(vkCreateRenderPass(d, &rpi, nullptr, &pass));
   VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &depth_view;
   fbi.width = fbi.height = size; fbi.layers = 1;
   VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(d, &fbi, nullptr, &framebuffer));
   std::vector<VkPipelineShaderStageCreateInfo> pstages;
   std::vector<VkSpecializationInfo> specs(2);
   std::vector<std::vector<char>> codes;
   for (const char *name : {"vs", "fs"}) {
      if (!stages.count(name) || stages[name].file.empty() || stages[name].file == "none") continue;
      if (!strcmp(name, "fs") && !with_fs) continue;
      Stage &st = stages[name];
      const bool replaced = vs_override && !strcmp(name, "vs");
      codes.push_back(load(replaced ? std::string(vs_override) : dir + "\\" + st.file));
      VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = codes.back().size(); smi.pCode = (const uint32_t *)codes.back().data();
      VkShaderModule module; CHECK(vkCreateShaderModule(d, &smi, nullptr, &module));
      VkSpecializationInfo &spec = specs[pstages.size()];
      spec = {(uint32_t)st.map.size(), st.map.data(), st.data.size(), st.data.data()};
      pstages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         !strcmp(name, "vs") ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, module, replaced ? "main" : st.entry.c_str(),
                         st.map.empty() || replaced ? nullptr : &spec});
   }
   VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   vin.vertexBindingDescriptionCount = (uint32_t)vbs.size(); vin.pVertexBindingDescriptions = vbs.data();
   vin.vertexAttributeDescriptionCount = (uint32_t)attrs.size(); vin.pVertexAttributeDescriptions = attrs.data();
   VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = (VkPrimitiveTopology)topology;
   VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
   VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
   rs.cullMode = cull; rs.frontFace = (VkFrontFace)front; rs.polygonMode = (VkPolygonMode)polygon; rs.depthBiasEnable = bias_enable;
   rs.depthClampEnable = clamp && enable.depthClamp; rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
   ds.depthTestEnable = depth_test; ds.depthWriteEnable = depth_write; ds.depthCompareOp = (VkCompareOp)depth_op;
   /* Experiment switches for the raster/depth back end. */
   if (getenv("REPLAY_NO_DEPTH_WRITE")) ds.depthWriteEnable = VK_FALSE;
   if (getenv("REPLAY_NO_DEPTH_TEST")) ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE;
   if (getenv("REPLAY_NO_BIAS")) rs.depthBiasEnable = VK_FALSE;
   if (getenv("REPLAY_CULL_BACK")) rs.cullMode = VK_CULL_MODE_BACK_BIT;
   VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
   VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dsi.dynamicStateCount = 3; dsi.pDynamicStates = dyn;
   VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = (uint32_t)pstages.size(); gpi.pStages = pstages.data();
   gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
   gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dsi; gpi.layout = layout; gpi.renderPass = pass;
   VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
   /* REPLAY_ALTERNATE=<vertex SPIR-V | same>: a small draw (300 indices, one
    * instance) between the replayed draws, with a second pipeline built from
    * that shader, or with the replayed pipeline itself ("same"). Measures
    * what a pipeline switch costs on the GPU. */
   const char *alternate = getenv("REPLAY_ALTERNATE");
   VkPipeline alt_pipeline = pipeline;
   if (alternate && strcmp(alternate, "same")) {
      std::vector<char> code = load(alternate);
      VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size(); smi.pCode = (const uint32_t *)code.data();
      VkShaderModule module; CHECK(vkCreateShaderModule(d, &smi, nullptr, &module));
      VkPipelineShaderStageCreateInfo alt_stage = pstages[0]; alt_stage.module = module; alt_stage.pName = "main"; alt_stage.pSpecializationInfo = nullptr;
      VkGraphicsPipelineCreateInfo alt = gpi; alt.stageCount = 1; alt.pStages = &alt_stage;
      CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &alt, nullptr, &alt_pipeline));
   }

   /* ---- run ---- */
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 4;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   VkQueryPoolCreateInfo si{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; si.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; si.queryCount = 1;
   si.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                           VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
   VkQueryPool sp; CHECK(vkCreateQueryPool(d, &si, nullptr, &sp));
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
   {  /* the placeholder texture: opaque white */
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = texture; b.subresourceRange = tvi.subresourceRange;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkClearColorValue white{}; for (float &f : white.float32) f = 1.0f;
      vkCmdClearColorImage(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &b.subresourceRange);
      F(vkCmdCopyBuffer);
      for (auto &u : uploads) { VkBufferCopy region{0, 0, u.size}; vkCmdCopyBuffer(cmd, u.src, u.dst, 1, &region); }
      if (!uploads.empty()) {
         VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      }
      b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      CHECK(vkEndCommandBuffer(cmd)); CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
   }
   double best = 1e9, best_outside = 0; uint64_t stats[4] = {};
   for (int rep = -10; rep < 30; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0)); CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, 4); vkCmdResetQueryPool(cmd, sp, 0, 1);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2);
      VkClearValue clear{}; clear.depthStencil.depth = 0.0f;
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass; rbi.framebuffer = framebuffer;
      rbi.renderArea = {{0, 0}, {size, size}}; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (set_count) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set_count, sets.data(),
                                             (uint32_t)dynamic_offsets.size(), dynamic_offsets.data());
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, push.data());
      for (size_t i = 0; i < vbuffers.size(); ++i) vkCmdBindVertexBuffers(cmd, vbindings[i], 1, &vbuffers[i], &voffsets[i]);
      if (indexed) vkCmdBindIndexBuffer(cmd, index_buffer, 0, index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      VkViewport viewport{0, 0, (float)size, (float)size, 0, 1}; VkRect2D scissor{{0, 0}, {size, size}};
      if (getenv("REPLAY_SCISSOR_1X1")) scissor.extent = {1, 1}; /* removes rasterization and depth work */
      vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor); if (const char *b = getenv("REPLAY_BIAS")) sscanf(b, "%f,%f,%f", &bias_const, &bias_slope, &bias_clamp); /* constant,slope,clamp */
      vkCmdSetDepthBias(cmd, bias_const, bias_clamp, bias_slope);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
      vkCmdBeginQuery(cmd, sp, 0, 0);
      for (int i = 0; i < repeats; ++i) {
         /* Per-draw state the game changes between draws: push constants,
          * descriptor sets (with dynamic offsets) and vertex buffers. */
         if (getenv("REPLAY_REPUSH")) { push[0] ^= 1; vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, push.data()); }
         if (getenv("REPLAY_REBIND_SETS") && set_count)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set_count, sets.data(), (uint32_t)dynamic_offsets.size(), dynamic_offsets.data());
         if (getenv("REPLAY_REBIND_VB")) {
            std::vector<VkDeviceSize> shifted(voffsets.size(), 0);
            for (size_t v = 0; v < vbuffers.size(); ++v) { shifted[v] = (i & 1) ? 0 : 0; vkCmdBindVertexBuffers(cmd, vbindings[v], 1, &vbuffers[v], &shifted[v]); }
            if (indexed) vkCmdBindIndexBuffer(cmd, index_buffer, 0, index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
         }
         if (indexed) vkCmdDrawIndexed(cmd, count, instances, first, vertex_offset, first_instance);
         else vkCmdDraw(cmd, count, instances, first, first_instance);
         if (alternate) {
            if (alt_pipeline != pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alt_pipeline);
            if (indexed) vkCmdDrawIndexed(cmd, 300, 1, first, vertex_offset, first_instance);
            else vkCmdDraw(cmd, 300, 1, first, first_instance);
            if (alt_pipeline != pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
         }
      }
      vkCmdEndQuery(cmd, sp, 0);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      vkCmdEndRenderPass(cmd);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 3);
      CHECK(vkEndCommandBuffer(cmd)); CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      uint64_t ts[4]; CHECK(vkGetQueryPoolResults(d, qp, 0, 4, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      CHECK(vkGetQueryPoolResults(d, sp, 0, 1, sizeof(stats), stats, sizeof(stats), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      const double outside = ((ts[3] - ts[2]) - (ts[1] - ts[0])) * props.limits.timestampPeriod / 1e6;
      if (rep >= 0 && ms < best) { best = ms; best_outside = outside; }
   }
   printf("replay %s: %.4f ms per draw (best of 30 passes x %d draws); per draw: %llu IA vertices, %llu VS, %llu clip out, %llu FS\n",
          with_fs ? "vs+fs" : "vs only", best / repeats, repeats, (unsigned long long)(stats[0] / repeats), (unsigned long long)(stats[1] / repeats),
          (unsigned long long)(stats[2] / repeats), (unsigned long long)(stats[3] / repeats));
   printf("render pass begin + end (clear, resolve): %.4f ms\n", best_outside);
   return 0;
}
