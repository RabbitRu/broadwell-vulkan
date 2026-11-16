/* Compute beyond one-dimensional groups: 3D local sizes and group counts,
 * direct and indirect dispatch, and workgroup-shared memory with a barrier.
 * Every output word and the guard pages are compared with a CPU model. */
static json test_compute(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkUnmapMemory);
   API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCreateShaderModule); API(vkDestroyShaderModule); API(vkCreatePipelineLayout); API(vkDestroyPipelineLayout);
   API(vkCreateComputePipelines); API(vkDestroyPipeline);
   API(vkCreateDescriptorSetLayout); API(vkDestroyDescriptorSetLayout); API(vkCreateDescriptorPool); API(vkDestroyDescriptorPool);
   API(vkAllocateDescriptorSets); API(vkUpdateDescriptorSets);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdBindPipeline); API(vkCmdBindDescriptorSets);
   API(vkCmdDispatch); API(vkCmdDispatchIndirect); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
#undef API
   const uint32_t W = 24, H = 8, D = 4, elements = W * H * D;     /* 3x2x2 groups of 8x4x2 */
   const uint64_t data_bytes = 4096, allocation = 3 * 4096;
   struct Guarded { VkBuffer buffer{}; VkDeviceMemory memory{}; uint8_t *map{}; };
   auto make_buffer = [&](VkBufferUsageFlags usage) {
      Guarded g; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = data_bytes; bi.usage = usage;
      check(vkCreateBuffer(device, &bi, nullptr, &g.buffer), "Create compute buffer");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = allocation;
      check(vkAllocateMemory(device, &ai, nullptr, &g.memory), "Allocate compute buffer");
      check(vkBindBufferMemory(device, g.buffer, g.memory, 4096), "Bind compute buffer");
      void *map{}; check(vkMapMemory(device, g.memory, 0, VK_WHOLE_SIZE, 0, &map), "Map compute buffer"); g.map = static_cast<uint8_t *>(map);
      return g;
   };
   Guarded output = make_buffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), arguments = make_buffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   std::vector<uint8_t> arguments_expected(allocation, 0x5c);
   const VkDispatchIndirectCommand counts{3, 2, 2}; memcpy(arguments_expected.data() + 4096 + 32, &counts, sizeof(counts));
   memcpy(arguments.map, arguments_expected.data(), allocation);
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 1; dli.pBindings = &binding;
   VkDescriptorSetLayout set_layout{}; check(vkCreateDescriptorSetLayout(device, &dli, nullptr, &set_layout), "Create compute descriptor layout");
   VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &pool_size;
   VkDescriptorPool descriptor_pool{}; check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool), "Create compute descriptor pool");
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptor_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &set_layout;
   VkDescriptorSet set{}; check(vkAllocateDescriptorSets(device, &dai, &set), "Allocate compute descriptor set");
   VkDescriptorBufferInfo buffer_info{output.buffer, 0, data_bytes};
   VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &buffer_info;
   vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
   VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount = 1; li.pSetLayouts = &set_layout;
   VkPipelineLayout layout{}; check(vkCreatePipelineLayout(device, &li, nullptr, &layout), "Create compute pipeline layout");
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create compute command pool");
   json cases = json::array();
   struct Case { const char *name; const char *shader; bool shared; bool indirect; };
   for (const Case &test_case : {Case{"3D groups, direct dispatch", "compute-ids3d.spv", false, false}, Case{"3D groups, indirect dispatch", "compute-ids3d.spv", false, true},
                                 Case{"shared memory with barrier", "compute-shared.spv", true, false}}) {
      std::cerr << "Compute case " << test_case.name << '\n';
      std::ifstream input(fs::path("shaders") / test_case.shader, std::ios::binary | std::ios::ate); require(input.good(), "Missing compute SPIR-V");
      const size_t bytes = static_cast<size_t>(input.tellg()); require(bytes && bytes % 4 == 0, "Invalid compute module length");
      std::vector<uint32_t> code(bytes / 4); input.seekg(0); input.read(reinterpret_cast<char *>(code.data()), bytes); require(input.good(), "Cannot read compute SPIR-V");
      VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = bytes; si.pCode = code.data();
      VkShaderModule module{}; check(vkCreateShaderModule(device, &si, nullptr, &module), "Create compute shader module");
      VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main"; pi.layout = layout; pi.basePipelineIndex = -1;
      VkPipeline pipeline{}; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), test_case.name);
      memset(output.map, 0x5c, allocation);
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate compute command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin compute command buffer");
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      if (test_case.indirect) vkCmdDispatchIndirect(command, arguments.buffer, 32);
      else if (test_case.shared) vkCmdDispatch(command, 5, 1, 1);
      else vkCmdDispatch(command, 3, 2, 2);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), test_case.name);
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create compute fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), test_case.name); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), test_case.name);
      std::vector<uint8_t> expected(allocation, 0x5c); uint32_t written = 0;
      if (test_case.shared) {
         for (uint32_t g = 0; g < 5; ++g) for (uint32_t i = 0; i < 32; ++i) { const uint32_t value = ((i + 1) % 32) * 7 + 3 + g * 100; memcpy(expected.data() + 4096 + (g * 32 + i) * 4, &value, 4); ++written; }
      } else {
         for (uint32_t z = 0; z < D; ++z) for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            const uint32_t value = x | y << 10 | z << 20; memcpy(expected.data() + 4096 + (x + y * W + z * W * H) * 4, &value, 4); ++written; }
         require(written == elements, "Compute model size wrong");
      }
      if (memcmp(output.map, expected.data(), allocation)) {
         for (uint64_t i = 0; i < allocation; i += 4) if (memcmp(output.map + i, expected.data() + i, 4)) {
            uint32_t a, e; memcpy(&a, output.map + i, 4); memcpy(&e, expected.data() + i, 4);
            std::cerr << "Compute word " << (static_cast<int64_t>(i) - 4096) / 4 << " expected=" << std::hex << e << " actual=" << a << std::dec << '\n'; break; }
         throw std::runtime_error(std::string(test_case.name) + ": compute output or guards differ from CPU model");
      }
      require(!memcmp(arguments.map, arguments_expected.data(), allocation), "Indirect dispatch arguments or guards changed");
      cases.push_back({{"case", test_case.name}, {"verifiedWords", written}, {"guardsIntact", true}});
      vkDestroyFence(device, fence, nullptr); vkDestroyPipeline(device, pipeline, nullptr); vkDestroyShaderModule(device, module, nullptr);
   }
   vkDestroyCommandPool(device, pool, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr); vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
   for (Guarded *g : {&output, &arguments}) { vkUnmapMemory(device, g->memory); vkDestroyBuffer(device, g->buffer, nullptr); vkFreeMemory(device, g->memory, nullptr); }
   return cases;
}
