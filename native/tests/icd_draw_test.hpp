/* Vertex input and the five draw forms on the real pipeline: per-vertex and
 * per-instance bindings at non-zero buffer offsets, 16/32-bit indices, base
 * vertex, first index/instance, direct and indirect draws. Geometry is
 * pixel-aligned quads with flat per-instance colours, so every pixel of the
 * render target has one exact expected value. */
static json test_draws(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateImage); API(vkDestroyImage); API(vkGetImageMemoryRequirements); API(vkBindImageMemory);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkUnmapMemory);
   API(vkCreateImageView); API(vkDestroyImageView); API(vkCreateRenderPass); API(vkDestroyRenderPass);
   API(vkCreateFramebuffer); API(vkDestroyFramebuffer); API(vkCreateShaderModule); API(vkDestroyShaderModule);
   API(vkCreatePipelineLayout); API(vkDestroyPipelineLayout); API(vkCreateGraphicsPipelines); API(vkDestroyPipeline);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdBeginRenderPass); API(vkCmdEndRenderPass);
   API(vkCmdBindPipeline); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkCreateBuffer); API(vkDestroyBuffer); API(vkGetBufferMemoryRequirements); API(vkBindBufferMemory);
   API(vkCmdBindVertexBuffers); API(vkCmdBindIndexBuffer); API(vkCmdDraw); API(vkCmdDrawIndexed);
   API(vkCmdDrawIndirect); API(vkCmdDrawIndexedIndirect);
#undef API
   using ReadImage = int (*)(VkImage, void *, uint64_t, uint32_t *);
   auto read_image = reinterpret_cast<ReadImage>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_read_image"));
   require(read_image != nullptr, "Missing hardware diagnostics");
   VkShaderModule modules[2]{};
   for (unsigned i = 0; i < 2; ++i) {
      std::ifstream input(fs::path("shaders") / (i ? "attributes.frag.spv" : "attributes.vert.spv"), std::ios::binary | std::ios::ate);
      require(input.good(), "Missing attribute SPIR-V");
      const size_t bytes = static_cast<size_t>(input.tellg()); require(bytes && bytes % 4 == 0, "Invalid attribute module length");
      std::vector<uint32_t> code(bytes / 4); input.seekg(0); input.read(reinterpret_cast<char *>(code.data()), bytes);
      require(input.good(), "Cannot read attribute SPIR-V");
      VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = bytes; si.pCode = code.data();
      check(vkCreateShaderModule(device, &si, nullptr, &modules[i]), "Create attribute shader module");
   }
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout layout{};
   check(vkCreatePipelineLayout(device, &li, nullptr, &layout), "Create draw pipeline layout");
   VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
   VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
   VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = 1; ri.pAttachments = &attachment;
   ri.subpassCount = 1; ri.pSubpasses = &subpass;
   VkRenderPass render_pass{}; check(vkCreateRenderPass(device, &ri, nullptr, &render_pass), "Create draw render pass");

   /* Each buffer sits between two 4096-byte guard regions of its own allocation. */
   struct Guarded { VkBuffer buffer{}; VkDeviceMemory memory{}; void *map{}; std::vector<uint8_t> expected; };
   std::vector<Guarded> buffers;
   auto make_buffer = [&](VkBufferUsageFlags usage, const void *data, size_t bytes) {
      Guarded g; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = usage;
      check(vkCreateBuffer(device, &bi, nullptr, &g.buffer), "Create draw input buffer");
      VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device, g.buffer, &mr); require(mr.size <= 4096, "Unexpected draw buffer size");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 3 * 4096;
      check(vkAllocateMemory(device, &ai, nullptr, &g.memory), "Allocate draw input buffer");
      check(vkBindBufferMemory(device, g.buffer, g.memory, 4096), "Bind draw input buffer");
      check(vkMapMemory(device, g.memory, 0, VK_WHOLE_SIZE, 0, &g.map), "Map draw input buffer");
      g.expected.assign(3 * 4096, 0x5c); memcpy(g.expected.data() + 4096, data, bytes); memcpy(g.map, g.expected.data(), g.expected.size());
      buffers.push_back(g); return g.buffer;
   };
   /* Unit quads of 1/8 of the target. Sets: A0 = 6 unindexed vertices of cell (0,0); A1 = its 4 corners;
    * B = 6 vertices shifted to cell (0,2); C = 4 corners shifted to cell (1,2). */
   struct Vertex { float x, y; };
   const float q = 0.25f, cell = 0.5f;
   auto corners = [&](float dx, float dy, Vertex *v) { v[0] = {-1 + dx, -1 + dy}; v[1] = {-1 + dx + q, -1 + dy}; v[2] = {-1 + dx, -1 + dy + q}; v[3] = {-1 + dx + q, -1 + dy + q}; };
   auto unindexed = [&](float dx, float dy, Vertex *v) { Vertex c[4]; corners(dx, dy, c); const unsigned order[6] = {0, 1, 2, 2, 1, 3}; for (unsigned i = 0; i < 6; ++i) v[i] = c[order[i]]; };
   uint8_t vertex_data[32 + 20 * sizeof(Vertex)]; memset(vertex_data, 0xee, sizeof(vertex_data));
   Vertex vertices[20]; unindexed(0, 0, vertices); corners(0, 0, vertices + 6); unindexed(0, 2 * cell, vertices + 10); corners(cell, 2 * cell, vertices + 16);
   memcpy(vertex_data + 32, vertices, sizeof(vertices));
   struct Instance { float ox, oy; uint8_t rgba[4]; };
   static_assert(sizeof(Instance) == 12, "Unexpected instance layout");
   Instance instances[5];
   for (unsigned i = 0; i < 5; ++i) instances[i] = {cell * (i % 4), cell * (i / 4), {static_cast<uint8_t>(40 + 30 * i), static_cast<uint8_t>(200 - 20 * i), static_cast<uint8_t>(5 + 17 * i), 255}};
   uint8_t instance_data[48 + sizeof(instances)]; memset(instance_data, 0xdd, sizeof(instance_data)); memcpy(instance_data + 48, instances, sizeof(instances));
   const uint16_t index16[4 + 6] = {0xbbbb, 0xbbbb, 0xbbbb, 0xbbbb, 0, 1, 2, 2, 1, 3};
   const uint32_t index32[4 + 3 + 6] = {0xcccccccc, 0xcccccccc, 0xcccccccc, 0xcccccccc, 99, 98, 97, 0, 1, 2, 2, 1, 3};
   uint32_t indirect[64]; memset(indirect, 0xaa, sizeof(indirect));
   const VkDrawIndirectCommand direct_command{6, 1, 10, 0}; memcpy(reinterpret_cast<char *>(indirect) + 16, &direct_command, sizeof(direct_command));
   const VkDrawIndexedIndirectCommand indexed_command{6, 1, 3, 16, 0}; memcpy(reinterpret_cast<char *>(indirect) + 64, &indexed_command, sizeof(indexed_command));
   const VkBuffer vertex_buffer = make_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_data, sizeof(vertex_data));
   const VkBuffer instance_buffer = make_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, instance_data, sizeof(instance_data));
   const VkBuffer index16_buffer = make_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index16, sizeof(index16));
   const VkBuffer index32_buffer = make_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index32, sizeof(index32));
   const VkBuffer indirect_buffer = make_buffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirect, sizeof(indirect));
   /* Which instance colour each cell of the 4x4 grid must show; -1 = clear colour. */
   int owner[4][4]; for (auto &row : owner) for (int &value : row) value = -1;
   for (int i = 0; i < 5; ++i) owner[i / 4][i % 4] = i;
   owner[2][0] = 0; owner[2][1] = 0;
   json results = json::array();
   for (uint32_t size : {128u, 256u}) for (VkImageTiling tiling : {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL}) {
      std::cerr << "Draw case " << size << " tiling=" << tiling << '\n';
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = attachment.format;
      ii.extent = {size, size, 1}; ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = tiling; ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      VkImage image{}; check(vkCreateImage(device, &ii, nullptr, &image), "Create draw render target");
      VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, image, &mr);
      const uint64_t offset = (std::max)(4096ull, mr.alignment), bytes = offset + mr.size + 4096;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes;
      VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate draw render target");
      check(vkBindImageMemory(device, image, memory, offset), "Bind draw render target");
      void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map draw render target"); memset(map, 0x4b, bytes);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ii.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view), "Create draw render target view");
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = render_pass;
      fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = fi.height = size; fi.layers = 1;
      VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "Create draw framebuffer");
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (unsigned i = 0; i < 2; ++i) { stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
         stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module = modules[i]; stages[i].pName = "main"; }
      const VkVertexInputBindingDescription bindings[2] = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
      const VkVertexInputAttributeDescription attributes[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}, {2, 1, VK_FORMAT_R8G8B8A8_UNORM, 8}};
      VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vertex.vertexBindingDescriptionCount = 2; vertex.pVertexBindingDescriptions = bindings;
      vertex.vertexAttributeDescriptionCount = 3; vertex.pVertexAttributeDescriptions = attributes;
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
      VkPipeline pipeline{}; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "Create vertex-input pipeline");
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
      check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create draw command pool");
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate draw command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin draw command buffer");
      VkClearValue clear{}; clear.color.float32[3] = 1;
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = render_pass; rp.framebuffer = framebuffer;
      rp.renderArea = scissor; rp.clearValueCount = 1; rp.pClearValues = &clear;
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      const VkBuffer bound[2] = {vertex_buffer, instance_buffer}; const VkDeviceSize bound_offsets[2] = {32, 48};
      vkCmdBindVertexBuffers(command, 0, 2, bound, bound_offsets);
      vkCmdDraw(command, 6, 2, 0, 0);                                  /* instances 0,1 */
      vkCmdBindIndexBuffer(command, index16_buffer, 8, VK_INDEX_TYPE_UINT16);
      vkCmdDrawIndexed(command, 6, 2, 0, 6, 2);                        /* instances 2,3, base vertex 6 */
      vkCmdBindIndexBuffer(command, index32_buffer, 16, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(command, 6, 1, 3, 6, 4);                        /* instance 4, first index 3 */
      vkCmdDrawIndirect(command, indirect_buffer, 16, 1, sizeof(VkDrawIndirectCommand));             /* set B, instance 0 */
      vkCmdDrawIndexedIndirect(command, indirect_buffer, 64, 1, sizeof(VkDrawIndexedIndirectCommand)); /* set C, instance 0 */
      vkCmdEndRenderPass(command);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), "End draw command buffer");
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create draw fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), "Submit draws"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), "Wait draws");
      std::vector<uint32_t> pixels(size * size); uint32_t actual_tiling{};
      require(!read_image(image, pixels.data(), pixels.size() * 4, &actual_tiling), "Read draw render target");
      const auto case_dir = fs::path("diagnostics") / ("draw-" + std::to_string(size) + "-" + std::to_string(tiling)); fs::create_directories(case_dir);
      std::ofstream(case_dir / "pixels.rgba", std::ios::binary).write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * 4);
      uint32_t coloured = 0, mismatches = 0;
      for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
         const uint32_t cx = x / (size / 4), cy = y / (size / 4);
         const bool inside = x % (size / 4) < size / 8 && y % (size / 4) < size / 8 && owner[cy][cx] >= 0;
         uint32_t expected = 0xff000000u;
         if (inside) { memcpy(&expected, instances[owner[cy][cx]].rgba, 4); ++coloured; }
         if (pixels[y * size + x] != expected && mismatches++ < 8)
            std::cerr << "Pixel " << x << ',' << y << " expected=" << std::hex << expected << " actual=" << pixels[y * size + x] << std::dec << '\n';
      }
      require(mismatches == 0, "Vertex input/indexed/instanced/indirect draw differs from CPU expectation");
      const auto guards = static_cast<const uint8_t *>(map);
      for (uint64_t i = 0; i < bytes; ++i) if (i < offset || i >= offset + mr.size) require(guards[i] == 0x4b, "Draw render target guard overwritten");
      for (const Guarded &g : buffers) require(!memcmp(g.map, g.expected.data(), g.expected.size()), "Draw input buffer or its guards changed");
      results.push_back({{"size", size}, {"vkTiling", tiling}, {"comparedPixels", size * size}, {"colouredPixels", coloured}, {"quads", 7}, {"guardsIntact", true}});
      vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyImageView(device, view, nullptr);
      vkUnmapMemory(device, memory); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
   }
   for (const Guarded &g : buffers) { vkUnmapMemory(device, g.memory); vkDestroyBuffer(device, g.buffer, nullptr); vkFreeMemory(device, g.memory, nullptr); }
   vkDestroyRenderPass(device, render_pass, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
   return results;
}
