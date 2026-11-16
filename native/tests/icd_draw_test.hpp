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
   API(vkCmdDrawIndirect); API(vkCmdDrawIndexedIndirect); API(vkCmdExecuteCommands); API(vkCmdSetScissor);
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
   uint8_t vertex_data[32 + 26 * sizeof(Vertex)]; memset(vertex_data, 0xee, sizeof(vertex_data));
   Vertex vertices[26]; unindexed(0, 0, vertices); corners(0, 0, vertices + 6); unindexed(0, 2 * cell, vertices + 10); corners(cell, 2 * cell, vertices + 16);
   /* Set D (vertices 20..25): one quad covering the whole target, for the stencil-masked pass. */
   const Vertex whole[6] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}}; memcpy(vertices + 20, whole, sizeof(whole));
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
   using QueryChunks = int (*)(VkCommandBuffer, uint32_t *);
   auto query_chunks = reinterpret_cast<QueryChunks>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_query_command_chunks"));
   require(query_chunks != nullptr, "Missing command chunk diagnostics");
   struct Case { uint32_t size; VkImageTiling tiling; unsigned mode; };
   for (const Case &test_case : {Case{128, VK_IMAGE_TILING_LINEAR, 0}, Case{128, VK_IMAGE_TILING_OPTIMAL, 0}, Case{256, VK_IMAGE_TILING_LINEAR, 0}, Case{256, VK_IMAGE_TILING_OPTIMAL, 0},
                                 Case{128, VK_IMAGE_TILING_OPTIMAL, 1}, Case{128, VK_IMAGE_TILING_OPTIMAL, 2}, Case{128, VK_IMAGE_TILING_OPTIMAL, 3},
                                 Case{128, VK_IMAGE_TILING_OPTIMAL, 4}, Case{128, VK_IMAGE_TILING_OPTIMAL, 5}, Case{128, VK_IMAGE_TILING_OPTIMAL, 6}, Case{128, VK_IMAGE_TILING_OPTIMAL, 7}}) {
      const uint32_t size = test_case.size; const VkImageTiling tiling = test_case.tiling; const unsigned mode_index = test_case.mode;
      /* Debugging aid: HASVK_TEST_DRAW_MODES=4,6 runs only the listed modes. */
      { char filter[64]; const DWORD length = GetEnvironmentVariableA("HASVK_TEST_DRAW_MODES", filter, sizeof(filter));
        if (length && length < sizeof(filter) && !strchr(filter, static_cast<char>('0' + mode_index))) continue; }
      std::cerr << "Draw case " << size << " tiling=" << tiling << " mode=" << mode_index << '\n';
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
      /* Mode 4 = additive blending, 5 = 4x MSAA resolved by the render pass, 6 = stencil mask, 7 = dynamic scissor. */
      VkRenderPass case_pass = render_pass; VkImage extra_image{}; VkDeviceMemory extra_memory{}; VkImageView extra_view{};
      VkImageView framebuffer_views[2] = {view, VK_NULL_HANDLE}; uint32_t framebuffer_view_count = 1;
      if (mode_index == 5 || mode_index == 6) {
         VkImageCreateInfo ei = ii; ei.tiling = VK_IMAGE_TILING_OPTIMAL;
         if (mode_index == 5) ei.samples = VK_SAMPLE_COUNT_4_BIT;
         else { ei.format = VK_FORMAT_D24_UNORM_S8_UINT; ei.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT; }
         check(vkCreateImage(device, &ei, nullptr, &extra_image), "Create MSAA or stencil attachment");
         VkMemoryRequirements er{}; vkGetImageMemoryRequirements(device, extra_image, &er);
         VkMemoryAllocateInfo eai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; eai.allocationSize = er.size;
         check(vkAllocateMemory(device, &eai, nullptr, &extra_memory), "Allocate MSAA or stencil attachment");
         check(vkBindImageMemory(device, extra_image, extra_memory, 0), "Bind MSAA or stencil attachment");
         VkImageViewCreateInfo ev = vi; ev.image = extra_image; ev.format = ei.format;
         if (mode_index == 6) ev.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
         check(vkCreateImageView(device, &ev, nullptr, &extra_view), "Create MSAA or stencil view");
         VkAttachmentDescription both[2] = {attachment, attachment};
         VkAttachmentReference first{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, second{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
         VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &first;
         if (mode_index == 5) {      /* 0 = multisampled colour, 1 = resolve target read back below */
            both[0].samples = VK_SAMPLE_COUNT_4_BIT; both[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; both[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            both[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; sp.pResolveAttachments = &second;
            framebuffer_views[0] = extra_view; framebuffer_views[1] = view;
         } else {                    /* 0 = colour, 1 = depth/stencil */
            both[1].format = ei.format; both[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; both[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            both[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; both[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            second.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; sp.pDepthStencilAttachment = &second;
            framebuffer_views[1] = extra_view;
         }
         VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ci.attachmentCount = 2; ci.pAttachments = both; ci.subpassCount = 1; ci.pSubpasses = &sp;
         check(vkCreateRenderPass(device, &ci, nullptr, &case_pass), "Create MSAA or stencil render pass"); framebuffer_view_count = 2;
      }
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = case_pass;
      fi.attachmentCount = framebuffer_view_count; fi.pAttachments = framebuffer_views; fi.width = fi.height = size; fi.layers = 1;
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
      pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.layout = layout; pi.renderPass = case_pass; pi.basePipelineIndex = -1;
      if (mode_index == 4) { blend.blendEnable = VK_TRUE; blend.srcColorBlendFactor = blend.dstColorBlendFactor = blend.srcAlphaBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
         blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD; }
      if (mode_index == 5) ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
      VkPipelineDepthStencilStateCreateInfo stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
      if (mode_index == 6) {     /* first pipeline only writes stencil = 5 where geometry lands */
         stencil.stencilTestEnable = VK_TRUE; stencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 5};
         stencil.back = stencil.front; stencil.maxDepthBounds = 1; pi.pDepthStencilState = &stencil; blend.colorWriteMask = 0;
      }
      const VkDynamicState dynamic_scissor = VK_DYNAMIC_STATE_SCISSOR;
      VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 1; dynamic.pDynamicStates = &dynamic_scissor;
      if (mode_index == 7) pi.pDynamicState = &dynamic;
      VkPipeline pipeline{}; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "Create vertex-input pipeline");
      VkPipeline masked_pipeline{};
      if (mode_index == 6) {     /* second pipeline draws colour only where stencil == 5 */
         stencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 5}; stencil.back = stencil.front; blend.colorWriteMask = 15;
         check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &masked_pipeline), "Create stencil-masked pipeline");
      }
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
      check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create draw command pool");
      /* Modes: 0 = inline; 1 = inline with enough repeated draws to chain several command BOs;
       * 2 = two secondaries, one inheriting the framebuffer and one not; 3 = one chained secondary. */
      const unsigned mode = mode_index; const bool use_secondaries = mode == 2 || mode == 3;
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate draw command buffer");
      VkCommandBuffer secondaries[2]{}; ca.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY; ca.commandBufferCount = 2;
      check(vkAllocateCommandBuffers(device, &ca, secondaries), "Allocate secondary command buffers");
      auto first_half = [&](VkCommandBuffer target, unsigned repeats) {
         vkCmdBindPipeline(target, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
         const VkBuffer bound[2] = {vertex_buffer, instance_buffer}; const VkDeviceSize bound_offsets[2] = {32, 48};
         vkCmdBindVertexBuffers(target, 0, 2, bound, bound_offsets);
         for (unsigned i = 0; i < repeats; ++i) vkCmdDraw(target, 6, 2, 0, 0);    /* instances 0,1 */
         vkCmdBindIndexBuffer(target, index16_buffer, 8, VK_INDEX_TYPE_UINT16);
         vkCmdDrawIndexed(target, 6, 2, 0, 6, 2);                                 /* instances 2,3, base vertex 6 */
      };
      auto second_half = [&](VkCommandBuffer target, bool bind) {
         if (bind) {
            vkCmdBindPipeline(target, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const VkBuffer bound[2] = {vertex_buffer, instance_buffer}; const VkDeviceSize bound_offsets[2] = {32, 48};
            vkCmdBindVertexBuffers(target, 0, 2, bound, bound_offsets);
         }
         vkCmdBindIndexBuffer(target, index32_buffer, 16, VK_INDEX_TYPE_UINT32);
         vkCmdDrawIndexed(target, 6, 1, 3, 6, 4);                                 /* instance 4, first index 3 */
         vkCmdDrawIndirect(target, indirect_buffer, 16, 1, sizeof(VkDrawIndirectCommand));             /* set B, instance 0 */
         vkCmdDrawIndexedIndirect(target, indirect_buffer, 64, 1, sizeof(VkDrawIndexedIndirectCommand)); /* set C, instance 0 */
      };
      const unsigned long_repeats = 6000;
      if (use_secondaries) {
         VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}; inherit.renderPass = render_pass; inherit.subpass = 0;
         VkCommandBufferBeginInfo sbegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; sbegin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT; sbegin.pInheritanceInfo = &inherit;
         inherit.framebuffer = framebuffer;
         check(vkBeginCommandBuffer(secondaries[0], &sbegin), "Begin secondary with framebuffer");
         first_half(secondaries[0], mode == 3 ? long_repeats : 1);
         if (mode == 3) second_half(secondaries[0], false);
         check(vkEndCommandBuffer(secondaries[0]), "End secondary with framebuffer");
         inherit.framebuffer = VK_NULL_HANDLE;
         check(vkBeginCommandBuffer(secondaries[1], &sbegin), "Begin secondary without framebuffer");
         if (mode == 2) second_half(secondaries[1], true);
         check(vkEndCommandBuffer(secondaries[1]), "End secondary without framebuffer");
      }
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin draw command buffer");
      VkClearValue clears[2]{}; clears[0].color.float32[3] = 1;
      if (mode_index == 4) clears[0].color.float32[0] = 0.2f;
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = case_pass; rp.framebuffer = framebuffer;
      rp.renderArea = scissor; rp.clearValueCount = framebuffer_view_count; rp.pClearValues = clears;
      vkCmdBeginRenderPass(command, &rp, use_secondaries ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
      if (use_secondaries) vkCmdExecuteCommands(command, mode == 2 ? 2 : 1, secondaries);
      else {
         const VkRect2D left_half{{0, 0}, {size / 2, size}};
         if (mode == 7) vkCmdSetScissor(command, 0, 1, &left_half);
         first_half(command, mode == 1 ? long_repeats : 1); second_half(command, false);
         if (mode == 6) { vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, masked_pipeline); vkCmdDraw(command, 6, 1, 20, 0); }
      }
      vkCmdEndRenderPass(command);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), "End draw command buffer");
      uint32_t primary_chunks{}, secondary_chunks{};
      require(!query_chunks(command, &primary_chunks) && !query_chunks(secondaries[0], &secondary_chunks), "Command chunk diagnostics failed");
      if (mode_index == 1 || mode_index == 3) require(primary_chunks >= 3, "Long command buffer did not chain several BOs");
      if (mode_index == 3) require(secondary_chunks >= 3, "Long secondary did not chain several BOs");
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create draw fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), "Submit draws"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), "Wait draws");
      std::vector<uint32_t> pixels(size * size); uint32_t actual_tiling{};
      require(!read_image(image, pixels.data(), pixels.size() * 4, &actual_tiling), "Read draw render target");
      const auto case_dir = fs::path("diagnostics") / ("draw-" + std::to_string(size) + "-" + std::to_string(tiling) + "-mode" + std::to_string(mode_index)); fs::create_directories(case_dir);
      std::ofstream(case_dir / "pixels.rgba", std::ios::binary).write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * 4);
      uint32_t coloured = 0, mismatches = 0;
      for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
         const uint32_t cx = x / (size / 4), cy = y / (size / 4);
         const bool inside = x % (size / 4) < size / 8 && y % (size / 4) < size / 8 && owner[cy][cx] >= 0;
         uint32_t expected = mode_index == 4 ? 0xff000033u : 0xff000000u;
         if (inside && !(mode_index == 7 && x >= size / 2)) {
            const uint8_t *colour = instances[mode_index == 6 ? 0 : owner[cy][cx]].rgba; memcpy(&expected, colour, 4); ++coloured;
            if (mode_index == 4) expected = (expected & 0xffffff00u) | (colour[0] + 51u);   /* ONE + ONE over the 0.2 red clear */
         }
         if (pixels[y * size + x] != expected && mismatches++ < 8)
            std::cerr << "Pixel " << x << ',' << y << " expected=" << std::hex << expected << " actual=" << pixels[y * size + x] << std::dec << '\n';
      }
      require(mismatches == 0, "Vertex input/indexed/instanced/indirect draw differs from CPU expectation");
      const auto guards = static_cast<const uint8_t *>(map);
      for (uint64_t i = 0; i < bytes; ++i) if (i < offset || i >= offset + mr.size) require(guards[i] == 0x4b, "Draw render target guard overwritten");
      for (const Guarded &g : buffers) require(!memcmp(g.map, g.expected.data(), g.expected.size()), "Draw input buffer or its guards changed");
      results.push_back({{"size", size}, {"vkTiling", tiling}, {"comparedPixels", size * size}, {"colouredPixels", coloured}, {"quads", 7}, {"guardsIntact", true},
         {"mode", std::vector<const char *>{"inline", "inline, chained command BOs", "two secondaries", "chained secondary", "additive blending",
            "4x MSAA with render-pass resolve", "stencil write then stencil-masked draw", "dynamic scissor"}[mode_index]},
         {"primaryCommandBos", primary_chunks}, {"secondaryCommandBos", secondary_chunks}});
      vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
      if (masked_pipeline) vkDestroyPipeline(device, masked_pipeline, nullptr);
      if (case_pass != render_pass) vkDestroyRenderPass(device, case_pass, nullptr);
      if (extra_view) { vkDestroyImageView(device, extra_view, nullptr); vkDestroyImage(device, extra_image, nullptr); vkFreeMemory(device, extra_memory, nullptr); }
      vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyImageView(device, view, nullptr);
      vkUnmapMemory(device, memory); vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
   }
   for (const Guarded &g : buffers) { vkUnmapMemory(device, g.memory); vkDestroyBuffer(device, g.buffer, nullptr); vkFreeMemory(device, g.memory, nullptr); }
   vkDestroyRenderPass(device, render_pass, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
   return results;
}
