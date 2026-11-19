/* Sampling a depth attachment right after a render pass that cleared it with
 * LOAD_OP_CLEAR (as shadow maps are): D16, clear value V, a quad at depth 0.5
 * over the left half, then a second pass reads every texel with texelFetch
 * into an R32_SFLOAT target that is copied back. Expected: 0.5 on the left,
 * exactly V on the right (the part that keeps the clear value - with HiZ fast
 * clears that is what the sampler returns for fast-cleared blocks; Gfx8
 * fast-clears such images only to 0.0, see anv_image_hiz_clear_value).
 * Sizes 256 and 1536, one layer or layer 1 of three (as Talos' shadow maps),
 * clear values 0, 0.25 and 1, each recorded twice. Exit code 0 when all pass.
 * Shaders: depth_sample_*.vert/frag, embedded by depth_sample_spv.h. */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "depth_sample_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d (line %d)\n", #x, (int)r_, __LINE__); exit(1); } } while (0)

int main()
{
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) auto name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory); F(vkGetBufferMemoryRequirements);
   F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateGraphicsPipelines);
   F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets);
   F(vkCmdDraw); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateRenderPass); F(vkCreateFramebuffer); F(vkCmdBeginRenderPass);
   F(vkCmdEndRenderPass); F(vkCreateImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory); F(vkCreateImageView); F(vkCreateSampler);
   F(vkCmdCopyImageToBuffer); F(vkCmdPipelineBarrier);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   auto module = [&](const uint32_t *code, size_t bytes) {
      VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = bytes; smi.pCode = code;
      VkShaderModule m; CHECK(vkCreateShaderModule(d, &smi, nullptr, &m)); return m;
   };
   VkShaderModule quad_vs = module(k_depth_sample_quad_vert, sizeof(k_depth_sample_quad_vert));
   VkShaderModule fill_fs = module(k_depth_sample_fill_frag, sizeof(k_depth_sample_fill_frag));
   VkShaderModule read_vs = module(k_depth_sample_read_vert, sizeof(k_depth_sample_read_vert));
   VkShaderModule read_fs = module(k_depth_sample_read_frag, sizeof(k_depth_sample_read_frag));
   auto bind_image = [&](VkImage image) {
      VkMemoryRequirements req; vkGetImageMemoryRequirements(d, image, &req); VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = req.size; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindImageMemory(d, image, mem, 0));
   };
   VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &cpi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
   VkSampler sampler; CHECK(vkCreateSampler(d, &sci, nullptr, &sampler));
   VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &lb;
   VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout));
   VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
   pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &push;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));
   VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 16; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));

   int failures = 0;
   const uint32_t sizes[] = {256, 1536};
   const float clears[] = {0.0f, 0.25f, 1.0f};
   const uint32_t layer_counts[] = {1, 3};
   for (uint32_t size : sizes) for (uint32_t layers : layer_counts) for (float clear : clears) {
      const uint32_t layer = layers > 1 ? 1 : 0;
      /* depth image and passes */
      VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; di.imageType = VK_IMAGE_TYPE_2D; di.format = VK_FORMAT_D16_UNORM;
      di.extent = {size, size, 1}; di.mipLevels = 1; di.arrayLayers = layers; di.samples = VK_SAMPLE_COUNT_1_BIT;
      di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      VkImage depth; CHECK(vkCreateImage(d, &di, nullptr, &depth)); bind_image(depth);
      VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; dvi.image = depth; dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      dvi.format = di.format; dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
      VkImageView depth_view; CHECK(vkCreateImageView(d, &dvi, nullptr, &depth_view));
      VkAttachmentDescription datt{0, di.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkAttachmentReference dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      VkSubpassDescription dsub{}; dsub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; dsub.pDepthStencilAttachment = &dref;
      VkSubpassDependency dep{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0};
      VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.attachmentCount = 1; rpi.pAttachments = &datt;
      rpi.subpassCount = 1; rpi.pSubpasses = &dsub; rpi.dependencyCount = 1; rpi.pDependencies = &dep;
      VkRenderPass depth_pass; CHECK(vkCreateRenderPass(d, &rpi, nullptr, &depth_pass));
      VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass = depth_pass; fbi.attachmentCount = 1;
      fbi.pAttachments = &depth_view; fbi.width = fbi.height = size; fbi.layers = 1;
      VkFramebuffer depth_fb; CHECK(vkCreateFramebuffer(d, &fbi, nullptr, &depth_fb));
      /* colour target for reading */
      VkImageCreateInfo ci = di; ci.format = VK_FORMAT_R32_SFLOAT; ci.arrayLayers = 1;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkImage color; CHECK(vkCreateImage(d, &ci, nullptr, &color)); bind_image(color);
      VkImageViewCreateInfo cvi = dvi; cvi.image = color; cvi.format = ci.format; cvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView color_view; CHECK(vkCreateImageView(d, &cvi, nullptr, &color_view));
      VkAttachmentDescription catt{0, ci.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
      VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription csub{}; csub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; csub.colorAttachmentCount = 1; csub.pColorAttachments = &cref;
      VkRenderPassCreateInfo crpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; crpi.attachmentCount = 1; crpi.pAttachments = &catt;
      crpi.subpassCount = 1; crpi.pSubpasses = &csub;
      VkRenderPass color_pass; CHECK(vkCreateRenderPass(d, &crpi, nullptr, &color_pass));
      VkFramebufferCreateInfo cfbi = fbi; cfbi.renderPass = color_pass; cfbi.pAttachments = &color_view;
      VkFramebuffer color_fb; CHECK(vkCreateFramebuffer(d, &cfbi, nullptr, &color_fb));
      /* pipelines */
      VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport{0, 0, (float)size, (float)size, 0, 1}; VkRect2D scissor{{0, 0}, {size, size}};
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
      vp.pViewports = &viewport; vp.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
      VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 0xf;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
      VkPipelineShaderStageCreateInfo stages[2] = {
         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, quad_vs, "main", nullptr},
         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fill_fs, "main", nullptr}};
      VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.stageCount = 2; gpi.pStages = stages;
      gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
      gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.layout = layout; gpi.renderPass = depth_pass;
      VkPipeline fill; CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &fill));
      stages[0].module = read_vs; stages[1].module = read_fs;
      VkPipelineDepthStencilStateCreateInfo nods{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      cb.attachmentCount = 1; cb.pAttachments = &blend; gpi.pDepthStencilState = &nods; gpi.renderPass = color_pass;
      VkPipeline read; CHECK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &read));
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &set_layout;
      VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
      VkDescriptorImageInfo dii{sampler, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
      vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
      /* readback buffer */
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = (VkDeviceSize)size * size * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkBuffer buffer; CHECK(vkCreateBuffer(d, &bi, nullptr, &buffer));
      VkMemoryRequirements req; vkGetBufferMemoryRequirements(d, buffer, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; VkDeviceMemory mem;
      CHECK(vkAllocateMemory(d, &ai, nullptr, &mem)); CHECK(vkBindBufferMemory(d, buffer, mem, 0));
      float *texels; CHECK(vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, (void **)&texels));
      memset(texels, 0xcd, bi.size);
      /* record: two frames, as a game does (the second clears an image that was sampled) */
      for (int frame = 0; frame < 2; ++frame) {
         CHECK(vkResetCommandBuffer(cmd, 0));
         VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
         VkClearValue cv{}; cv.depthStencil.depth = clear;
         VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rbi.renderPass = depth_pass; rbi.framebuffer = depth_fb;
         rbi.renderArea = {{0, 0}, {size, size}}; rbi.clearValueCount = 1; rbi.pClearValues = &cv;
         vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fill);
         vkCmdDraw(cmd, 6, 1, 0, 0);
         vkCmdEndRenderPass(cmd);
         rbi.renderPass = color_pass; rbi.framebuffer = color_fb; rbi.clearValueCount = 0;
         vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, read);
         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
         vkCmdDraw(cmd, 3, 1, 0, 0);
         vkCmdEndRenderPass(cmd);
         VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {size, size, 1};
         vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
         VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
         CHECK(vkEndCommandBuffer(cmd));
         VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
         CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      }
      uint32_t bad_left = 0, bad_right = 0; float first_bad = 0;
      for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
         const float v = texels[y * size + x];
         const bool left = x < size / 2;
         const float want = left ? 0.5f : clear;
         if (fabsf(v - want) > 1.0f / 65535.0f) { if (!bad_left && !bad_right) first_bad = v; (left ? bad_left : bad_right)++; }
      }
      const bool ok = !bad_left && !bad_right;
      failures += !ok;
      printf("%s: D16 %ux%u, %u layer(s) (view layer %u), clear %.2f: drawn half %u wrong, cleared half %u wrong", ok ? "PASS" : "FAIL",
             size, size, layers, layer, clear, bad_left, bad_right);
      if (!ok) printf(" (first wrong value %f)", first_bad);
      printf("\n");
   }
   printf(failures ? "FAIL: %d case(s)\n" : "PASS: all cases\n", failures);
   return failures ? 1 : 0;
}
