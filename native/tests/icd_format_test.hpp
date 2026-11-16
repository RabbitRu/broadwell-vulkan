/* Format and image-type coverage on hardware. For each format the advertised
 * features are checked against the Vulkan 1.0 mandatory set, then random
 * texels are uploaded to every mip level and array layer of an odd-sized
 * image and read back through the GPU; the round trip must be bit-exact.
 * 3D and cube-compatible images are covered with RGBA8. */
static json test_formats(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFormatProperties format_properties,
   VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateImage); API(vkDestroyImage); API(vkGetImageMemoryRequirements); API(vkBindImageMemory);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkUnmapMemory);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkFreeCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkCreateBuffer); API(vkDestroyBuffer); API(vkGetBufferMemoryRequirements); API(vkBindBufferMemory);
   API(vkCmdCopyBufferToImage); API(vkCmdCopyImageToBuffer);
#undef API
   enum Kind { Colour, Depth, Stencil };
   struct Format { VkFormat format; const char *name; uint32_t bytes; Kind kind; bool mandatory; bool filter; bool render; };
   /* bytes = size of one texel of the tested aspect in a buffer. */
   const Format formats[] = {
      {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM_PACK16", 2, Colour, true, true, true},
      {VK_FORMAT_A1R5G5B5_UNORM_PACK16, "A1R5G5B5_UNORM_PACK16", 2, Colour, true, true, true},
      {VK_FORMAT_R8_UNORM, "R8_UNORM", 1, Colour, true, true, true}, {VK_FORMAT_R8_SNORM, "R8_SNORM", 1, Colour, true, true, false},
      {VK_FORMAT_R8_UINT, "R8_UINT", 1, Colour, true, false, true}, {VK_FORMAT_R8_SINT, "R8_SINT", 1, Colour, true, false, true},
      {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", 2, Colour, true, true, true}, {VK_FORMAT_R8G8_SNORM, "R8G8_SNORM", 2, Colour, true, true, false},
      {VK_FORMAT_R8G8_UINT, "R8G8_UINT", 2, Colour, true, false, true}, {VK_FORMAT_R8G8_SINT, "R8G8_SINT", 2, Colour, true, false, true},
      {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, Colour, true, true, true}, {VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", 4, Colour, true, true, false},
      {VK_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT", 4, Colour, true, false, true}, {VK_FORMAT_R8G8B8A8_SINT, "R8G8B8A8_SINT", 4, Colour, true, false, true},
      {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", 4, Colour, true, true, true},
      {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 4, Colour, true, true, true}, {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB", 4, Colour, true, true, true},
      {VK_FORMAT_A8B8G8R8_UNORM_PACK32, "A8B8G8R8_UNORM_PACK32", 4, Colour, true, true, true},
      {VK_FORMAT_A8B8G8R8_SRGB_PACK32, "A8B8G8R8_SRGB_PACK32", 4, Colour, true, true, true},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", 4, Colour, true, true, true},
      {VK_FORMAT_A2B10G10R10_UINT_PACK32, "A2B10G10R10_UINT_PACK32", 4, Colour, true, false, true},
      {VK_FORMAT_R16_UINT, "R16_UINT", 2, Colour, true, false, true}, {VK_FORMAT_R16_SINT, "R16_SINT", 2, Colour, true, false, true},
      {VK_FORMAT_R16_SFLOAT, "R16_SFLOAT", 2, Colour, true, true, true},
      {VK_FORMAT_R16G16_UINT, "R16G16_UINT", 4, Colour, true, false, true}, {VK_FORMAT_R16G16_SINT, "R16G16_SINT", 4, Colour, true, false, true},
      {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT", 4, Colour, true, true, true},
      {VK_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT", 8, Colour, true, false, true}, {VK_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT", 8, Colour, true, false, true},
      {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 8, Colour, true, true, true},
      {VK_FORMAT_R32_UINT, "R32_UINT", 4, Colour, true, false, true}, {VK_FORMAT_R32_SINT, "R32_SINT", 4, Colour, true, false, true},
      {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", 4, Colour, true, false, true},
      {VK_FORMAT_R32G32_UINT, "R32G32_UINT", 8, Colour, true, false, true}, {VK_FORMAT_R32G32_SINT, "R32G32_SINT", 8, Colour, true, false, true},
      {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT", 8, Colour, true, false, true},
      {VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT", 16, Colour, true, false, true}, {VK_FORMAT_R32G32B32A32_SINT, "R32G32B32A32_SINT", 16, Colour, true, false, true},
      {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT", 16, Colour, true, false, true},
      {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT_PACK32", 4, Colour, true, true, false},
      {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "E5B9G9R9_UFLOAT_PACK32", 4, Colour, true, true, false},
      {VK_FORMAT_D16_UNORM, "D16_UNORM", 2, Depth, true, false, true},
      {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", 4, Depth, false, false, true},
      {VK_FORMAT_X8_D24_UNORM_PACK32, "X8_D24_UNORM_PACK32", 4, Depth, false, false, true},
      {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT (stencil)", 1, Stencil, false, false, true},
      {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT (stencil)", 1, Stencil, false, false, true},
      {VK_FORMAT_S8_UINT, "S8_UINT", 1, Stencil, false, false, true},
   };
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create format command pool");
   /* One staging and one read-back buffer, each between guard pages. */
   const uint64_t buffer_bytes = 256 * 1024;
   struct Guarded { VkBuffer buffer{}; VkDeviceMemory memory{}; uint8_t *map{}; };
   auto make_buffer = [&](Guarded &g) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = buffer_bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(device, &bi, nullptr, &g.buffer), "Create format buffer");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = buffer_bytes + 2 * 4096;
      check(vkAllocateMemory(device, &ai, nullptr, &g.memory), "Allocate format buffer");
      check(vkBindBufferMemory(device, g.buffer, g.memory, 4096), "Bind format buffer");
      void *map{}; check(vkMapMemory(device, g.memory, 0, VK_WHOLE_SIZE, 0, &map), "Map format buffer"); g.map = static_cast<uint8_t *>(map);
   };
   Guarded staging, readback; make_buffer(staging); make_buffer(readback);
   uint32_t seed = 0x1234567;
   auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
   struct Shape { const char *name; VkImageType type; VkImageCreateFlags flags; VkExtent3D extent; uint32_t levels, layers; };
   auto round_trip = [&](const Format &f, const Shape &shape) -> uint64_t {
      const VkImageAspectFlags aspect = f.kind == Colour ? VK_IMAGE_ASPECT_COLOR_BIT : f.kind == Depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
      VkImageAspectFlags all_aspects = aspect;
      if (f.format == VK_FORMAT_D24_UNORM_S8_UINT || f.format == VK_FORMAT_D32_SFLOAT_S8_UINT) all_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = shape.type; ii.flags = shape.flags; ii.format = f.format;
      ii.extent = shape.extent; ii.mipLevels = shape.levels; ii.arrayLayers = shape.layers; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (f.kind == Colour ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
      VkImage image{}; check(vkCreateImage(device, &ii, nullptr, &image), f.name);
      VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, image, &mr);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
      VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), f.name);
      check(vkBindImageMemory(device, image, memory, 0), f.name);
      /* Buffer layout: every (level, layer) tightly packed, each region 4-byte aligned. */
      std::vector<VkBufferImageCopy> regions; uint64_t offset = 0;
      for (uint32_t level = 0; level < shape.levels; ++level) for (uint32_t layer = 0; layer < shape.layers; ++layer) {
         VkBufferImageCopy r{}; r.bufferOffset = offset; r.imageSubresource = {aspect, level, layer, 1};
         r.imageExtent = {(std::max)(1u, shape.extent.width >> level), (std::max)(1u, shape.extent.height >> level), (std::max)(1u, shape.extent.depth >> level)};
         offset += (uint64_t(r.imageExtent.width) * r.imageExtent.height * r.imageExtent.depth * f.bytes + 3) & ~3ull; regions.push_back(r);
      }
      require(offset <= buffer_bytes, "Format test image does not fit the staging buffer");
      memset(staging.map, 0x5c, buffer_bytes + 2 * 4096); memset(readback.map, 0x5c, buffer_bytes + 2 * 4096);
      std::vector<uint8_t> expected(buffer_bytes + 2 * 4096, 0x5c);
      for (const VkBufferImageCopy &r : regions) {
         const uint64_t texels = uint64_t(r.imageExtent.width) * r.imageExtent.height * r.imageExtent.depth;
         uint8_t *out = expected.data() + 4096 + r.bufferOffset;
         for (uint64_t i = 0; i < texels; ++i) {
            if (f.format == VK_FORMAT_D32_SFLOAT) { const float value = (next() & 0xffff) / 65535.0f; memcpy(out + 4 * i, &value, 4); }
            else if (f.format == VK_FORMAT_X8_D24_UNORM_PACK32) { const uint32_t value = next() & 0xffffff; memcpy(out + 4 * i, &value, 4); }
            else for (uint32_t b = 0; b < f.bytes; ++b) out[f.bytes * i + b] = static_cast<uint8_t>(next());
            /* Half-float NaN payloads may be canonicalised; keep exponents below the maximum. */
            if (f.format == VK_FORMAT_R16_SFLOAT || f.format == VK_FORMAT_R16G16_SFLOAT || f.format == VK_FORMAT_R16G16B16A16_SFLOAT)
               for (uint32_t b = 1; b < f.bytes; b += 2) out[f.bytes * i + b] &= 0xbf;
            if (f.format == VK_FORMAT_R32_SFLOAT || f.format == VK_FORMAT_R32G32_SFLOAT || f.format == VK_FORMAT_R32G32B32A32_SFLOAT)
               for (uint32_t b = 3; b < f.bytes; b += 4) out[f.bytes * i + b] &= 0xbf;
         }
      }
      memcpy(staging.map + 4096, expected.data() + 4096, buffer_bytes);
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate format command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin format command buffer");
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
      b.subresourceRange = {all_aspects, 0, shape.levels, 0, shape.layers};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      vkCmdCopyBufferToImage(command, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, static_cast<uint32_t>(regions.size()), regions.data());
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), f.name);
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create format fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), f.name); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), f.name);
      vkDestroyFence(device, fence, nullptr); vkFreeCommandBuffers(device, pool, 1, &command);
      /* The X8 byte of X8_D24 is undefined on read-back. */
      if (f.format == VK_FORMAT_X8_D24_UNORM_PACK32) for (uint64_t i = 3; i < offset; i += 4) readback.map[4096 + i] = expected[4096 + i];
      /* Padding between regions is not written by the copy. */
      if (memcmp(readback.map, expected.data(), expected.size())) {
         for (uint64_t i = 0; i < expected.size(); ++i) if (readback.map[i] != expected[i]) {
            std::cerr << f.name << ' ' << shape.name << ": first difference at byte " << static_cast<int64_t>(i) - 4096 << " of " << offset
               << " expected=" << int(expected[i]) << " actual=" << int(readback.map[i]) << '\n'; break; }
         throw std::runtime_error(std::string(f.name) + " " + shape.name + ": upload/read-back round trip differs");
      }
      require(!memcmp(staging.map, expected.data(), expected.size()), "Format staging buffer or guards changed");
      vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
      return offset;
   };
   const Shape mipped{"2D 37x19, 3 levels, 2 layers", VK_IMAGE_TYPE_2D, 0, {37, 19, 1}, 3, 2};
   json cases = json::array(); json unsupported = json::array();
   for (const Format &f : formats) {
      VkFormatProperties properties{}; format_properties(physical, f.format, &properties);
      const VkFormatFeatureFlags have = properties.optimalTilingFeatures;
      if (!have) { require(!f.mandatory, f.name); unsupported.push_back(f.name); continue; }
      VkFormatFeatureFlags need = 0;
      if (f.kind == Colour) {
         need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
         if (f.filter) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
         if (f.render) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
      } else need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      if (f.mandatory && (have & need) != need) {
         std::cerr << f.name << ": features " << std::hex << have << " lack mandatory " << (need & ~have) << std::dec << '\n';
         throw std::runtime_error(std::string(f.name) + ": mandatory format features missing");
      }
      std::cerr << "Format " << f.name << '\n';
      const uint64_t bytes = round_trip(f, mipped);
      cases.push_back({{"format", f.name}, {"optimalFeatures", have}, {"roundTripBytes", bytes}, {"levels", 3}, {"layers", 2}});
   }
   const Format rgba{VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, Colour, true, true, true};
   json shapes = json::array();
   for (const Shape &shape : {Shape{"1D 61, 3 levels, 2 layers", VK_IMAGE_TYPE_1D, 0, {61, 1, 1}, 3, 2},
                              Shape{"3D 21x13x6, 3 levels", VK_IMAGE_TYPE_3D, 0, {21, 13, 6}, 3, 1},
                              Shape{"cube 16x16, 5 levels, 6 faces", VK_IMAGE_TYPE_2D, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, {16, 16, 1}, 5, 6},
                              Shape{"2D 200x150, full mip chain", VK_IMAGE_TYPE_2D, 0, {200, 150, 1}, 8, 1}}) {
      std::cerr << "Shape " << shape.name << '\n';
      shapes.push_back({{"shape", shape.name}, {"roundTripBytes", round_trip(rgba, shape)}});
   }
   for (Guarded *g : {&staging, &readback}) { vkUnmapMemory(device, g->memory); vkDestroyBuffer(device, g->buffer, nullptr); vkFreeMemory(device, g->memory, nullptr); }
   vkDestroyCommandPool(device, pool, nullptr);
   return {{"formats", cases}, {"unsupportedOptionalFormats", unsupported}, {"shapes", shapes}};
}
