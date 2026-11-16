/* Integration test: Vulkan storage images, GPU write/read and CPU detiling.
 * Included by icd_probe.cpp after the common client helpers. */
static json test_storage_images(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define IMAGE_API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   IMAGE_API(vkCreateImage); IMAGE_API(vkDestroyImage); IMAGE_API(vkCreateImageView); IMAGE_API(vkDestroyImageView);
   IMAGE_API(vkGetImageMemoryRequirements); IMAGE_API(vkBindImageMemory);
   IMAGE_API(vkCreateBuffer); IMAGE_API(vkDestroyBuffer); IMAGE_API(vkGetBufferMemoryRequirements); IMAGE_API(vkBindBufferMemory);
   IMAGE_API(vkAllocateMemory); IMAGE_API(vkFreeMemory); IMAGE_API(vkMapMemory); IMAGE_API(vkUnmapMemory);
   IMAGE_API(vkCreateDescriptorSetLayout); IMAGE_API(vkDestroyDescriptorSetLayout); IMAGE_API(vkCreatePipelineLayout); IMAGE_API(vkDestroyPipelineLayout);
   IMAGE_API(vkCreateDescriptorPool); IMAGE_API(vkDestroyDescriptorPool); IMAGE_API(vkAllocateDescriptorSets); IMAGE_API(vkUpdateDescriptorSets);
   IMAGE_API(vkCreateShaderModule); IMAGE_API(vkDestroyShaderModule); IMAGE_API(vkCreateComputePipelines); IMAGE_API(vkDestroyPipeline);
   IMAGE_API(vkCreateCommandPool); IMAGE_API(vkDestroyCommandPool); IMAGE_API(vkAllocateCommandBuffers);
   IMAGE_API(vkBeginCommandBuffer); IMAGE_API(vkEndCommandBuffer); IMAGE_API(vkCmdBindPipeline); IMAGE_API(vkCmdBindDescriptorSets);
   IMAGE_API(vkCmdDispatch); IMAGE_API(vkCmdPipelineBarrier); IMAGE_API(vkCreateFence); IMAGE_API(vkDestroyFence);
   IMAGE_API(vkQueueSubmit); IMAGE_API(vkWaitForFences);
#undef IMAGE_API
   using ReadImage = int (*)(VkImage, void *, uint64_t, uint32_t *);
   auto read_image = reinterpret_cast<ReadImage>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_read_image"));
   require(read_image != nullptr, "Missing CPU image detile diagnostics");
   wchar_t executable[32768]; DWORD path_length = GetModuleFileNameW(nullptr, executable, 32768);
   require(path_length && path_length < 32768, "Cannot locate image shaders");
   const fs::path directory = fs::path(executable).parent_path() / "shaders";
   VkShaderModule modules[2]{};
   for (unsigned i = 0; i < 2; ++i) {
      std::ifstream file(directory / (i ? "storage-image-read.spv" : "storage-image-write.spv"), std::ios::binary | std::ios::ate);
      require(file.good(), "Image SPIR-V is missing");
      const auto bytes = file.tellg(); require(bytes > 0 && bytes % 4 == 0, "Invalid image SPIR-V size");
      std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4); file.seekg(0); file.read(reinterpret_cast<char *>(code.data()), bytes);
      require(file.good(), "Cannot read image SPIR-V");
      VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = static_cast<size_t>(bytes); si.pCode = code.data();
      check(vkCreateShaderModule(device, &si, nullptr, &modules[i]), "Create storage image shader module");
   }
   VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
   };
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = bindings;
   VkDescriptorSetLayout layout{}; check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout), "Create storage image descriptor layout");
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &layout;
   VkPipelineLayout pipeline_layout{}; check(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout), "Create storage image pipeline layout");
   json results = json::array();
   const unsigned dimensions[][2] = {{37, 23}, {129, 67}, {257, 131}};
   for (const auto &size : dimensions) {
      const unsigned width = size[0], height = size[1], pixels = width * height;
      VkSpecializationMapEntry entries[2] = {{0, 0, 4}, {1, 4, 4}};
      uint32_t specialized[2] = {width, height};
      VkSpecializationInfo specialization{2, entries, sizeof(specialized), specialized};
      VkPipeline pipelines[2]{};
      for (unsigned i = 0; i < 2; ++i) {
         VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
         pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
         pi.stage.module = modules[i]; pi.stage.pName = "main"; pi.stage.pSpecializationInfo = &specialization;
         pi.layout = pipeline_layout; pi.basePipelineIndex = -1;
         check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipelines[i]), "Compile specialized image pipeline");
      }
      for (VkImageTiling tiling : {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL}) {
         VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_info.imageType = VK_IMAGE_TYPE_2D;
         image_info.format = VK_FORMAT_R8G8B8A8_UNORM; image_info.extent = {width, height, 1}; image_info.mipLevels = image_info.arrayLayers = 1;
         image_info.samples = VK_SAMPLE_COUNT_1_BIT; image_info.tiling = tiling; image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
         VkImage image{}; check(vkCreateImage(device, &image_info, nullptr, &image), "Create real HasVK RGBA8 image");
         VkMemoryRequirements image_requirements{}; vkGetImageMemoryRequirements(device, image, &image_requirements);
         require(image_requirements.size && image_requirements.memoryTypeBits == 1, "Invalid image requirements");
         const uint64_t image_offset = (std::max)(4096ull, image_requirements.alignment);
         const uint64_t image_bytes = image_offset + image_requirements.size + 4096;
         VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = image_bytes;
         VkDeviceMemory image_memory{}; check(vkAllocateMemory(device, &ai, nullptr, &image_memory), "Allocate parameterized image BO");
         check(vkBindImageMemory(device, image, image_memory, image_offset), "Bind image at nonzero aligned offset");
         void *image_map{}; check(vkMapMemory(device, image_memory, 0, VK_WHOLE_SIZE, 0, &image_map), "Map image allocation");
         memset(image_map, 0x1b, image_bytes);
         VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
         vi.format = image_info.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view), "Create HasVK storage image view");
         VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 512ull + pixels * 16ull; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
         VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Create image readback SSBO");
         VkMemoryRequirements buffer_requirements{}; vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
         const uint64_t buffer_bytes = 8192 + ((buffer_requirements.size + 4095) & ~4095ull);
         ai.allocationSize = buffer_bytes;
         VkDeviceMemory buffer_memory{}; check(vkAllocateMemory(device, &ai, nullptr, &buffer_memory), "Allocate image readback memory");
         check(vkBindBufferMemory(device, buffer, buffer_memory, 4096), "Bind image readback buffer");
         void *buffer_map{}; check(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &buffer_map), "Map image readback");
         auto words = static_cast<uint32_t *>(buffer_map);
         std::fill(words, words + buffer_bytes / 4, 0x6ec1578fu);
         VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
         VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = pool_sizes;
         VkDescriptorPool descriptor_pool{}; check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool), "Create image descriptor pool");
         VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptor_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
         VkDescriptorSet descriptor{}; check(vkAllocateDescriptorSets(device, &dai, &descriptor), "Allocate image descriptors");
         VkDescriptorImageInfo image_descriptor{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
         VkDescriptorBufferInfo buffer_descriptor{buffer, 64, VK_WHOLE_SIZE};
         VkWriteDescriptorSet writes[2]{};
         for (auto &write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = descriptor; write.descriptorCount = 1; }
         writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &image_descriptor;
         writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &buffer_descriptor;
         vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
         VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool command_pool{};
         check(vkCreateCommandPool(device, &cpi, nullptr, &command_pool), "Create image command pool");
         VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
         VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &cai, &command), "Allocate image command buffer");
         VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin image commands");
         VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
         barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.image = image;
         barrier.subresourceRange = vi.subresourceRange; barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
         vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
         vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor, 0, nullptr);
         vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[0]); vkCmdDispatch(command, (pixels + 31) / 32, 1, 1);
         barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
         vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[1]); vkCmdDispatch(command, (pixels + 31) / 32, 1, 1);
         VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
         host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.buffer = buffer; host.size = VK_WHOLE_SIZE;
         vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
         check(vkEndCommandBuffer(command), "End image command buffer");
         VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fi, nullptr, &fence), "Create image fence");
         VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
         check(vkQueueSubmit(queue, 1, &submit, fence), "Submit image write/barrier/read");
         check(vkWaitForFences(device, 1, &fence, VK_TRUE, 20000000000ull), "Wait image GPU completion");
         std::vector<uint8_t> rgba(pixels * 4); uint32_t actual_tiling{};
         require(!read_image(image, rgba.data(), rgba.size(), &actual_tiling), "Independent CPU image detile failed");
         constexpr unsigned first = (4096 + 64) / 4 + 16;
         for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const bool green = x >= width / 4 && x < 3 * width / 4 && y >= height / 4 && y < 3 * height / 4;
            const unsigned values[4] = {green ? 0u : 255u, green ? 255u : 0u, 0, 255};
            for (unsigned c = 0; c < 4; ++c) {
               const unsigned offset = (y * width + x) * 4 + c;
               if (words[first + offset] != values[c] || rgba[offset] != values[c]) {
                  fs::path failure = fs::current_path() / "diagnostics" / ("image-failure-" + std::to_string(GetTickCount64())); fs::create_directories(failure);
                  std::ofstream(failure / "image.rgba", std::ios::binary).write(reinterpret_cast<const char *>(rgba.data()), rgba.size());
                  std::ofstream(failure / "buffer.bin", std::ios::binary).write(static_cast<const char *>(buffer_map), buffer_bytes);
                  std::ofstream(failure / "failure.json") << json({{"width", width}, {"height", height}, {"tiling", actual_tiling},
                     {"x", x}, {"y", y}, {"component", c}, {"expected", values[c]}, {"shaderRead", words[first + offset]}, {"cpuRead", rgba[offset]}}).dump(2);
                  throw std::runtime_error("Vulkan storage image mismatch: " + failure.u8string());
               }
            }
         }
         for (uint64_t i = 0; i < buffer_bytes / 4; ++i)
            if (i < first || i >= first + pixels * 4) require(words[i] == 0x6ec1578fu, "Image SSBO guard corrupted");
         const auto image_data = static_cast<const uint8_t *>(image_map);
         for (uint64_t i = 0; i < image_bytes; ++i)
            if (i < image_offset || i >= image_offset + image_requirements.size) require(image_data[i] == 0x1b, "Image BO guard corrupted");
         require(tiling == VK_IMAGE_TILING_LINEAR ? actual_tiling == 0 : actual_tiling != 0, "Image tiling was not honored");
         results.push_back({{"width", width}, {"height", height}, {"vkTiling", tiling}, {"islTiling", actual_tiling},
            {"pixelsVerified", pixels}, {"cpuDetileAndShaderReadAgreed", true}, {"auxDisabled", true}, {"passed", true}});
         vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, command_pool, nullptr);
         vkDestroyDescriptorPool(device, descriptor_pool, nullptr); vkDestroyImageView(device, view, nullptr);
         vkDestroyBuffer(device, buffer, nullptr); vkUnmapMemory(device, buffer_memory); vkFreeMemory(device, buffer_memory, nullptr);
         vkDestroyImage(device, image, nullptr); vkUnmapMemory(device, image_memory); vkFreeMemory(device, image_memory, nullptr);
      }
      for (auto pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
   }
   for (auto module : modules) vkDestroyShaderModule(device, module, nullptr);
   vkDestroyPipelineLayout(device, pipeline_layout, nullptr); vkDestroyDescriptorSetLayout(device, layout, nullptr);
   return results;
}
