/* Transfer commands on hardware: fill/update/copy buffer, buffer<->image and
 * image->image copies, nearest blit, colour/depth image clears and
 * vkCmdClearAttachments. Every result is read back and compared byte for byte
 * with a CPU model, together with the guard pages around each buffer. */
static json test_copies(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateImage); API(vkDestroyImage); API(vkGetImageMemoryRequirements); API(vkBindImageMemory);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkUnmapMemory);
   API(vkCreateImageView); API(vkDestroyImageView); API(vkCreateRenderPass); API(vkDestroyRenderPass);
   API(vkCreateFramebuffer); API(vkDestroyFramebuffer);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdBeginRenderPass); API(vkCmdEndRenderPass); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkCreateBuffer); API(vkDestroyBuffer); API(vkGetBufferMemoryRequirements); API(vkBindBufferMemory);
   API(vkCmdFillBuffer); API(vkCmdUpdateBuffer); API(vkCmdCopyBuffer); API(vkCmdCopyBufferToImage); API(vkCmdCopyImageToBuffer);
   API(vkCmdCopyImage); API(vkCmdBlitImage); API(vkCmdClearColorImage); API(vkCmdClearDepthStencilImage); API(vkCmdClearAttachments);
#undef API
   using ReadImage = int (*)(VkImage, void *, uint64_t, uint32_t *);
   auto read_image = reinterpret_cast<ReadImage>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_read_image"));
   require(read_image != nullptr, "Missing hardware diagnostics");
   const uint32_t W = 64, H = 48;
   struct Guarded { VkBuffer buffer{}; VkDeviceMemory memory{}; uint8_t *map{}; uint64_t bytes{}; std::vector<uint8_t> expected; };
   std::vector<Guarded *> all_buffers;
   auto make_buffer = [&](Guarded &g, uint64_t bytes) {
      g.bytes = bytes; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes;
      bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(device, &bi, nullptr, &g.buffer), "Create transfer buffer");
      VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device, g.buffer, &mr); require(mr.size <= bytes + 4096, "Unexpected transfer buffer size");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes + 3 * 4096;
      check(vkAllocateMemory(device, &ai, nullptr, &g.memory), "Allocate transfer buffer");
      check(vkBindBufferMemory(device, g.buffer, g.memory, 4096), "Bind transfer buffer");
      void *map{}; check(vkMapMemory(device, g.memory, 0, VK_WHOLE_SIZE, 0, &map), "Map transfer buffer"); g.map = static_cast<uint8_t *>(map);
      g.expected.assign(ai.allocationSize, 0x5c); memset(g.map, 0x5c, ai.allocationSize); all_buffers.push_back(&g);
   };
   auto data = [](Guarded &g) { return g.expected.data() + 4096; };
   auto verify = [&](Guarded &g, const char *what) {
      if (memcmp(g.map, g.expected.data(), g.expected.size())) {
         for (uint64_t i = 0; i < g.expected.size(); ++i) if (g.map[i] != g.expected[i]) {
            std::cerr << what << ": first difference at buffer byte " << static_cast<int64_t>(i) - 4096 << " expected=" << int(g.expected[i]) << " actual=" << int(g.map[i]) << '\n'; break; }
         throw std::runtime_error(std::string(what) + ": GPU result or guard differs from CPU model");
      }
   };
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create transfer command pool");
   auto begin_commands = [&]() {
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
      VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate transfer command buffer");
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin transfer command buffer");
      return command;
   };
   auto run = [&](VkCommandBuffer command, const char *what) {
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      check(vkEndCommandBuffer(command), what);
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create transfer fence");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(queue, 1, &submit, fence), what); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), what);
      vkDestroyFence(device, fence, nullptr);
   };
   auto transition = [&](VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = from; b.newLayout = to;
      b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image; b.subresourceRange = {aspect, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command, from == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
   };
   json report;

   /* Buffer commands. Source pattern covers 16 KiB. */
   Guarded source, target; make_buffer(source, 16384); make_buffer(target, 4096);
   for (uint32_t i = 0; i < 16384; ++i) data(source)[i] = static_cast<uint8_t>((i * 7 + (i >> 8) * 13 + 3) & 0xff);
   memcpy(source.map + 4096, data(source), 16384);
   uint8_t update[64]; for (unsigned i = 0; i < sizeof(update); ++i) update[i] = static_cast<uint8_t>(0xc0 + i);
   {
      VkCommandBuffer command = begin_commands();
      vkCmdFillBuffer(command, target.buffer, 16, 256, 0xdeadbeef);
      vkCmdUpdateBuffer(command, target.buffer, 512, sizeof(update), update);
      const VkBufferCopy regions[2] = {{0, 1024, 300}, {1000, 2048, 37}};
      vkCmdCopyBuffer(command, source.buffer, target.buffer, 2, regions);
      run(command, "Buffer fill/update/copy");
      for (unsigned i = 0; i < 64; ++i) { const uint32_t word = 0xdeadbeef; memcpy(data(target) + 16 + 4 * i, &word, 4); }
      memcpy(data(target) + 512, update, sizeof(update));
      memcpy(data(target) + 1024, data(source), 300); memcpy(data(target) + 2048, data(source) + 1000, 37);
      verify(target, "Buffer fill/update/copy"); verify(source, "Buffer copy source");
      report["bufferCommands"] = {{"fillBytes", 256}, {"updateBytes", 64}, {"copyRegions", 2}, {"verifiedBytes", target.expected.size()}};
   }

   /* Image commands, for each tiling. Texel (x,y) of the pattern image is taken
    * from the source buffer with a padded row length. */
   const uint32_t row_length = W + 3, pattern_offset = 64;
   auto texel = [&](uint32_t x, uint32_t y) { uint32_t value; memcpy(&value, data(source) + pattern_offset + (y * row_length + x) * 4, 4); return value; };
   json image_cases = json::array();
   for (VkImageTiling tiling : {VK_IMAGE_TILING_LINEAR, VK_IMAGE_TILING_OPTIMAL}) {
      std::cerr << "Copy case tiling=" << tiling << '\n';
      struct Image { VkImage image{}; VkDeviceMemory memory{}; };
      auto make_image = [&](VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImageTiling image_tiling) {
         Image result; VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format;
         ii.extent = {width, height, 1}; ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = image_tiling; ii.usage = usage;
         check(vkCreateImage(device, &ii, nullptr, &result.image), "Create transfer image");
         VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, result.image, &mr);
         VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
         check(vkAllocateMemory(device, &ai, nullptr, &result.memory), "Allocate transfer image");
         check(vkBindImageMemory(device, result.image, result.memory, 0), "Bind transfer image");
         return result;
      };
      const VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      Image a = make_image(VK_FORMAT_R8G8B8A8_UNORM, W, H, transfer, tiling), b = make_image(VK_FORMAT_R8G8B8A8_UNORM, W, H, transfer, tiling);
      Image c = make_image(VK_FORMAT_R8G8B8A8_UNORM, 2 * W, 2 * H, transfer, tiling);
      Guarded read_a, read_b, read_c; make_buffer(read_a, W * H * 4); make_buffer(read_b, W * H * 4 + 256); make_buffer(read_c, 4 * W * H * 4);
      const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      VkCommandBuffer command = begin_commands();
      transition(command, a.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy upload{}; upload.bufferOffset = pattern_offset; upload.bufferRowLength = row_length; upload.bufferImageHeight = H;
      upload.imageSubresource = layers; upload.imageExtent = {W, H, 1};
      vkCmdCopyBufferToImage(command, source.buffer, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
      transition(command, a.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy download{}; download.imageSubresource = layers; download.imageExtent = {W, H, 1};
      vkCmdCopyImageToBuffer(command, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_a.buffer, 1, &download);
      /* b: clear, then receive a sub-rectangle of a. Read back with an offset and a padded row. */
      transition(command, b.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue clear_colour{}; clear_colour.float32[0] = 0.2f; clear_colour.float32[1] = 0.4f; clear_colour.float32[2] = 0.6f; clear_colour.float32[3] = 1;
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(command, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_colour, 1, &range);
      VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = layers; copy.srcOffset = {8, 4, 0}; copy.dstOffset = {30, 20, 0}; copy.extent = {20, 10, 1};
      vkCmdCopyImage(command, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      transition(command, b.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy partial{}; partial.bufferOffset = 128; partial.bufferRowLength = 40; partial.imageSubresource = layers;
      partial.imageOffset = {24, 16, 0}; partial.imageExtent = {32, 20, 1};
      vkCmdCopyImageToBuffer(command, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_b.buffer, 1, &partial);
      /* c: nearest 2x upscale of a. */
      transition(command, c.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = layers;
      blit.srcOffsets[1] = {static_cast<int32_t>(W), static_cast<int32_t>(H), 1}; blit.dstOffsets[1] = {static_cast<int32_t>(2 * W), static_cast<int32_t>(2 * H), 1};
      vkCmdBlitImage(command, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
      transition(command, c.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy download_c{}; download_c.imageSubresource = layers; download_c.imageExtent = {2 * W, 2 * H, 1};
      vkCmdCopyImageToBuffer(command, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_c.buffer, 1, &download_c);
      run(command, "Image copy/clear/blit");
      for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { const uint32_t value = texel(x, y); memcpy(data(read_a) + (y * W + x) * 4, &value, 4); }
      verify(read_a, "CopyBufferToImage/CopyImageToBuffer");
      const uint32_t cleared = 51u | (102u << 8) | (153u << 16) | 0xff000000u;
      for (uint32_t y = 0; y < 20; ++y) for (uint32_t x = 0; x < 32; ++x) {
         const uint32_t ix = 24 + x, iy = 16 + y;   /* image coordinates of this read-back texel */
         const bool copied = ix >= 30 && ix < 50 && iy >= 20 && iy < 30;
         const uint32_t value = copied ? texel(ix - 30 + 8, iy - 20 + 4) : cleared;
         memcpy(data(read_b) + 128 + (y * 40 + x) * 4, &value, 4);
      }
      verify(read_b, "ClearColorImage/CopyImage/partial readback");
      for (uint32_t y = 0; y < 2 * H; ++y) for (uint32_t x = 0; x < 2 * W; ++x) { const uint32_t value = texel(x / 2, y / 2); memcpy(data(read_c) + (y * 2 * W + x) * 4, &value, 4); }
      verify(read_c, "BlitImage nearest 2x");
      verify(source, "Transfer source buffer");
      std::vector<uint32_t> direct(W * H); uint32_t actual_tiling{};
      require(!read_image(a.image, direct.data(), direct.size() * 4, &actual_tiling) && !memcmp(direct.data(), data(read_a), direct.size() * 4), "Independent image read differs from GPU readback");

      /* Depth clear and vkCmdClearAttachments. */
      Image depth = make_image(VK_FORMAT_D16_UNORM, W, H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | transfer, VK_IMAGE_TILING_OPTIMAL);
      Guarded read_depth; make_buffer(read_depth, W * H * 2);
      Image target_image = make_image(VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, tiling);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = target_image.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = VK_FORMAT_R8G8B8A8_UNORM; vi.subresourceRange = range;
      VkImageView view{}; check(vkCreateImageView(device, &vi, nullptr, &view), "Create clear target view");
      VkAttachmentDescription attachment{}; attachment.format = VK_FORMAT_R8G8B8A8_UNORM; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount = 1; ri.pAttachments = &attachment; ri.subpassCount = 1; ri.pSubpasses = &subpass;
      VkRenderPass render_pass{}; check(vkCreateRenderPass(device, &ri, nullptr, &render_pass), "Create clear render pass");
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass = render_pass; fi.attachmentCount = 1; fi.pAttachments = &view;
      fi.width = W; fi.height = H; fi.layers = 1;
      VkFramebuffer framebuffer{}; check(vkCreateFramebuffer(device, &fi, nullptr, &framebuffer), "Create clear framebuffer");
      command = begin_commands();
      transition(command, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      const VkClearDepthStencilValue depth_value{0.25f, 0}; const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      vkCmdClearDepthStencilImage(command, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth_value, 1, &depth_range);
      /* Read back through Vulkan: a HiZ-cleared depth surface is valid only after
       * the resolve done by the TRANSFER_SRC transition. */
      transition(command, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy depth_download{}; depth_download.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; depth_download.imageExtent = {W, H, 1};
      vkCmdCopyImageToBuffer(command, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_depth.buffer, 1, &depth_download);
      VkClearValue red{}; red.color.float32[0] = red.color.float32[3] = 1;
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass = render_pass; rp.framebuffer = framebuffer;
      rp.renderArea = {{0, 0}, {W, H}}; rp.clearValueCount = 1; rp.pClearValues = &red;
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      VkClearAttachment clear_attachment{}; clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; clear_attachment.clearValue.color.float32[1] = clear_attachment.clearValue.color.float32[3] = 1;
      const VkClearRect clear_rect{{{10, 6}, {20, 12}}, 0, 1};
      vkCmdClearAttachments(command, 1, &clear_attachment, 1, &clear_rect);
      vkCmdEndRenderPass(command);
      run(command, "Depth clear and ClearAttachments");
      std::vector<uint16_t> depth_pixels(W * H); memcpy(depth_pixels.data(), read_depth.map + 4096, W * H * 2);
      for (uint16_t value : depth_pixels) require(std::abs(static_cast<int>(value) - 16384) <= 1, "ClearDepthStencilImage value wrong");
      memcpy(data(read_depth), depth_pixels.data(), W * H * 2); verify(read_depth, "Depth readback guards");
      require(!read_image(target_image.image, direct.data(), direct.size() * 4, &actual_tiling), "Read ClearAttachments target");
      for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
         const bool inside = x >= 10 && x < 30 && y >= 6 && y < 18;
         require(direct[y * W + x] == (inside ? 0xff00ff00u : 0xff0000ffu), "ClearAttachments rectangle wrong");
      }
      image_cases.push_back({{"vkTiling", tiling}, {"width", W}, {"height", H}, {"uploadDownloadTexels", W * H}, {"copyImageTexels", 200}, {"blitTexels", 4 * W * H},
         {"clearColorImage", true}, {"clearDepthStencilImage", true}, {"clearAttachments", true}, {"guardsIntact", true}});
      vkDestroyFramebuffer(device, framebuffer, nullptr); vkDestroyRenderPass(device, render_pass, nullptr); vkDestroyImageView(device, view, nullptr);
      for (Image *image : {&a, &b, &c, &depth, &target_image}) { vkDestroyImage(device, image->image, nullptr); vkFreeMemory(device, image->memory, nullptr); }
      for (Guarded *g : {&read_a, &read_b, &read_c, &read_depth}) {
         all_buffers.erase(std::find(all_buffers.begin(), all_buffers.end(), g));
         vkUnmapMemory(device, g->memory); vkDestroyBuffer(device, g->buffer, nullptr); vkFreeMemory(device, g->memory, nullptr);
      }
   }
   report["imageCases"] = image_cases;
   for (Guarded *g : all_buffers) { vkUnmapMemory(device, g->memory); vkDestroyBuffer(device, g->buffer, nullptr); vkFreeMemory(device, g->memory, nullptr); }
   vkDestroyCommandPool(device, pool, nullptr);
   return report;
}

/* Device memory well beyond the old 128 MiB budget: twelve live 64 MiB
 * allocations (768 MiB), each written by the GPU at both ends in one batch. */
static json test_large_memory(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCmdFillBuffer); API(vkCmdPipelineBarrier); API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
#undef API
   using MemoryStats = int (*)(VkDevice, uint64_t *, uint32_t *);
   auto stats = reinterpret_cast<MemoryStats>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_query_memory"));
   require(stats != nullptr, "Missing WDDM allocation diagnostics");
   uint64_t baseline_bytes{}; uint32_t baseline_count{}; require(!stats(device, &baseline_bytes, &baseline_count), "Cannot query WDDM allocation usage");
   const uint64_t size = 64ull << 20, edge = 65536; const unsigned count = 12;
   struct Block { VkBuffer buffer{}; VkDeviceMemory memory{}; uint8_t *map{}; };
   std::vector<Block> blocks(count);
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Create large-memory command pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
   VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Allocate large-memory command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin large-memory command buffer");
   for (unsigned i = 0; i < count; ++i) {
      Block &b = blocks[i]; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(device, &bi, nullptr, &b.buffer), "Create 64 MiB buffer");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = size;
      check(vkAllocateMemory(device, &ai, nullptr, &b.memory), "Allocate 64 MiB of device memory");
      check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "Bind 64 MiB buffer");
      void *map{}; check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &map), "Map 64 MiB buffer"); b.map = static_cast<uint8_t *>(map);
      memset(b.map, 0x11, edge); memset(b.map + size - edge, 0x22, edge);
      for (uint64_t page = edge; page < size - edge; page += 1ull << 20) memset(b.map + page, static_cast<int>(0x40 + i), 4096);   /* sentinels the GPU must not touch */
      vkCmdFillBuffer(command, b.buffer, 0, edge, 0xa0000000u + i); vkCmdFillBuffer(command, b.buffer, size - edge, edge, 0xb0000000u + i);
   }
   uint64_t live_bytes{}; uint32_t live_count{};
   require(!stats(device, &live_bytes, &live_count) && live_bytes >= baseline_bytes + count * size && live_count >= baseline_count + count, "Large allocation accounting mismatch");
   VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
   check(vkEndCommandBuffer(command), "End large-memory command buffer");
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Create large-memory fence");
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
   check(vkQueueSubmit(queue, 1, &submit, fence), "Submit large-memory fills"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull), "Wait large-memory fills");
   for (unsigned i = 0; i < count; ++i) {
      const Block &b = blocks[i];
      for (uint64_t word = 0; word < edge / 4; ++word) {
         uint32_t low, high; memcpy(&low, b.map + word * 4, 4); memcpy(&high, b.map + size - edge + word * 4, 4);
         require(low == 0xa0000000u + i && high == 0xb0000000u + i, "GPU fill of a large allocation differs");
      }
      for (uint64_t page = edge; page < size - edge; page += 1ull << 20) for (unsigned k = 0; k < 4096; k += 509) require(b.map[page + k] == 0x40 + i, "Large allocation sentinel overwritten");
   }
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
   /* State pools may have grown meanwhile, so compare just before and after the frees. */
   require(!stats(device, &live_bytes, &live_count), "Cannot query WDDM allocation usage");
   for (Block &b : blocks) { vkDestroyBuffer(device, b.buffer, nullptr); vkFreeMemory(device, b.memory, nullptr); }
   uint64_t final_bytes{}; uint32_t final_count{};
   require(!stats(device, &final_bytes, &final_count) && live_bytes - final_bytes == count * size && live_count - final_count == count, "Large allocations were not returned");
   return {{"allocations", count}, {"bytesEach", size}, {"liveBytes", count * size}, {"gpuFilledBothEnds", true}, {"allReturned", true}};
}
/* Memory type 1 (device local, not host visible, no CPU mapping): the GPU
 * fills a 16 MiB buffer in it and copies 64 KiB from two places into
 * host-visible memory (type 0), which the CPU checks. */
static json test_device_local_memory(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCmdFillBuffer); API(vkCmdCopyBuffer); API(vkCmdPipelineBarrier); API(vkQueueSubmit); API(vkQueueWaitIdle);
#undef API
   const uint64_t size = 16ull << 20, chunk = 65536;
   auto make = [&](uint32_t type, VkBufferUsageFlags usage, uint64_t bytes, VkBuffer &buffer, VkDeviceMemory &memory) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = usage;
      check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Buffer");
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes; ai.memoryTypeIndex = type;
      check(vkAllocateMemory(device, &ai, nullptr, &memory), "Memory"); check(vkBindBufferMemory(device, buffer, memory, 0), "Bind");
   };
   VkBuffer gpu{}, host{}; VkDeviceMemory gpu_memory{}, host_memory{};
   make(1, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, size, gpu, gpu_memory);
   make(0, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2 * chunk, host, host_memory);
   /* Mapping type 1 would be invalid usage (VUID-vkMapMemory-memory-00682); the
    * driver refuses it anyway, but a test under validation layers must not try. */
   void *map{};
   check(vkMapMemory(device, host_memory, 0, VK_WHOLE_SIZE, 0, &map), "Map host memory");
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{}; check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
   VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
   vkCmdFillBuffer(cmd, gpu, 0, size / 2, 0x0de71ce0u); vkCmdFillBuffer(cmd, gpu, size / 2, size / 2, 0x0de71ce1u);
   VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
   const VkBufferCopy regions[2] = {{0, 0, chunk}, {size - chunk, chunk, chunk}};
   vkCmdCopyBuffer(cmd, gpu, host, 2, regions);
   barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
   check(vkEndCommandBuffer(cmd), "End");
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE), "Submit"); check(vkQueueWaitIdle(queue), "Wait");
   const uint32_t *words = static_cast<const uint32_t *>(map);
   for (uint64_t i = 0; i < chunk / 4; ++i)
      require(words[i] == 0x0de71ce0u && words[chunk / 4 + i] == 0x0de71ce1u, "Copy from device-local memory differs");
   vkDestroyCommandPool(device, pool, nullptr);
   vkDestroyBuffer(device, gpu, nullptr); vkFreeMemory(device, gpu_memory, nullptr);
   vkDestroyBuffer(device, host, nullptr); vkFreeMemory(device, host_memory, nullptr);
   return {{"bytes", size}, {"gpuWriteThenCopyVerified", true}};
}
/* One allocation of exactly maxMemoryAllocationSize (1 GiB, the Vulkan 1.1
 * minimum): the GPU fills 64 KiB at the start, the middle and the end, the
 * CPU checks them and untouched sentinels in between. */
static json test_largest_allocation(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCmdFillBuffer); API(vkCmdPipelineBarrier); API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
#undef API
   const uint64_t size = 1ull << 30, chunk = 65536; const uint64_t offsets[3] = {0, size / 2, size - chunk};
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Create 1 GiB buffer");
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = size;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate 1 GiB of device memory");
   check(vkBindBufferMemory(device, buffer, memory, 0), "Bind 1 GiB buffer");
   void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map 1 GiB allocation"); uint8_t *bytes = static_cast<uint8_t *>(map);
   for (uint64_t page = 0; page < size; page += 16ull << 20) memset(bytes + page + chunk, 0x5a, 4096);
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{}; check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
   VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &ca, &command), "Command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(command, &begin), "Begin");
   for (unsigned i = 0; i < 3; ++i) vkCmdFillBuffer(command, buffer, offsets[i], chunk, 0xc0de0000u + i);
   VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
   check(vkEndCommandBuffer(command), "End");
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Fence");
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
   check(vkQueueSubmit(queue, 1, &submit, fence), "Submit 1 GiB fills"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull), "Wait 1 GiB fills");
   for (unsigned i = 0; i < 3; ++i) for (uint64_t w = 0; w < chunk / 4; ++w) { uint32_t v; memcpy(&v, bytes + offsets[i] + w * 4, 4); require(v == 0xc0de0000u + i, "GPU fill in the 1 GiB allocation differs"); }
   for (uint64_t page = 0; page < size; page += 16ull << 20) for (unsigned k = 0; k < 4096; k += 511) require(bytes[page + chunk + k] == 0x5a, "1 GiB allocation sentinel overwritten");
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
   return {{"bytes", size}, {"gpuFills", 3}, {"sentinelPages", size / (16ull << 20)}};
}
