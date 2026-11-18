/* Depth-only fill rate on this ICD, shaped like a shadow-map pass: a D16
 * 1536x1536 attachment cleared, then <layers> full-screen triangles drawn
 * back to front (every pixel passes), timed by GPU timestamps around the
 * render pass. Fragment stage: none, an empty shader, or an alpha test.
 * Compare with d3d11_depth_fill_probe.
 * Usage: depth_fill_probe.exe <vert.spv> <frag.spv|none> [reps] [layers] */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

static std::vector<uint32_t> load(const char *path)
{
   FILE *file = fopen(path, "rb"); if (!file) return {};
   fseek(file, 0, SEEK_END); long size = ftell(file); fseek(file, 0, SEEK_SET);
   std::vector<uint32_t> code(size / 4); fread(code.data(), 1, size, file); fclose(file);
   return code;
}

int main(int argc, char **argv)
{
   if (argc < 3) return 2;
   const bool has_fs = strcmp(argv[2], "none") != 0;
   const int reps = argc > 3 ? atoi(argv[3]) : 200;
   const uint32_t layers = argc > 4 ? atoi(argv[4]) : 8, size = 1536;
   std::vector<uint32_t> vs = load(argv[1]), fs = has_fs ? load(argv[2]) : std::vector<uint32_t>();
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   VkPhysicalDeviceProperties props; ((PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties"))(pd, &props);
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) auto name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkAllocateMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout);
   F(vkCreatePipelineLayout); F(vkCreateGraphicsPipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
   F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer);
   F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets); F(vkCmdDraw); F(vkCreateQueryPool); F(vkCmdResetQueryPool); F(vkCmdWriteTimestamp);
   F(vkGetQueryPoolResults); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory);
   F(vkCreateImageView); F(vkCreateSampler); F(vkCmdPipelineBarrier); F(vkCmdClearColorImage); F(vkCreateRenderPass); F(vkCreateFramebuffer);
   F(vkCmdBeginRenderPass); F(vkCmdEndRenderPass);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   auto make_image = [&](VkFormat format, uint32_t extent, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *image, VkImageView *view) {
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format;
      ii.extent = {extent, extent, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = usage;
      VkResult r = vkCreateImage(d, &ii, nullptr, image); if (r) return r;
      VkMemoryRequirements req; vkGetImageMemoryRequirements(d, *image, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; VkDeviceMemory mem;
      r = vkAllocateMemory(d, &ai, nullptr, &mem); if (r) return r;
      r = vkBindImageMemory(d, *image, mem, 0); if (r) return r;
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = *image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
      vi.subresourceRange = {aspect, 0, 1, 0, 1};
      return vkCreateImageView(d, &vi, nullptr, view);
   };
   VkImage depth, texture; VkImageView depth_view, texture_view;
   CHECK(make_image(VK_FORMAT_D16_UNORM, size, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &depth, &depth_view));
   CHECK(make_image(VK_FORMAT_R8G8B8A8_UNORM, 512, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &texture, &texture_view));
   VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   VkSampler sampler; CHECK(vkCreateSampler(d, &sci, nullptr, &sampler));
   VkAttachmentDescription att{0, VK_FORMAT_D16_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.pDepthStencilAttachment = &ref;
   VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &att;
   rpi.subpassCount = 1; rpi.pSubpasses = &sub;
   VkRenderPass pass; CHECK(vkCreateRenderPass(d, &rpi, nullptr, &pass));
   VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass = pass; fbi.attachmentCount = 1;
   fbi.pAttachments = &depth_view; fbi.width = fbi.height = size; fbi.layers = 1;
   VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(d, &fbi, nullptr, &framebuffer));
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &sl));
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &sl;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));
   VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = vs.size() * 4; smi.pCode = vs.data();
   VkShaderModule vmod, fmod = VK_NULL_HANDLE; CHECK(vkCreateShaderModule(d, &smi, nullptr, &vmod));
   if (has_fs) { smi.codeSize = fs.size() * 4; smi.pCode = fs.data(); CHECK(vkCreateShaderModule(d, &smi, nullptr, &fmod)); }
   VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vmod, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main", nullptr}};
   VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport viewport{0, 0, (float)size, (float)size, 0, 1}; VkRect2D scissor{{0, 0}, {size, size}};
   VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.pViewports = &viewport;
   vp.scissorCount = 1; vp.pScissors = &scissor;
   VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
   ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
   VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = has_fs ? 2 : 1; gpi.pStages = stages;
   gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs;
   gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.layout = layout; gpi.renderPass = pass;
   VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
   VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &sl;
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
   VkDescriptorImageInfo dii{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1;
   w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
   vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   {  /* opaque texture: the alpha test never discards, but the compiler cannot know */
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = texture; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkClearColorValue color{}; color.float32[0] = 0.5f; color.float32[3] = 1.0f;
      vkCmdClearColorImage(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
      b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
   }
   double best = 1e9;
   for (int rep = 0; rep < reps; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, 2);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
      VkClearValue clear{}; clear.depthStencil.depth = 1.0f;
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass; rbi.framebuffer = framebuffer;
      rbi.renderArea = scissor; rbi.clearValueCount = 1; rbi.pClearValues = &clear;
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
      vkCmdDraw(cmd, 3, layers, 0, 0);
      vkCmdEndRenderPass(cmd);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      uint64_t ts[2]; CHECK(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      if (ms < best) best = ms;
   }
   printf("vulkan %s: best %.3f ms for %u layers -> %.2f Gpixels/s\n", has_fs ? argv[2] : "no FS", best, layers,
          (double)size * size * layers / (best / 1000) / 1e9);
   return 0;
}
