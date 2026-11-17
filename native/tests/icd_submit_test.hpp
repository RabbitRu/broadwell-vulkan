/* Submission cost: wall time per vkQueueSubmit+wait for an almost empty
 * command buffer, with little and with much live device memory, and per
 * command buffer when several go in one vkQueueSubmit. Diagnostic numbers;
 * the only assertion is that every submission completes. */
#include <chrono>
/* Several command buffers in one vkQueueSubmit run in order, including a
 * repeated (simultaneous-use) buffer and a buffer that waits for an event in
 * the middle of the sequence. Fill buffers write word 0; copy buffers move
 * word 0 to word k, so every word records which fill preceded that copy. */
static json test_submit_order(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCreateFence); API(vkDestroyFence); API(vkResetFences); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCmdFillBuffer); API(vkCmdCopyBuffer); API(vkCmdPipelineBarrier); API(vkCmdWaitEvents);
   API(vkCreateEvent); API(vkDestroyEvent); API(vkSetEvent);
#undef API
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 256;
   bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Order buffer");
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 4096;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Order memory");
   check(vkBindBufferMemory(device, buffer, memory, 0), "Bind order buffer");
   void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map order memory");
   volatile uint32_t *words = static_cast<volatile uint32_t *>(map);
   VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VkEvent event{}; check(vkCreateEvent(device, &ei, nullptr, &event), "Order event");
   check(vkSetEvent(device, event), "Set order event");
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Order pool");
   /* fill[v] writes v to word 0; copy[k] copies word 0 to word k; waiter waits for the event, then copies word 0 to word 5. */
   VkCommandBuffer fill[4]{}, copy[4]{}, waiter{};
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 4;
   check(vkAllocateCommandBuffers(device, &ca, fill), "Fill buffers"); check(vkAllocateCommandBuffers(device, &ca, copy), "Copy buffers");
   ca.commandBufferCount = 1; check(vkAllocateCommandBuffers(device, &ca, &waiter), "Waiter buffer");
   VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
   transfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT; transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
   auto record = [&](VkCommandBuffer c, auto &&body) {
      check(vkBeginCommandBuffer(c, &begin), "Begin order");
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transfer, 0, nullptr, 0, nullptr);
      body(c); check(vkEndCommandBuffer(c), "End order");
   };
   auto copy_word = [&](VkCommandBuffer c, uint32_t k) { VkBufferCopy r{0, 4ull * k, 4}; vkCmdCopyBuffer(c, buffer, buffer, 1, &r); };
   for (uint32_t v = 1; v < 4; ++v) record(fill[v], [&](VkCommandBuffer c) { vkCmdFillBuffer(c, buffer, 0, 4, v); });
   for (uint32_t k = 1; k < 4; ++k) record(copy[k], [&](VkCommandBuffer c) { copy_word(c, k); });
   record(waiter, [&](VkCommandBuffer c) {
      vkCmdWaitEvents(c, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
      vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transfer, 0, nullptr, 0, nullptr);
      copy_word(c, 5); });
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Order fence");
   auto submit = [&](std::vector<VkCommandBuffer> sequence, std::vector<uint32_t> expected, const char *what) {
      for (uint32_t i = 0; i < 8; ++i) words[i] = 0xffffffffu;
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = uint32_t(sequence.size()); s.pCommandBuffers = sequence.data();
      check(vkQueueSubmit(queue, 1, &s, fence), what); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), what);
      check(vkResetFences(device, 1, &fence), what);
      for (uint32_t i = 0; i < 8; ++i) if (words[i] != expected[i]) {
         std::cerr << what << ": word " << i << " = " << words[i] << ", expected " << expected[i] << '\n';
         throw std::runtime_error(std::string("Command buffer order differs: ") + what);
      }
   };
   const uint32_t none = 0xffffffffu;
   submit({fill[1], copy[1], fill[2], copy[2], copy[1], waiter, fill[3], copy[3]}, {3, 2, 2, 3, none, 2, none, none},
          "repeat and event wait inside one submission");
   submit({fill[1], waiter, copy[2]}, {1, none, 1, none, none, 1, none, none}, "same buffers re-chained in another order");
   submit({fill[2], copy[1], copy[1], copy[2], fill[3], copy[3]}, {3, 2, 2, 3, none, none, none, none}, "back-to-back repeat");
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyEvent(device, event, nullptr);
   vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
   return {{"passed", true}, {"sequences", 3}};
}
static json test_submit_cost(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCreateFence); API(vkDestroyFence); API(vkResetFences); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkAllocateMemory); API(vkFreeMemory);
#undef API
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Submit-cost pool");
   VkCommandBuffer commands[8]{};
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 8;
   check(vkAllocateCommandBuffers(device, &ca, commands), "Submit-cost command buffers");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   for (VkCommandBuffer c : commands) { check(vkBeginCommandBuffer(c, &begin), "Begin"); check(vkEndCommandBuffer(c), "End"); }
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Fence");
   auto measure = [&](uint32_t per_submit, unsigned rounds) {
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < rounds; ++i) {
         VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = per_submit; s.pCommandBuffers = commands;
         check(vkQueueSubmit(queue, 1, &s, fence), "Submit"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "Wait");
         check(vkResetFences(device, 1, &fence), "Reset");
      }
      return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / (rounds * per_submit);
   };
   json result;
   result["usPerCommandBufferSmallMemory"] = measure(1, 40);
   result["usPerCommandBufferEightPerSubmit"] = measure(8, 10);
   /* 512 MiB of live allocations that the submission does not reference. */
   std::vector<VkDeviceMemory> ballast(4);
   for (auto &m : ballast) { VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 128ull << 20; check(vkAllocateMemory(device, &ai, nullptr, &m), "Ballast"); }
   result["usPerCommandBufferWith512MiBLive"] = measure(1, 10);
   {  /* CPU read bandwidth of a mapped WDDM allocation vs ordinary heap memory:
       * a write-combined/uncached mapping reads an order of magnitude slower. */
      auto map_memory = reinterpret_cast<PFN_vkMapMemory>(get(device, "vkMapMemory"));
      const size_t bytes = 64u << 20; void *mapped{}; check(map_memory(device, ballast[0], 0, VK_WHOLE_SIZE, 0, &mapped), "Map ballast");
      std::vector<uint64_t> heap(bytes / 8, 1);
      auto bandwidth = [&](const uint64_t *data) { uint64_t sum = 0; const auto start = std::chrono::steady_clock::now();
         for (size_t i = 0; i < bytes / 8; ++i) sum += data[i];
         const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
         if (sum == 42) std::cerr << ' '; return bytes / seconds / 1e9; };
      bandwidth(static_cast<const uint64_t *>(mapped));
      result["cpuReadGBpsMappedWddm"] = bandwidth(static_cast<const uint64_t *>(mapped));
      result["cpuReadGBpsHeap"] = bandwidth(heap.data());
   }
   for (auto m : ballast) vkFreeMemory(device, m, nullptr);
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
   std::cerr << "Submit cost: " << result.dump() << '\n';
   return result;
}
