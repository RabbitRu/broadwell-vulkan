/* Replays a whole render pass captured by the driver (HASVK_WDDM_CAPTURE_PASS,
 * see mesa/anv_wddm_capture.c on branch debug: pipelines.txt, pass.txt, p<i>-*.spv,
 * buf<i>.bin) on this ICD and times it with GPU timestamps, so that what a
 * game's pass costs can be taken apart without running the game.
 * Pipelines are rebuilt from their create info; every draw gets its captured
 * pipeline, push constants, buffer ranges (at offset 0 of the captured range),
 * vertex/index buffers and dynamic state. Images are not captured: sampled
 * images are opaque white placeholders, so alpha tests pass everywhere.
 * Usage: replay_pass_probe.exe <capture dir> [passes, default 20]
 * Switches (environment):
 *   REPLAY_NO_FS=1           pipelines without their fragment stage
 *   REPLAY_SKIP=a,b,...      leave out the draws of these pipelines
 *   REPLAY_ONLY=a,b,...      only the draws of these pipelines
 *   REPLAY_PER_DRAW=1        also time every draw alone (sums by pipeline)
 *   REPLAY_LIST=<n>          with REPLAY_PER_DRAW: the n slowest draws, with samples passed
 *   REPLAY_SORT=1            draws grouped by pipeline (first use order)
 *   REPLAY_NO_DEPTH_BIAS=1   depth bias off
 *   REPLAY_NO_TEXTURES=1     placeholders instead of the captured textures
 *   REPLAY_SCISSOR_1X1=1     1x1 scissor: vertex work only (dynamic scissor)
 *   REPLAY_VS_<i>=<spv>      replacement vertex shader for pipeline i
 *   REPLAY_LAYERS=<n>        depth image array layers (default: as captured)
 *   REPLAY_REPEAT_FIRST=1    the first draw twice (the second is timed in the list as draw 0) */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

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
static std::set<int> list_env(const char *name)
{
   std::set<int> out;
   if (const char *v = getenv(name)) { std::stringstream s(v); std::string item; while (std::getline(s, item, ',')) out.insert(atoi(item.c_str())); }
   return out;
}

struct Stage { std::string file, entry; std::vector<VkSpecializationMapEntry> map; std::vector<uint8_t> data; VkSpecializationInfo spec{}; };
struct Binding { uint32_t set, binding, type, count; };
struct Pipeline {
   std::map<std::string, Stage> stages;
   std::vector<VkVertexInputBindingDescription> vbs;
   std::vector<VkVertexInputAttributeDescription> attrs;
   std::vector<Binding> bindings;
   std::vector<VkDynamicState> dynamic;
   uint32_t set_count = 0, topology = 3, cull = 0, front = 0, polygon = 0, bias = 0, clamp = 0, depth_test = 1, depth_write = 1, depth_op = 6;
   float bias_const = 0, bias_slope = 0, bias_clamp = 0;
   std::vector<VkDescriptorSetLayout> set_layouts;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   bool has(VkDynamicState s) const { return std::find(dynamic.begin(), dynamic.end(), s) != dynamic.end(); }
};
struct Desc { std::string kind; uint32_t set, binding, element, type; int id; uint64_t range; int view_type;
              int format = 0, image_id = -1; uint32_t base_mip = 0, levels = 1, base_layer = 0, layer_count = 1; };
struct ImageData { std::string file; int format; uint32_t width, height, depth, mips, layers, type; uint64_t size; };
struct Draw {
   int pipeline; uint32_t indexed, count, instances, first, first_instance; int32_t vertex_offset;
   VkViewport viewport{}; VkRect2D scissor{}; float bias_const = 0, bias_clamp = 0, bias_slope = 0;
   std::vector<uint8_t> push; std::vector<Desc> descs;
   std::vector<std::pair<uint32_t, std::pair<int, uint64_t>>> vbs;  /* binding -> (buffer id, offset) */
   int ib = -1; uint64_t ib_offset = 0; uint32_t ib_type = 1;
   std::vector<VkDescriptorSet> sets; std::vector<uint32_t> dynamic_offsets;
};

int main(int argc, char **argv)
{
   if (argc < 2) { printf("usage: replay_pass_probe <capture dir> [passes]\n"); return 2; }
   const std::string dir = argv[1];
   const int passes = argc > 2 ? atoi(argv[2]) : 20;
   const bool no_fs = getenv("REPLAY_NO_FS") != nullptr;
   const std::set<int> skip = list_env("REPLAY_SKIP"), only = list_env("REPLAY_ONLY");

   /* ---- pipelines.txt ---- */
   std::map<int, Pipeline> pipelines;
   {
      std::ifstream txt(dir + "\\pipelines.txt");
      if (!txt) { printf("no pipelines.txt\n"); return 2; }
      Pipeline *p = nullptr;
      for (std::string line; std::getline(txt, line);) {
         std::istringstream s(line); std::string key; s >> key;
         if (key == "pipeline") { int i; s >> i; p = &pipelines[i]; continue; }
         if (!p) continue;
         if (key == "end") p = nullptr;
         else if (key == "vs_module" || key == "fs_module") { Stage &st = p->stages[key.substr(0, 2)]; s >> st.file >> st.entry; }
         else if (key == "vs_spec" || key == "fs_spec") { VkSpecializationMapEntry e; size_t size; s >> e.constantID >> e.offset >> size; e.size = size; p->stages[key.substr(0, 2)].map.push_back(e); }
         else if (key == "vs_spec_data" || key == "fs_spec_data") { std::string hex; s >> hex; p->stages[key.substr(0, 2)].data = unhex(hex); }
         else if (key == "vb_binding") { VkVertexInputBindingDescription b; uint32_t rate; s >> b.binding >> b.stride >> rate; b.inputRate = (VkVertexInputRate)rate; p->vbs.push_back(b); }
         else if (key == "vb_attr") { VkVertexInputAttributeDescription a; int fmt; s >> a.location >> a.binding >> fmt >> a.offset; a.format = (VkFormat)fmt; p->attrs.push_back(a); }
         else if (key == "topology") s >> p->topology;
         else if (key == "raster") s >> p->cull >> p->front >> p->polygon >> p->bias >> p->bias_const >> p->bias_slope >> p->bias_clamp >> p->clamp;
         else if (key == "depth") s >> p->depth_test >> p->depth_write >> p->depth_op;
         else if (key == "dynamic") { int d; s >> d; p->dynamic.push_back((VkDynamicState)d); }
         else if (key == "set") { Binding b; std::string w; s >> b.set >> w >> b.binding >> w >> b.type >> w >> b.count; p->bindings.push_back(b); }
         else if (key == "set_count") s >> p->set_count;
      }
   }
   /* ---- pass.txt ---- */
   std::map<int, std::pair<std::string, uint64_t>> buffer_files;
   std::map<int, ImageData> image_data;
   std::vector<Draw> draws;
   uint32_t width = 1536, height = 1536, layers = 1; int format = VK_FORMAT_D16_UNORM, final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
   float clear_depth = 0;
   {
      std::ifstream txt(dir + "\\pass.txt");
      if (!txt) { printf("no pass.txt\n"); return 2; }
      Draw *d = nullptr;
      for (std::string line; std::getline(txt, line);) {
         std::istringstream s(line); std::string key; s >> key;
         if (key == "pass") s >> width >> height;
         else if (key == "attachment") { std::string w; uint32_t i, stencil; s >> i >> w >> format >> w >> w >> w >> final_layout >> w >> clear_depth >> stencil >> w >> layers; }
         else if (key == "image_data") {
            int id; ImageData im; std::string w; s >> id >> im.file >> w >> im.format >> w >> im.width >> im.height >> im.depth >> w >> im.mips >> w >> im.layers >> w >> im.type >> w >> im.size;
            image_data[id] = im;
         }
         else if (key == "buffer") { int id; std::string f; uint64_t size; s >> id >> f >> size; buffer_files[id] = {f, size}; }
         else if (key == "draw") { draws.emplace_back(); d = &draws.back(); s >> d->pipeline >> d->indexed >> d->count >> d->instances >> d->first >> d->vertex_offset >> d->first_instance; }
         else if (!d) continue;
         else if (key == "viewport") s >> d->viewport.x >> d->viewport.y >> d->viewport.width >> d->viewport.height >> d->viewport.minDepth >> d->viewport.maxDepth;
         else if (key == "scissor") s >> d->scissor.offset.x >> d->scissor.offset.y >> d->scissor.extent.width >> d->scissor.extent.height;
         else if (key == "bias") s >> d->bias_const >> d->bias_clamp >> d->bias_slope;
         else if (key == "push") { std::string hex; s >> hex; d->push = unhex(hex); }
         else if (key == "buf") { Desc x{key}; s >> x.set >> x.binding >> x.element >> x.type >> x.id >> x.range; d->descs.push_back(x); }
         else if (key == "image") {
            Desc x{key}; s >> x.set >> x.binding >> x.element >> x.type >> x.view_type >> x.format;
            if (!(s >> x.image_id)) x.image_id = -1; else s >> x.base_mip >> x.levels >> x.base_layer >> x.layer_count;
            d->descs.push_back(x);
         }
         else if (key == "texel" || key == "null") { Desc x{key}; s >> x.set >> x.binding >> x.element >> x.type; d->descs.push_back(x); }
         else if (key == "vb") { uint32_t b; int id; uint64_t off; s >> b >> id >> off; d->vbs.push_back({b, {id, off}}); }
         else if (key == "ib") s >> d->ib >> d->ib_offset >> d->ib_type;
      }
   }
   std::vector<Draw *> order;
   for (auto &d : draws) {
      if (only.size() && !only.count(d.pipeline)) continue;
      if (skip.count(d.pipeline)) continue;
      if (!pipelines.count(d.pipeline)) continue;
      order.push_back(&d);
   }
   if (getenv("REPLAY_SORT")) {
      std::map<int, int> first_use;
      for (size_t i = 0; i < order.size(); ++i) first_use.emplace(order[i]->pipeline, (int)i);
      std::stable_sort(order.begin(), order.end(), [&](Draw *a, Draw *b) { return first_use[a->pipeline] < first_use[b->pipeline]; });
   }

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
   F(vkCmdClearColorImage); F(vkCmdBeginQuery); F(vkCmdEndQuery); F(vkCmdCopyBufferToImage);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);

   auto make_buffer = [&](const std::vector<char> &data, uint64_t size, VkBuffer *buffer) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = std::max<uint64_t>(size, 256);
      bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      CHECK(vkCreateBuffer(d, &bi, nullptr, buffer));
      VkMemoryRequirements req; vkGetBufferMemoryRequirements(d, *buffer, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; VkDeviceMemory mem;
      CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindBufferMemory(d, *buffer, mem, 0));
      void *map; CHECK(vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &map));
      memset(map, 0, bi.size); memcpy(map, data.data(), std::min<size_t>(data.size(), bi.size));
   };
   std::map<int, VkBuffer> buffers;
   auto get_buffer = [&](int id) -> VkBuffer {
      auto it = buffers.find(id);
      if (it != buffers.end()) return it->second;
      std::vector<char> data; uint64_t size = 256;
      auto f = buffer_files.find(id);
      if (f != buffer_files.end()) { size = f->second.second; if (f->second.first != "unmapped") data = load(dir + "\\" + f->second.first); }
      VkBuffer b; make_buffer(data, size, &b); buffers[id] = b; return b;
   };
   VkBuffer dummy; make_buffer({}, 65536, &dummy);

   /* placeholders: opaque white 2D and 2D-array textures */
   auto make_image = [&](VkImageViewType type, VkImage *image, VkImageView *view) {
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
      ii.extent = {16, 16, 1}; ii.mipLevels = 1; ii.arrayLayers = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.flags = type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      CHECK(vkCreateImage(d, &ii, nullptr, image));
      VkMemoryRequirements req; vkGetImageMemoryRequirements(d, *image, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, *image, mem, 0));
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = *image; vi.viewType = type; vi.format = ii.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers};
      CHECK(vkCreateImageView(d, &vi, nullptr, view));
   };
   VkImage images[3]; VkImageView views[3];
   const VkImageViewType view_types[3] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE};
   for (int i = 0; i < 3; ++i) make_image(view_types[i], &images[i], &views[i]);
   auto view_for = [&](int type) { return type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? views[1] : type == VK_IMAGE_VIEW_TYPE_CUBE ? views[2] : views[0]; };
   /* Captured textures (REPLAY_NO_TEXTURES=1: placeholders only), uploaded below. */
   const bool use_textures = !getenv("REPLAY_NO_TEXTURES");
   struct RealImage { VkImage image; VkBuffer staging; std::vector<VkBufferImageCopy> regions; };
   std::map<int, RealImage> real_images; std::map<std::string, VkImageView> real_views;
   auto real_view = [&](const Desc &x) -> VkImageView {
      auto it = image_data.find(x.image_id);
      if (!use_textures || x.image_id < 0 || it == image_data.end()) return view_for(x.view_type);
      const ImageData &im = it->second;
      if (!real_images.count(x.image_id)) {
         std::vector<char> data = load(dir + "\\" + im.file);
         if (data.size() < im.size) return view_for(x.view_type);
         RealImage r{};
         VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = (VkImageType)im.type; ii.format = (VkFormat)im.format;
         ii.extent = {im.width, im.height, im.depth}; ii.mipLevels = im.mips; ii.arrayLayers = im.layers; ii.samples = VK_SAMPLE_COUNT_1_BIT;
         ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
         if (im.type == VK_IMAGE_TYPE_2D && im.layers >= 6 && im.width == im.height) ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
         CHECK(vkCreateImage(d, &ii, nullptr, &r.image));
         VkMemoryRequirements req; vkGetImageMemoryRequirements(d, r.image, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
         ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, r.image, mem, 0));
         make_buffer(data, im.size, &r.staging);
         /* block size from the packed size of the top level and the extents */
         uint64_t texels_total = 0;
         for (uint32_t l = 0; l < im.mips; ++l) {
            const uint32_t w = std::max(1u, im.width >> l), h = std::max(1u, im.height >> l), dd = std::max(1u, im.depth >> l);
            const bool bc = im.format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && im.format <= VK_FORMAT_BC7_SRGB_BLOCK;
            texels_total += (uint64_t)(bc ? ((w + 3) / 4) * ((h + 3) / 4) : (uint64_t)w * h) * dd * im.layers;
         }
         const uint64_t unit = texels_total ? im.size / texels_total : 4;   /* bytes per block or texel */
         uint64_t offset = 0;
         for (uint32_t l = 0; l < im.mips; ++l) {
            const uint32_t w = std::max(1u, im.width >> l), h = std::max(1u, im.height >> l), dd = std::max(1u, im.depth >> l);
            const bool bc = im.format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && im.format <= VK_FORMAT_BC7_SRGB_BLOCK;
            VkBufferImageCopy region{}; region.bufferOffset = offset; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, im.layers};
            region.imageExtent = {w, h, dd}; r.regions.push_back(region);
            offset += (uint64_t)(bc ? ((w + 3) / 4) * ((h + 3) / 4) : (uint64_t)w * h) * dd * im.layers * unit;
         }
         real_images[x.image_id] = r;
      }
      char key[96]; snprintf(key, sizeof(key), "%d/%d/%d/%u/%u/%u/%u", x.image_id, x.view_type, x.format, x.base_mip, x.levels, x.base_layer, x.layer_count);
      auto v = real_views.find(key);
      if (v != real_views.end()) return v->second;
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = real_images[x.image_id].image; vi.viewType = (VkImageViewType)x.view_type;
      vi.format = (VkFormat)x.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, x.base_mip, x.levels, x.base_layer, x.layer_count};
      VkImageView view; CHECK(vkCreateImageView(d, &vi, nullptr, &view)); real_views[key] = view; return view;
   };
   VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sci.maxLod = 16;
   VkSampler sampler; CHECK(vkCreateSampler(d, &sci, nullptr, &sampler));

   /* ---- render pass ---- */
   if (getenv("REPLAY_LAYERS")) layers = (uint32_t)atoi(getenv("REPLAY_LAYERS"));
   VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; di.imageType = VK_IMAGE_TYPE_2D; di.format = (VkFormat)format;
   di.extent = {width, height, 1}; di.mipLevels = 1; di.arrayLayers = layers; di.samples = VK_SAMPLE_COUNT_1_BIT;
   di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   VkImage depth; CHECK(vkCreateImage(d, &di, nullptr, &depth));
   { VkMemoryRequirements req; vkGetImageMemoryRequirements(d, depth, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
     ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, depth, mem, 0)); }
   VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; dvi.image = depth; dvi.viewType = VK_IMAGE_VIEW_TYPE_2D; dvi.format = di.format;
   dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
   VkImageView depth_view; CHECK(vkCreateImageView(d, &dvi, nullptr, &depth_view));
   VkAttachmentDescription att{0, di.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, (VkImageLayout)final_layout};
   VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.pDepthStencilAttachment = &ref;
   VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &att; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
   VkRenderPass pass; CHECK(vkCreateRenderPass(d, &rpi, nullptr, &pass));
   VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &depth_view;
   fbi.width = width; fbi.height = height; fbi.layers = 1;
   VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(d, &fbi, nullptr, &framebuffer));

   /* ---- pipelines (only those drawn) ---- */
   std::set<int> used; for (Draw *dr : order) used.insert(dr->pipeline);
   for (int index : used) {
      Pipeline &p = pipelines[index];
      uint32_t set_count = p.set_count;
      for (auto &b : p.bindings) set_count = std::max(set_count, b.set + 1);
      p.set_layouts.resize(set_count);
      for (uint32_t s = 0; s < set_count; ++s) {
         std::vector<VkDescriptorSetLayoutBinding> lb;
         for (auto &b : p.bindings) if (b.set == s) lb.push_back({b.binding, (VkDescriptorType)b.type, b.count, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
         VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = (uint32_t)lb.size(); li.pBindings = lb.data();
         CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &p.set_layouts[s]));
      }
      VkPushConstantRange range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
      VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = set_count; pli.pSetLayouts = p.set_layouts.data();
      pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &range;
      CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &p.layout));
      std::vector<VkPipelineShaderStageCreateInfo> stages;
      for (const char *name : {"vs", "fs"}) {
         if (!p.stages.count(name) || p.stages[name].file.empty() || p.stages[name].file == "none") continue;
         if (!strcmp(name, "fs") && no_fs) continue;
         Stage &st = p.stages[name];
         const std::string override_name = "REPLAY_VS_" + std::to_string(index);
         const char *override_file = !strcmp(name, "vs") ? getenv(override_name.c_str()) : nullptr;
         std::vector<char> code = load(override_file ? std::string(override_file) : dir + "\\" + st.file);
         if (override_file) { st.map.clear(); st.entry = "main"; }
         if (code.empty()) { printf("pipeline %d: missing %s\n", index, st.file.c_str()); return 1; }
         VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size(); smi.pCode = (const uint32_t *)code.data();
         VkShaderModule module; CHECK(vkCreateShaderModule(d, &smi, nullptr, &module));
         st.spec = {(uint32_t)st.map.size(), st.map.data(), st.data.size(), st.data.data()};
         stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                           !strcmp(name, "vs") ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, module, st.entry.c_str(),
                           st.map.empty() ? nullptr : &st.spec});
      }
      VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vin.vertexBindingDescriptionCount = (uint32_t)p.vbs.size(); vin.pVertexBindingDescriptions = p.vbs.data();
      vin.vertexAttributeDescriptionCount = (uint32_t)p.attrs.size(); vin.pVertexAttributeDescriptions = p.attrs.data();
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = (VkPrimitiveTopology)p.topology;
      VkViewport viewport{0, 0, (float)width, (float)height, 0, 1}; VkRect2D scissor{{0, 0}, {width, height}};
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
      vp.pViewports = &viewport; vp.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
      rs.cullMode = p.cull; rs.frontFace = (VkFrontFace)p.front; rs.polygonMode = (VkPolygonMode)p.polygon;
      rs.depthBiasEnable = p.bias && !getenv("REPLAY_NO_DEPTH_BIAS"); rs.depthBiasConstantFactor = p.bias_const; rs.depthBiasSlopeFactor = p.bias_slope;
      rs.depthBiasClamp = p.bias_clamp; rs.depthClampEnable = p.clamp && enable.depthClamp; rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      ds.depthTestEnable = p.depth_test; ds.depthWriteEnable = p.depth_write; ds.depthCompareOp = (VkCompareOp)p.depth_op;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
      dsi.dynamicStateCount = (uint32_t)p.dynamic.size(); dsi.pDynamicStates = p.dynamic.data();
      VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = (uint32_t)stages.size(); gpi.pStages = stages.data();
      gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
      gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dsi; gpi.layout = p.layout; gpi.renderPass = pass;
      CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &p.pipeline));
   }

   /* ---- descriptor sets, one per draw and set ---- */
   VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1024},
                                   {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = (uint32_t)order.size() * 8 + 8;
   dpi.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0])); dpi.pPoolSizes = sizes;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   for (Draw *dr : order) {
      Pipeline &p = pipelines[dr->pipeline];
      if (p.set_layouts.empty()) continue;
      dr->sets.resize(p.set_layouts.size());
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp;
      dai.descriptorSetCount = (uint32_t)p.set_layouts.size(); dai.pSetLayouts = p.set_layouts.data();
      CHECK(vkAllocateDescriptorSets(d, &dai, dr->sets.data()));
      std::vector<Binding> sorted = p.bindings;
      std::sort(sorted.begin(), sorted.end(), [](const Binding &a, const Binding &b) { return a.set != b.set ? a.set < b.set : a.binding < b.binding; });
      for (auto &b : sorted) {
         for (uint32_t e = 0; e < b.count && e < 16; ++e) {
            const Desc *x = nullptr;
            for (auto &y : dr->descs) if (y.set == b.set && y.binding == b.binding && y.element == e) x = &y;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = dr->sets[b.set]; w.dstBinding = b.binding; w.dstArrayElement = e;
            w.descriptorCount = 1; w.descriptorType = (VkDescriptorType)b.type;
            VkDescriptorBufferInfo bi{dummy, 0, 256};
            VkDescriptorImageInfo ii{sampler, x && x->kind == "image" ? real_view(*x) : view_for(VK_IMAGE_VIEW_TYPE_2D), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            switch (b.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
               if (x && x->kind == "buf" && x->id >= 0) bi = {get_buffer(x->id), 0, std::max<uint64_t>(x->range, 16)};
               w.pBufferInfo = &bi;
               if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) dr->dynamic_offsets.push_back(0);
               break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER:
               w.pImageInfo = &ii; break;
            default:
               continue;   /* texel buffers etc. left unwritten */
            }
            vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
         }
      }
   }

   /* ---- run ---- */
   VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &cpi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   const bool per_draw = getenv("REPLAY_PER_DRAW") != nullptr;
   const uint32_t query_count = 2 + (per_draw ? 2 * (uint32_t)order.size() : 0);
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = query_count;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   VkQueryPoolCreateInfo si{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; si.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; si.queryCount = 1;
   si.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                           VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
   VkQueryPool sp; CHECK(vkCreateQueryPool(d, &si, nullptr, &sp));
   VkQueryPoolCreateInfo oi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; oi.queryType = VK_QUERY_TYPE_OCCLUSION; oi.queryCount = (uint32_t)order.size() + 1;
   VkQueryPool op; CHECK(vkCreateQueryPool(d, &oi, nullptr, &op));
   std::vector<double> best_draw(order.size()); std::vector<uint64_t> samples(order.size() + 1);
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
   {
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      for (int i = 0; i < 3; ++i) {
         VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = images[i];
         b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
         b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
         VkClearColorValue white{}; for (float &f : white.float32) f = 1.0f;
         vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &b.subresourceRange);
         b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      }
      for (auto &kv : real_images) {
         VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = kv.second.image;
         b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
         b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
         vkCmdCopyBufferToImage(cmd, kv.second.staging, kv.second.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)kv.second.regions.size(), kv.second.regions.data());
         b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      }
      CHECK(vkEndCommandBuffer(cmd)); CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      printf("textures: %zu captured images, %zu views%s\n", real_images.size(), real_views.size(), use_textures ? "" : " (disabled)");
   }
   double best = 1e9; uint64_t stats[4] = {};
   std::map<int, double> best_by_pipeline; std::map<int, int> draws_by_pipeline;
   for (Draw *dr : order) ++draws_by_pipeline[dr->pipeline];
   /* CPU time of recording the draws (bind, push, dynamic state, draw calls;
    * everything the application side does per draw is prepared outside). */
   std::vector<std::vector<uint8_t>> pushes;
   for (Draw *dr : order) { pushes.push_back(dr->push); pushes.back().resize(128); }
   const bool scissor_1x1 = getenv("REPLAY_SCISSOR_1X1") != nullptr, repeat_first = getenv("REPLAY_REPEAT_FIRST") != nullptr;
   std::vector<double> record_us;
   LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
   /* REPLAY_RECORD_ONLY=<n>: record the pass n times without submitting
    * (driver CPU cost alone, e.g. for a sampling profiler), then exit. */
   if (const char *record_only = getenv("REPLAY_RECORD_ONLY")) {
      const int n = atoi(record_only);
      for (int rep = 0; rep < n; ++rep) {
         CHECK(vkResetCommandBuffer(cmd, 0)); CHECK(vkBeginCommandBuffer(cmd, &begin));
         VkClearValue clear{}; clear.depthStencil.depth = clear_depth;
         VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass; rbi.framebuffer = framebuffer;
         rbi.renderArea = {{0, 0}, {width, height}}; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
         vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
         int bound = -1;
         LARGE_INTEGER r0, r1; QueryPerformanceCounter(&r0);
         for (size_t i = 0; i < order.size(); ++i) {
            Draw *dr = order[i]; Pipeline &p = pipelines[dr->pipeline];
            if (bound != dr->pipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline); bound = dr->pipeline; }
            if (!dr->sets.empty())
               vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, (uint32_t)dr->sets.size(), dr->sets.data(),
                                       (uint32_t)dr->dynamic_offsets.size(), dr->dynamic_offsets.data());
            vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, pushes[i].data());
            if (p.has(VK_DYNAMIC_STATE_VIEWPORT)) vkCmdSetViewport(cmd, 0, 1, &dr->viewport);
            if (p.has(VK_DYNAMIC_STATE_SCISSOR)) vkCmdSetScissor(cmd, 0, 1, &dr->scissor);
            if (p.has(VK_DYNAMIC_STATE_DEPTH_BIAS)) vkCmdSetDepthBias(cmd, dr->bias_const, dr->bias_clamp, dr->bias_slope);
            for (auto &vb : dr->vbs) { VkBuffer b = get_buffer(vb.second.first); VkDeviceSize off = vb.second.second; vkCmdBindVertexBuffers(cmd, vb.first, 1, &b, &off); }
            if (dr->indexed && dr->ib >= 0)
               vkCmdBindIndexBuffer(cmd, get_buffer(dr->ib), dr->ib_offset, dr->ib_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            if (dr->indexed) vkCmdDrawIndexed(cmd, dr->count, dr->instances, dr->first, dr->vertex_offset, dr->first_instance);
            else vkCmdDraw(cmd, dr->count, dr->instances, dr->first, dr->first_instance);
         }
         QueryPerformanceCounter(&r1);
         record_us.push_back((r1.QuadPart - r0.QuadPart) * 1e6 / double(qpf.QuadPart));
         vkCmdEndRenderPass(cmd);
         CHECK(vkEndCommandBuffer(cmd));
      }
      std::sort(record_us.begin(), record_us.end());
      const double median = record_us[record_us.size() / 2];
      printf("recording only (CPU): %.1f us best, %.1f us median of %zu, %.0f ns per draw (median)\n", record_us.front(), median,
             record_us.size(), order.empty() ? 0.0 : median * 1e3 / order.size());
      return 0;
   }
   for (int rep = -5; rep < passes; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0)); CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, query_count); vkCmdResetQueryPool(cmd, sp, 0, 1); vkCmdResetQueryPool(cmd, op, 0, (uint32_t)order.size() + 1);
      VkClearValue clear{}; clear.depthStencil.depth = clear_depth;
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass; rbi.framebuffer = framebuffer;
      rbi.renderArea = {{0, 0}, {width, height}}; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBeginQuery(cmd, sp, 0, 0);
      int bound = -1;
      LARGE_INTEGER r0, r1; QueryPerformanceCounter(&r0);
      for (size_t i = 0; i < order.size(); ++i) {
         Draw *dr = order[i]; Pipeline &p = pipelines[dr->pipeline];
         if (bound != dr->pipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline); bound = dr->pipeline; }
         if (!dr->sets.empty())
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, (uint32_t)dr->sets.size(), dr->sets.data(),
                                    (uint32_t)dr->dynamic_offsets.size(), dr->dynamic_offsets.data());
         vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128, pushes[i].data());
         if (p.has(VK_DYNAMIC_STATE_VIEWPORT)) vkCmdSetViewport(cmd, 0, 1, &dr->viewport);
         if (p.has(VK_DYNAMIC_STATE_SCISSOR)) { VkRect2D sc = dr->scissor; if (scissor_1x1) sc.extent = {1, 1}; vkCmdSetScissor(cmd, 0, 1, &sc); }
         if (p.has(VK_DYNAMIC_STATE_DEPTH_BIAS)) vkCmdSetDepthBias(cmd, dr->bias_const, dr->bias_clamp, dr->bias_slope);
         for (auto &vb : dr->vbs) { VkBuffer b = get_buffer(vb.second.first); VkDeviceSize off = vb.second.second; vkCmdBindVertexBuffers(cmd, vb.first, 1, &b, &off); }
         if (dr->indexed && dr->ib >= 0)
            vkCmdBindIndexBuffer(cmd, get_buffer(dr->ib), dr->ib_offset, dr->ib_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
         if (i == 0 && repeat_first) {
            if (dr->indexed) vkCmdDrawIndexed(cmd, dr->count, dr->instances, dr->first, dr->vertex_offset, dr->first_instance);
            else vkCmdDraw(cmd, dr->count, dr->instances, dr->first, dr->first_instance);
         }
         if (per_draw) { vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 + 2 * (uint32_t)i); vkCmdBeginQuery(cmd, op, (uint32_t)i, 0); }
         if (dr->indexed) vkCmdDrawIndexed(cmd, dr->count, dr->instances, dr->first, dr->vertex_offset, dr->first_instance);
         else vkCmdDraw(cmd, dr->count, dr->instances, dr->first, dr->first_instance);
         if (per_draw) { vkCmdEndQuery(cmd, op, (uint32_t)i); vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 3 + 2 * (uint32_t)i); }
      }
      QueryPerformanceCounter(&r1);
      if (rep >= 0) record_us.push_back((r1.QuadPart - r0.QuadPart) * 1e6 / double(qpf.QuadPart));
      vkCmdEndQuery(cmd, sp, 0);
      vkCmdEndRenderPass(cmd);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cmd)); CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      std::vector<uint64_t> ts(query_count);
      CHECK(vkGetQueryPoolResults(d, qp, 0, query_count, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      CHECK(vkGetQueryPoolResults(d, sp, 0, 1, sizeof(stats), stats, sizeof(stats), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      if (rep >= 0 && ms < best) {
         best = ms;
         if (per_draw) {
            best_by_pipeline.clear();
            CHECK(vkGetQueryPoolResults(d, op, 0, (uint32_t)order.size(), order.size() * 8, samples.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            for (size_t i = 0; i < order.size(); ++i)
            { best_draw[i] = (ts[3 + 2 * i] - ts[2 + 2 * i]) * props.limits.timestampPeriod / 1e6; best_by_pipeline[order[i]->pipeline] += best_draw[i]; }
         }
      }
   }
   printf("pass: %.3f ms (best of %d), %zu draws, %zu pipelines%s; %llu IA vertices, %llu VS, %llu clip out, %llu FS\n", best, passes, order.size(),
          used.size(), no_fs ? ", no FS" : "", (unsigned long long)stats[0], (unsigned long long)stats[1], (unsigned long long)stats[2],
          (unsigned long long)stats[3]);
   if (!record_us.empty()) {
      std::sort(record_us.begin(), record_us.end());
      const double median = record_us[record_us.size() / 2];
      printf("recording (CPU): %.1f us best, %.1f us median of %zu, %.0f ns per draw (median)\n", record_us.front(), median,
             record_us.size(), order.empty() ? 0.0 : median * 1e3 / order.size());
   }
   if (per_draw) {
      std::vector<std::pair<double, int>> list;
      for (auto &kv : best_by_pipeline) list.push_back({kv.second, kv.first});
      std::sort(list.rbegin(), list.rend());
      for (auto &x : list) {
         const Pipeline &p = pipelines[x.second];
         printf("  pipeline %3d: %.3f ms in %d draws (each alone)%s\n", x.second, x.first, draws_by_pipeline[x.second],
                p.stages.count("fs") && !p.stages.at("fs").file.empty() && !no_fs ? ", FS" : "");
      }
   }
   if (per_draw && getenv("REPLAY_LIST")) {
      std::vector<size_t> idx(order.size()); for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
      std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return best_draw[a] > best_draw[b]; });
      for (int k = 0; k < atoi(getenv("REPLAY_LIST")) && k < (int)idx.size(); ++k) {
         const size_t i = idx[k]; const Draw *dr = order[i];
         printf("  draw %3zu pipeline %3d: %.3f ms, %u x %u vertices, %llu samples passed (%.2f Gsamples/s)\n", i, dr->pipeline, best_draw[i], dr->count, dr->instances,
                (unsigned long long)samples[i], samples[i] / (best_draw[i] * 1e6));
      }
   }
   return 0;
}
