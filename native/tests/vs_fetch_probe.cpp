/* Vertex shader throughput with texel-buffer loads in a loop (vs_fetch.vert)
 * on this ICD, timed by GPU timestamps around one draw of 3*2^20 vertices
 * that are all clipped away (only the VS runs). Compare with
 * d3d11_vs_fetch_probe (same shader in HLSL on Intel's D3D11 driver).
 * [draws] [vertices per draw]: many small draws instead, for the fixed cost
 * of a draw.
 * Usage: vs_fetch_probe.exe <vs_fetch.vert.spv> [iterations] [mode 0=loads 1=ALU] [draws] [vertices] */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
   if (argc < 2) return 2;
   const int count = argc > 2 ? atoi(argv[2]) : 4, mode = argc > 3 ? atoi(argv[3]) : 0;
   const uint32_t draws = argc > 4 ? atoi(argv[4]) : 1, per_draw = argc > 5 ? atoi(argv[5]) : 3u << 20;
   FILE *file = fopen(argv[1], "rb"); if (!file) return 2;
   fseek(file, 0, SEEK_END); long size = ftell(file); fseek(file, 0, SEEK_SET);
   std::vector<uint32_t> code(size / 4); fread(code.data(), 1, size, file); fclose(file);
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
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateBufferView);
   F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateGraphicsPipelines);
   F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets);
   F(vkCmdPushConstants); F(vkCmdDraw); F(vkCreateQueryPool); F(vkCmdResetQueryPool); F(vkCmdWriteTimestamp); F(vkGetQueryPoolResults);
   F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateRenderPass); F(vkCreateFramebuffer); F(vkCmdBeginRenderPass); F(vkCmdEndRenderPass);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   /* 4096 texels of R32G32B32A32_SFLOAT */
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096 * 16; bi.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
   VkBuffer buffer; CHECK(vkCreateBuffer(d, &bi, nullptr, &buffer));
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bi.size;
   VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindBufferMemory(d, buffer, mem, 0));
   float *texels; CHECK(vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, (void **)&texels));
   for (int i = 0; i < 4096 * 4; ++i) texels[i] = (float)(i % 97) * 0.01f;
   VkBufferViewCreateInfo bvi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO}; bvi.buffer = buffer; bvi.format = VK_FORMAT_R32G32B32A32_SFLOAT; bvi.range = VK_WHOLE_SIZE;
   VkBufferView view; CHECK(vkCreateBufferView(d, &bvi, nullptr, &view));
   VkAttachmentDescription att{0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
   (void)att;
   VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
   VkRenderPass pass; CHECK(vkCreateRenderPass(d, &rpi, nullptr, &pass));
   VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass = pass; fbi.width = fbi.height = 64; fbi.layers = 1;
   VkFramebuffer framebuffer; CHECK(vkCreateFramebuffer(d, &fbi, nullptr, &framebuffer));
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &sl));
   VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &sl;
   pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &range;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));
   VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size() * 4; smi.pCode = code.data();
   VkShaderModule vmod; CHECK(vkCreateShaderModule(d, &smi, nullptr, &vmod));
   VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vmod, "main", nullptr};
   VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport viewport{0, 0, 64, 64, 0, 1}; VkRect2D scissor{{0, 0}, {64, 64}};
   VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.pViewports = &viewport;
   vp.scissorCount = 1; vp.pScissors = &scissor;
   VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
   VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
   VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = 1; gpi.pStages = &stage;
   gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs;
   gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb; gpi.layout = layout; gpi.renderPass = pass;
   VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
   VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &sl;
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1;
   w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; w.pTexelBufferView = &view;
   vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   const uint32_t vertices = draws * per_draw;
   const int32_t push[2] = {count, mode};
   double best = 1e9;
   for (int rep = 0; rep < 60; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, 2);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = pass; rbi.framebuffer = framebuffer; rbi.renderArea = scissor;
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, push);
      for (uint32_t i = 0; i < draws; ++i) vkCmdDraw(cmd, per_draw, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      uint64_t ts[2]; CHECK(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      if (ms < best) best = ms;
   }
   printf("vulkan %s x%d: best %.3f ms for %u draws, %u vertices -> %.1f Mvertices/s, %.2f us per draw\n", mode ? "ALU" : "texelFetch", count, best,
          draws, vertices, vertices / (best / 1000) / 1e6, best * 1000 / draws);
   return 0;
}
