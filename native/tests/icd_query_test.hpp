/* Occlusion, pipeline-statistics and timestamp queries on the real Vulkan pipeline. Sample counts reported by the
 * GPU are cross-checked against the pixels the same draws left in the render
 * target; CPU and GPU result paths, reset and unavailable queries are covered. */
static json test_queries(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get, float timestamp_period, uint32_t timestamp_bits)
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
   API(vkCreateQueryPool); API(vkDestroyQueryPool); API(vkCmdBeginQuery); API(vkCmdEndQuery);
   API(vkCmdResetQueryPool); API(vkCmdWriteTimestamp); API(vkCmdCopyQueryPoolResults); API(vkGetQueryPoolResults);
#undef API
   using ReadImage = int (*)(VkImage, void *, uint64_t, uint32_t *);
   auto read_image = reinterpret_cast<ReadImage>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_read_image"));
   require(read_image != nullptr, "Missing hardware diagnostics");
   VkShaderModule modules[2]{};
   for (unsigned i = 0; i < 2; ++i) {
      std::ifstream input(fs::path("shaders") / (i ? "triangle.frag.spv" : "triangle.vert.spv"), std::ios::binary | std::ios::ate);
      require(input.good(), "Missing graphics SPIR-V");
      const size_t bytes = static_cast<size_t>(input.tellg()); require(bytes && bytes % 4 == 0, "Invalid graphics module length");
      std::vector<uint32_t> code(bytes / 4); input.seekg(0); input.read(reinterpret_cast<char *>(code.data()), bytes);
      require(input.good(), "Cannot read graphics SPIR-V");
      VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = bytes; si.pCode = code.data();
      check(vkCreateShaderModule(device, &si, nullptr, &modules[i]), "Create query shader module");
   }
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout layout{};
   check(vkCreatePipelineLayout(device, &li, nullptr, &layout), "Create query pipeline layout");
   VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
   VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
   VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = 1; ri.pAttachments = &attachment;
   ri.subpassCount = 1; ri.pSubpasses = &subpass;
   VkRenderPass render_pass{}; check(vkCreateRenderPass(device, &ri, nullptr, &render_pass), "Create query render pass");
   /* GPU copy destination: 4096 guard bytes on both sides of the buffer. */
   const uint64_t result_bytes = 4096, result_allocation = 3 * 4096;
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = result_bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer result_buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &result_buffer), "Create query result buffer");
   VkMemoryRequirements br{}; vkGetBufferMemoryRequirements(device, result_buffer, &br); require(br.size <= result_bytes, "Unexpected result buffer size");
   VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bai.allocationSize = result_allocation;
   VkDeviceMemory result_memory{}; check(vkAllocateMemory(device, &bai, nullptr, &result_memory), "Allocate query result buffer");
   check(vkBindBufferMemory(device, result_buffer, result_memory, 4096), "Bind query result buffer");
   void *result_map{}; check(vkMapMemory(device, result_memory, 0, VK_WHOLE_SIZE, 0, &result_map), "Map query result buffer");
   json results = json::array();
   for (uint32_t size : {64u, 128u, 256u}) for (VkImageTiling tiling : {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL}) {
      std::cerr << "Query case " << size << " tiling=" << tiling << '\n';
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = attachment.format;
      ii.extent = {size, size, 1}; ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = tiling; ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      VkImage image{}; check(vkCreateImage(device, &ii, nullptr, &image), "Create query render target");
      VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, image, &mr);
      const uint64_t offset = (std::max)(4096ull, mr.alignment), bytes = offset + mr.size + 4096;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes;
      VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate query render target");
      check(vkBindImageMemory(device, image, memory, offset), "Bind query render target");
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ii.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view), "Create query render target view");
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = render_pass;
      fi.attachmentCount = 1; fi.pAttachments = &view; fi.width = fi.height = size; fi.layers = 1;
      VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "Create query framebuffer");
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
      VkPipeline pipeline{}; check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "Create query pipeline");
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = 4;
      VkQueryPool queries{}; check(vkCreateQueryPool(device, &qi, nullptr, &queries), "Create occlusion query pool");
      VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
      check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create query command pool");
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 4;
      VkCommandBuffer commands[4]{}; check(vkAllocateCommandBuffers(device, &ca, commands), "Allocate query command buffers");
      VkClearValue clear{}; clear.color.float32[0] = clear.color.float32[3] = 1;
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = render_pass; rp.framebuffer = framebuffer;
      rp.renderArea = scissor; rp.clearValueCount = 1; rp.pClearValues = &clear;
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      auto run = [&](VkCommandBuffer command, const char *what) {
         VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create query fence");
         VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
         check(vkQueueSubmit(queue, 1, &submit, fence), what); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), what);
         vkDestroyFence(device, fence, nullptr);
      };
      /* First batch: query 0 = one draw, 1 = nothing, 2 = two draws, 3 = reset only. */
      memset(result_map, 0x6d, result_allocation);
      check(vkBeginCommandBuffer(commands[0], &begin), "Begin query command buffer");
      vkCmdResetQueryPool(commands[0], queries, 0, 4);
      vkCmdBeginRenderPass(commands[0], &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(commands[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBeginQuery(commands[0], queries, 0, VK_QUERY_CONTROL_PRECISE_BIT); vkCmdDraw(commands[0], 3, 1, 0, 0); vkCmdEndQuery(commands[0], queries, 0);
      vkCmdBeginQuery(commands[0], queries, 1, VK_QUERY_CONTROL_PRECISE_BIT); vkCmdEndQuery(commands[0], queries, 1);
      vkCmdBeginQuery(commands[0], queries, 2, VK_QUERY_CONTROL_PRECISE_BIT);
      vkCmdDraw(commands[0], 3, 1, 0, 0); vkCmdDraw(commands[0], 3, 1, 0, 0); vkCmdEndQuery(commands[0], queries, 2);
      vkCmdEndRenderPass(commands[0]);
      vkCmdCopyQueryPoolResults(commands[0], queries, 0, 3, result_buffer, 64, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
      vkCmdCopyQueryPoolResults(commands[0], queries, 0, 4, result_buffer, 512, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
      vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(commands[0]), "End query command buffer");
      run(commands[0], "Submit occlusion queries");
      std::vector<uint32_t> pixels(size * size); uint32_t actual_tiling{};
      require(!read_image(image, pixels.data(), pixels.size() * 4, &actual_tiling), "Read query render target");
      uint64_t green = 0;
      for (uint32_t pixel : pixels) { require(pixel == 0xff0000ff || pixel == 0xff00ff00, "Unexpected query render target color"); green += pixel == 0xff00ff00; }
      require(green >= size * size / 9, "Query triangle has insufficient rasterized pixels");
      const uint64_t expected[3] = {green, 0, 2 * green};
      uint64_t wide[3][2]; memset(wide, 0xa5, sizeof(wide));
      check(vkGetQueryPoolResults(device, queries, 0, 3, sizeof(wide), wide, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT), "Get 64-bit occlusion results");
      uint32_t narrow[3]; memset(narrow, 0xa5, sizeof(narrow));
      check(vkGetQueryPoolResults(device, queries, 0, 3, sizeof(narrow), narrow, 4, 0), "Get 32-bit occlusion results");
      for (unsigned i = 0; i < 3; ++i) {
         if (wide[i][0] != expected[i]) std::cerr << "Query " << i << " expected=" << expected[i] << " actual=" << wide[i][0] << '\n';
         require(wide[i][0] == expected[i] && wide[i][1] == 1, "Occlusion sample count differs from rendered pixels");
         require(narrow[i] == static_cast<uint32_t>(expected[i]), "32-bit occlusion result differs");
      }
      uint64_t pending[2] = {0x1111111111111111ull, 0x2222222222222222ull};
      require(vkGetQueryPoolResults(device, queries, 3, 1, sizeof(pending), pending, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_NOT_READY,
         "Reset-only query must be unavailable");
      require(pending[0] == 0x1111111111111111ull && pending[1] == 0, "Unavailable query result/availability wrong");
      /* GPU copies: 64-bit+availability for 0..2 at 64, 32-bit partial for 0..3 at 512. */
      std::vector<uint8_t> copy_expected(result_allocation, 0x6d);
      for (unsigned i = 0; i < 3; ++i) { const uint64_t pair[2] = {expected[i], 1}; memcpy(copy_expected.data() + 4096 + 64 + 16 * i, pair, 16); }
      for (unsigned i = 0; i < 4; ++i) { const uint32_t pair[2] = {i < 3 ? static_cast<uint32_t>(expected[i]) : 0u, i < 3 ? 1u : 0u}; memcpy(copy_expected.data() + 4096 + 512 + 8 * i, pair, 8); }
      if (memcmp(result_map, copy_expected.data(), result_allocation)) {
         const auto actual = static_cast<const uint8_t *>(result_map);
         for (uint64_t i = 0; i < result_allocation; ++i) if (actual[i] != copy_expected[i]) { std::cerr << "Result buffer differs at " << i << '\n'; break; }
         std::ofstream(fs::path("diagnostics") / "query-copy.bin", std::ios::binary).write(static_cast<const char *>(result_map), result_allocation);
      }
      require(!memcmp(result_map, copy_expected.data(), result_allocation), "GPU query result copy or its guards differ");
      /* Second batch: reset and reuse query 0 with no draw; query 2 keeps its value. */
      check(vkBeginCommandBuffer(commands[1], &begin), "Begin query reuse command buffer");
      vkCmdResetQueryPool(commands[1], queries, 0, 1);
      vkCmdBeginRenderPass(commands[1], &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBeginQuery(commands[1], queries, 0, VK_QUERY_CONTROL_PRECISE_BIT); vkCmdEndQuery(commands[1], queries, 0);
      vkCmdEndRenderPass(commands[1]);
      vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(commands[1]), "End query reuse command buffer");
      run(commands[1], "Submit query reuse");
      check(vkGetQueryPoolResults(device, queries, 0, 3, sizeof(wide), wide, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT), "Get reused occlusion results");
      require(wide[0][0] == 0 && wide[0][1] == 1 && wide[2][0] == 2 * green && wide[2][1] == 1, "Query reset/reuse result wrong");
      /* Pipeline statistics around one draw; timestamps at both ends of the batch
       * and in a later batch after a known host delay. */
      VkQueryPoolCreateInfo sqi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; sqi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; sqi.queryCount = 1;
      sqi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
         VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
      VkQueryPool statistics{}; check(vkCreateQueryPool(device, &sqi, nullptr, &statistics), "Create pipeline statistics pool");
      VkQueryPoolCreateInfo tqi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; tqi.queryType = VK_QUERY_TYPE_TIMESTAMP; tqi.queryCount = 3;
      VkQueryPool stamps{}; check(vkCreateQueryPool(device, &tqi, nullptr, &stamps), "Create timestamp pool");
      check(vkBeginCommandBuffer(commands[2], &begin), "Begin statistics command buffer");
      vkCmdResetQueryPool(commands[2], statistics, 0, 1); vkCmdResetQueryPool(commands[2], stamps, 0, 3);
      vkCmdWriteTimestamp(commands[2], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stamps, 0);
      vkCmdBeginRenderPass(commands[2], &rp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(commands[2], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      vkCmdBeginQuery(commands[2], statistics, 0, 0); vkCmdDraw(commands[2], 3, 1, 0, 0); vkCmdEndQuery(commands[2], statistics, 0);
      vkCmdEndRenderPass(commands[2]);
      vkCmdWriteTimestamp(commands[2], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps, 1);
      vkCmdPipelineBarrier(commands[2], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(commands[2]), "End statistics command buffer");
      run(commands[2], "Submit statistics and timestamps");
      const DWORD delay_ms = 200; Sleep(delay_ms);
      check(vkBeginCommandBuffer(commands[3], &begin), "Begin late timestamp command buffer");
      vkCmdWriteTimestamp(commands[3], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps, 2);
      check(vkEndCommandBuffer(commands[3]), "End late timestamp command buffer");
      run(commands[3], "Submit late timestamp");
      uint64_t stats[7]; memset(stats, 0xa5, sizeof(stats));
      check(vkGetQueryPoolResults(device, statistics, 0, 1, sizeof(stats), stats, sizeof(stats), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT), "Get pipeline statistics");
      std::cerr << "Statistics:"; for (uint64_t value : stats) std::cerr << ' ' << value; std::cerr << '\n';
      require(stats[6] == 1 && stats[0] == 3 && stats[1] == 1 && stats[2] == 3 && stats[3] == 1 && stats[4] == 1, "Pipeline statistics differ from the single triangle");
      require(stats[5] >= green && stats[5] <= 2 * green, "Fragment shader invocation count implausible");
      uint64_t times[3][2]; memset(times, 0xa5, sizeof(times));
      check(vkGetQueryPoolResults(device, stamps, 0, 3, sizeof(times), times, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT), "Get timestamps");
      const uint64_t mask = (1ull << timestamp_bits) - 1;
      std::cerr << "Timestamps: " << times[0][0] << ' ' << times[1][0] << ' ' << times[2][0] << '\n';
      for (auto &time : times) require(time[1] == 1 && (time[0] & ~mask) == 0, "Timestamp unavailable or wider than timestampValidBits");
      const double batch_ns = static_cast<double>((times[1][0] - times[0][0]) & mask) * timestamp_period;
      const double delay_ns = static_cast<double>((times[2][0] - times[1][0]) & mask) * timestamp_period;
      require(times[0][0] != times[1][0] && batch_ns < 2e9, "Timestamps within one batch are not ordered");
      require(delay_ns >= delay_ms * 1e6 && delay_ns < delay_ms * 1e6 + 2e9, "Timestamp period does not match the host delay");
      results.push_back({{"size", size}, {"vkTiling", tiling}, {"samplesPassed", green}, {"emptyQuery", 0}, {"twoDraws", 2 * green},
         {"unavailableQueryNotReady", true}, {"gpuCopy64And32Verified", true}, {"resetReuseVerified", true}, {"guardsIntact", true},
         {"statistics", {{"iaVertices", stats[0]}, {"iaPrimitives", stats[1]}, {"vsInvocations", stats[2]}, {"clipInvocations", stats[3]},
            {"clipPrimitives", stats[4]}, {"fsInvocations", stats[5]}}},
         {"timestampBatchNs", batch_ns}, {"timestampDelayNs", delay_ns}, {"hostDelayMs", delay_ms}});
      vkDestroyQueryPool(device, statistics, nullptr); vkDestroyQueryPool(device, stamps, nullptr);
      vkDestroyCommandPool(device, pool, nullptr); vkDestroyQueryPool(device, queries, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyImageView(device, view, nullptr);
      vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
   }
   vkUnmapMemory(device, result_memory); vkDestroyBuffer(device, result_buffer, nullptr); vkFreeMemory(device, result_memory, nullptr);
   vkDestroyRenderPass(device, render_pass, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
   return results;
}
