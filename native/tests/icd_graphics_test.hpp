/* Actual Vulkan graphics pipeline/draw; CPU only validates and reads pixels. */
static json test_graphics(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get, bool textured = false)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateImage); API(vkDestroyImage); API(vkGetImageMemoryRequirements); API(vkBindImageMemory);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkUnmapMemory);
   API(vkCreateImageView); API(vkDestroyImageView); API(vkCreateRenderPass); API(vkDestroyRenderPass);
   API(vkCreateFramebuffer); API(vkDestroyFramebuffer); API(vkCreateShaderModule); API(vkDestroyShaderModule);
   API(vkCreatePipelineLayout); API(vkDestroyPipelineLayout); API(vkCreateGraphicsPipelines); API(vkDestroyPipeline);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdBeginRenderPass); API(vkCmdEndRenderPass);
   API(vkCmdBindPipeline); API(vkCmdDraw); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkCreateBuffer); API(vkDestroyBuffer); API(vkGetBufferMemoryRequirements); API(vkBindBufferMemory);
   API(vkGetImageSubresourceLayout); API(vkCreateSampler); API(vkDestroySampler);
   API(vkCreateDescriptorSetLayout); API(vkDestroyDescriptorSetLayout); API(vkCreateDescriptorPool); API(vkDestroyDescriptorPool);
   API(vkAllocateDescriptorSets); API(vkUpdateDescriptorSets); API(vkCmdBindDescriptorSets);
#undef API
   using ReadImage = int (*)(VkImage, void *, uint64_t, uint32_t *);
   using ReadCommand = int (*)(VkCommandBuffer, const void **, uint32_t *);
   const auto dll = GetModuleHandleW(L"vulkan_hasvk_wddm.dll");
   auto read_image = reinterpret_cast<ReadImage>(GetProcAddress(dll, "hasvk_wddm_icd_read_image"));
   auto read_command = reinterpret_cast<ReadCommand>(GetProcAddress(dll, "hasvk_wddm_icd_query_command_buffer"));
   require(read_image && read_command, "Missing hardware diagnostics");
   VkDescriptorSetLayout descriptor_layout{}; VkDescriptorPool descriptor_pool{}; VkDescriptorSet descriptor{};
   VkBuffer uniform{}; VkDeviceMemory uniform_memory{}, texture_memory{}; VkImage texture{}; VkImageView texture_view{}; VkSampler sampler{};
   uint64_t uniform_bytes{}, texture_bytes{}, texture_offset{}, texture_size{}; void *uniform_map{}, *texture_map{};
   std::vector<uint8_t> uniform_expected;
   if (textured) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 1024; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      check(vkCreateBuffer(device, &bi, nullptr, &uniform), "Create graphics UBO");
      VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device, uniform, &mr);
      uniform_bytes = 8192 + mr.size;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = uniform_bytes;
      check(vkAllocateMemory(device, &ai, nullptr, &uniform_memory), "Allocate graphics UBO");
      check(vkBindBufferMemory(device, uniform, uniform_memory, 4096), "Bind graphics UBO");
      check(vkMapMemory(device, uniform_memory, 0, VK_WHOLE_SIZE, 0, &uniform_map), "Map graphics UBO");
      memset(uniform_map, 0x73, uniform_bytes);
      const float near_data[4] = {0.25f, 1, 0, 0}, far_data[4] = {0.75f, 0.5f, 0, 0};
      memcpy(static_cast<char *>(uniform_map) + 4096 + 64 + 256, near_data, sizeof(near_data));
      memcpy(static_cast<char *>(uniform_map) + 4096 + 64 + 512, far_data, sizeof(far_data));
      uniform_expected.assign(static_cast<uint8_t *>(uniform_map), static_cast<uint8_t *>(uniform_map) + uniform_bytes);
      VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ti.imageType = VK_IMAGE_TYPE_2D; ti.format = VK_FORMAT_R8G8B8A8_UNORM;
      ti.extent = {4, 4, 1}; ti.mipLevels = ti.arrayLayers = 1; ti.samples = VK_SAMPLE_COUNT_1_BIT;
      ti.tiling = VK_IMAGE_TILING_LINEAR; ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT; ti.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      check(vkCreateImage(device, &ti, nullptr, &texture), "Create sampled texture");
      vkGetImageMemoryRequirements(device, texture, &mr); texture_offset = (std::max)(4096ull, mr.alignment); texture_size = mr.size;
      texture_bytes = texture_offset + mr.size + 4096; ai.allocationSize = texture_bytes;
      check(vkAllocateMemory(device, &ai, nullptr, &texture_memory), "Allocate sampled texture");
      check(vkBindImageMemory(device, texture, texture_memory, texture_offset), "Bind sampled texture");
      check(vkMapMemory(device, texture_memory, 0, VK_WHOLE_SIZE, 0, &texture_map), "Map sampled texture"); memset(texture_map, 0x29, texture_bytes);
      VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout sub_layout{};
      vkGetImageSubresourceLayout(device, texture, &sub, &sub_layout);
      require(sub_layout.rowPitch >= 16 && sub_layout.offset + 3 * sub_layout.rowPitch + 16 <= mr.size, "Invalid texture subresource layout");
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
         uint8_t color[4] = {static_cast<uint8_t>((x & 1) * 255), static_cast<uint8_t>((y & 1) * 255), static_cast<uint8_t>(((x + y) & 1) ? 0 : 255), 255};
         memcpy(static_cast<char *>(texture_map) + texture_offset + sub_layout.offset + y * sub_layout.rowPitch + x * 4, color, 4);
      }
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = texture; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ti.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      check(vkCreateImageView(device, &vi, nullptr, &texture_view), "Create sampled texture view");
      VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_NEAREST;
      si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.maxLod = 0; si.maxAnisotropy = 1;
      check(vkCreateSampler(device, &si, nullptr, &sampler), "Create actual Gen8 sampler");
      VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
      VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 2; dli.pBindings = bindings;
      check(vkCreateDescriptorSetLayout(device, &dli, nullptr, &descriptor_layout), "Create graphics descriptor layout");
      VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
      VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = sizes;
      check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool), "Create graphics descriptor pool");
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptor_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &descriptor_layout;
      check(vkAllocateDescriptorSets(device, &dai, &descriptor), "Allocate graphics descriptors");
      VkDescriptorBufferInfo buffer_info{uniform, 64, 16}; VkDescriptorImageInfo image_info{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet writes[2]{}; for (auto &w : writes) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = descriptor; w.descriptorCount = 1; }
      writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; writes[0].pBufferInfo = &buffer_info;
      writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &image_info;
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
   }
   VkShaderModule modules[2]{};
   for (unsigned i = 0; i < 2; ++i) {
      const fs::path path = fs::path("shaders") / (textured ? (i ? "textured.frag.spv" : "textured.vert.spv") : (i ? "triangle.frag.spv" : "triangle.vert.spv"));
      std::ifstream input(path, std::ios::binary | std::ios::ate); require(input.good(), "Missing graphics SPIR-V");
      const size_t bytes = static_cast<size_t>(input.tellg()); require(bytes && bytes % 4 == 0, "Invalid graphics module length");
      std::vector<uint32_t> code(bytes / 4); input.seekg(0); input.read(reinterpret_cast<char *>(code.data()), bytes);
      require(input.good(), "Cannot read graphics SPIR-V");
      VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = bytes; si.pCode = code.data();
      check(vkCreateShaderModule(device, &si, nullptr, &modules[i]), "Create graphics shader module");
   }
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout layout{};
   if (textured) { li.setLayoutCount = 1; li.pSetLayouts = &descriptor_layout; }
   check(vkCreatePipelineLayout(device, &li, nullptr, &layout), "Create graphics pipeline layout");
   VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
   VkAttachmentDescription attachments[2] = {attachment, attachment};
   attachments[1].format = VK_FORMAT_D16_UNORM; attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
   VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
   if (textured) subpass.pDepthStencilAttachment = &depth_reference;
   VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = textured ? 2 : 1; ri.pAttachments = attachments;
   ri.subpassCount = 1; ri.pSubpasses = &subpass;
   VkRenderPass render_pass{}; check(vkCreateRenderPass(device, &ri, nullptr, &render_pass), "Create core Vulkan render pass");
   json results = json::array();
   bool texture_initialized = false;
   for (unsigned depth_case = 0; depth_case < (textured ? 2u : 1u); ++depth_case)
   for (uint32_t size : {64u, 128u, 256u}) for (VkImageTiling tiling : {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL}) {
      std::cerr << "Graphics case " << size << " tiling=" << tiling << ": create\n";
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = attachment.format;
      ii.extent = {size, size, 1}; ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = tiling; ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      VkImage image{}; check(vkCreateImage(device, &ii, nullptr, &image), "Create render target");
      VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, image, &mr);
      const uint64_t offset = (std::max)(4096ull, mr.alignment), bytes = offset + mr.size + 4096;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes;
      VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate render target with guards");
      check(vkBindImageMemory(device, image, memory, offset), "Bind render target at nonzero offset");
      void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map target allocation"); memset(map, 0x4b, bytes);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ii.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view), "Create render target view");
      VkImage depth_image{}; VkImageView depth_view{}; VkDeviceMemory depth_memory{}; void *depth_map{};
      uint64_t depth_offset{}, depth_bytes{}, depth_size{};
      if (textured) {
         VkImageCreateInfo di = ii; di.format = VK_FORMAT_D16_UNORM; di.tiling = VK_IMAGE_TILING_OPTIMAL; di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
         check(vkCreateImage(device, &di, nullptr, &depth_image), "Create D16 depth image");
         VkMemoryRequirements dr{}; vkGetImageMemoryRequirements(device, depth_image, &dr);
         depth_offset = (std::max)(4096ull, dr.alignment); depth_size = dr.size; depth_bytes = depth_offset + dr.size + 4096; ai.allocationSize = depth_bytes;
         check(vkAllocateMemory(device, &ai, nullptr, &depth_memory), "Allocate D16 depth image");
         check(vkBindImageMemory(device, depth_image, depth_memory, depth_offset), "Bind D16 depth image");
         check(vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0, &depth_map), "Map depth allocation"); memset(depth_map, 0x51, depth_bytes);
         VkImageViewCreateInfo dv = vi; dv.image = depth_image; dv.format = di.format; dv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
         check(vkCreateImageView(device, &dv, nullptr, &depth_view), "Create D16 depth view");
      }
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = render_pass;
      VkImageView views[2] = {view, depth_view}; fi.attachmentCount = textured ? 2 : 1; fi.pAttachments = views; fi.width = fi.height = size; fi.layers = 1;
      VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "Create framebuffer");
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (unsigned i = 0; i < 2; ++i) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
         stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = modules[i]; stages[i].pName = "main"; }
      VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1}; VkRect2D scissor{{0, 0}, {size, size}};
      VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1; vp.pViewports = &viewport; vp.pScissors = &scissor;
      VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL;
      raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &blend;
      VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pi.stageCount = 2; pi.pStages = stages;
      pi.pVertexInputState = &vertex; pi.pInputAssemblyState = &assembly; pi.pViewportState = &vp; pi.pRasterizationState = &raster;
      pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.layout = layout; pi.renderPass = render_pass; pi.basePipelineIndex = -1;
      VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      ds.depthTestEnable = ds.depthWriteEnable = depth_case != 0; ds.depthCompareOp = VK_COMPARE_OP_LESS; ds.maxDepthBounds = 1;
      if (textured) pi.pDepthStencilState = &ds;
      VkPipeline pipeline{}; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "Compile SPIR-V graphics pipeline via ELK");
      std::cerr << "Graphics pipeline compiled; recording\n";
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
      check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create graphics command pool");
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate draw command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin draw command buffer");
      if (textured && !texture_initialized) {
         VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; tb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         tb.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED; tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; tb.image = texture; tb.subresourceRange = vi.subresourceRange;
         vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
         texture_initialized = true;
      }
      VkClearValue clear{}; clear.color.float32[0] = clear.color.float32[3] = 1;
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = render_pass; rp.framebuffer = framebuffer;
      VkClearValue clears[2] = {clear, {}}; clears[1].depthStencil.depth = 1;
      rp.renderArea = scissor; rp.clearValueCount = textured ? 2 : 1; rp.pClearValues = clears;
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (textured) {
         for (uint32_t dynamic_offset : {256u, 512u}) {
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor, 1, &dynamic_offset);
            vkCmdDraw(command, 3, 1, 0, 0);
         }
      } else vkCmdDraw(command, 3, 1, 0, 0);
      vkCmdEndRenderPass(command);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), "End graphics command buffer");
      const auto case_dir = fs::path("diagnostics") / ((textured ? "textured-" : "triangle-") + std::to_string(size) + "-" + std::to_string(tiling) + "-depth" + std::to_string(depth_case)); fs::create_directories(case_dir);
      const void *commands{}; uint32_t command_bytes{}; require(!read_command(command, &commands, &command_bytes), "Cannot inspect command batch");
      std::ofstream(case_dir / "batch.bin", std::ios::binary).write(static_cast<const char *>(commands), command_bytes);
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create draw fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      std::cerr << "Submitting " << command_bytes << " bytes of graphics commands\n";
      check(vkQueueSubmit(queue, 1, &submit, fence), "Submit graphics"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), "Wait hardware draw");
      std::vector<uint32_t> pixels(size * size); uint32_t actual_tiling{};
      require(!read_image(image, pixels.data(), pixels.size() * 4, &actual_tiling), "Read actual GPU image");
      std::ofstream(case_dir / "pixels.rgba", std::ios::binary).write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * 4);
      std::vector<uint16_t> depth_pixels(size * size);
      if (textured) require(!read_image(depth_image, depth_pixels.data(), depth_pixels.size() * 2, &actual_tiling), "Read actual D16 pixels");
      uint32_t green = 0, compared = 0;
      for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
         const uint32_t actual = pixels[y * size + x];
         if (!textured) require(actual == 0xff0000ff || actual == 0xff00ff00, "Unexpected render target color");
         if (actual == 0xff00ff00) ++green;
         const bool inside = x >= size / 4 && y >= size / 4 && x + y + 1 < size;
         // Exactly-on-diagonal samples depend on the specified top-left edge rule.
         if (x + y + 1 == size && x >= size / 4 && y >= size / 4) continue;
         uint32_t expected = inside ? 0xff00ff00u : 0xff0000ffu;
         if (textured && inside) {
            const uint32_t tx = (8 * x + 4 - 2 * size) / size, ty = (8 * y + 4 - 2 * size) / size;
            const uint32_t level = depth_case ? 255 : 128;
            expected = ((tx & 1) * level) | (((ty & 1) * level) << 8) | ((((tx + ty) & 1) ? 0 : level) << 16) | 0xff000000;
         }
         if (actual != expected) { std::cerr << "Pixel " << x << ',' << y << " expected=" << std::hex << expected << " actual=" << actual << std::dec << '\n'; }
         require(actual == expected, "Triangle rasterization/texture/UBO/depth differs from CPU expectations"); ++compared;
         if (textured) {
            const uint16_t expected_depth = inside && depth_case ? 16384 : 65535;
            require(std::abs(static_cast<int>(depth_pixels[y * size + x]) - expected_depth) <= 1, "D16 depth differs from independent CPU expectation");
         }
      }
      if (!textured) require(green >= size * size / 9, "Triangle has insufficient rasterized pixels");
      const auto guards = static_cast<const uint8_t *>(map);
      for (uint64_t i = 0; i < bytes; ++i) if (i < offset || i >= offset + mr.size) require(guards[i] == 0x4b, "Render target allocation guard overwritten");
      if (textured) {
         const auto dguards = static_cast<const uint8_t *>(depth_map);
         for (uint64_t i = 0; i < depth_bytes; ++i) if (i < depth_offset || i >= depth_offset + depth_size) require(dguards[i] == 0x51, "Depth allocation guard overwritten");
         require(!memcmp(uniform_map, uniform_expected.data(), uniform_bytes), "UBO data/guards overwritten");
         const auto tguards = static_cast<const uint8_t *>(texture_map);
         for (uint64_t i = 0; i < texture_bytes; ++i) if (i < texture_offset || i >= texture_offset + texture_size) require(tguards[i] == 0x29, "Texture allocation guard overwritten");
         vkDestroyImageView(device, depth_view, nullptr); vkUnmapMemory(device, depth_memory);
         vkDestroyImage(device, depth_image, nullptr); vkFreeMemory(device, depth_memory, nullptr);
      }
      results.push_back({{"size", size}, {"vkTiling", tiling}, {"comparedPixels", compared}, {"greenPixels", green}, {"guardsIntact", true}, {"textured", textured}, {"depthTest", depth_case != 0}});
      vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyImageView(device, view, nullptr);
      vkUnmapMemory(device, memory); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
   }
   vkDestroyRenderPass(device, render_pass, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
   if (textured) {
      vkDestroyDescriptorPool(device, descriptor_pool, nullptr); vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
      vkDestroySampler(device, sampler, nullptr); vkDestroyImageView(device, texture_view, nullptr);
      vkUnmapMemory(device, texture_memory); vkDestroyImage(device, texture, nullptr); vkFreeMemory(device, texture_memory, nullptr);
      vkUnmapMemory(device, uniform_memory); vkDestroyBuffer(device, uniform, nullptr); vkFreeMemory(device, uniform_memory, nullptr);
   }
   return results;
}
