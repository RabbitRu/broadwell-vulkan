/* Submission cost: wall time per vkQueueSubmit+wait for an almost empty
 * command buffer, with little and with much live device memory, and per
 * command buffer when several go in one vkQueueSubmit. Diagnostic numbers;
 * the only assertion is that every submission completes. */
#include <chrono>
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
