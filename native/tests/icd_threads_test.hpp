/* Thread safety: several threads record command buffers in parallel (each
 * with its own pool, as Vulkan requires), allocate and free memory, buffers
 * and events, and submit to the one queue under an application mutex (queue
 * access is externally synchronized). Each thread checks its own results. */
#include <mutex>
#include <thread>
static json test_threads(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define API(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkFreeCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdFillBuffer); API(vkCmdUpdateBuffer); API(vkCmdPipelineBarrier);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences);
   API(vkAllocateMemory); API(vkFreeMemory); API(vkMapMemory); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkBindBufferMemory);
   API(vkCreateEvent); API(vkDestroyEvent); API(vkSetEvent); API(vkGetEventStatus);
#undef API
   constexpr unsigned threads = 4, iterations = 40;
   std::mutex queue_mutex;
   std::vector<std::string> errors(threads);
   std::vector<unsigned> verified(threads);
   auto worker = [&](unsigned t) {
      try {
         VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
         check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "Thread pool");
         for (unsigned i = 0; i < iterations; ++i) {
            /* Fresh memory, buffer and event every iteration: BO and event allocation race with the other threads. */
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 65536;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Thread buffer");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 65536;
            VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Thread memory");
            check(vkBindBufferMemory(device, buffer, memory, 0), "Thread bind");
            void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Thread map");
            VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; VkEvent event{}; check(vkCreateEvent(device, &ei, nullptr, &event), "Thread event");
            check(vkSetEvent(device, event), "Thread set event");
            require(vkGetEventStatus(device, event) == VK_EVENT_SET, "Thread event not set");
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
            VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Thread command buffer");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Thread begin");
            const uint32_t value = (t + 1) << 24 | i;
            /* Many fills make the batch grow past one 64 KiB chunk now and then. */
            for (unsigned k = 0; k < 256; ++k) vkCmdFillBuffer(cmd, buffer, 256ull * k, 256, value + k);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            const uint32_t marker[4] = {value, ~value, t, i};
            vkCmdUpdateBuffer(cmd, buffer, 0, sizeof(marker), marker);
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(cmd), "Thread end");
            VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Thread fence");
            {
               std::lock_guard<std::mutex> lock(queue_mutex);
               VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
               check(vkQueueSubmit(queue, 1, &s, fence), "Thread submit");
            }
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "Thread wait");
            const uint32_t *words = static_cast<const uint32_t *>(map);
            require(words[0] == value && words[1] == ~value && words[2] == t && words[3] == i, "Thread marker differs");
            for (unsigned k = 1; k < 256; ++k) require(words[64 * k] == value + k && words[64 * k + 63] == value + k, "Thread fill differs");
            vkDestroyFence(device, fence, nullptr); vkFreeCommandBuffers(device, pool, 1, &cmd);
            vkDestroyEvent(device, event, nullptr); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
            ++verified[t];
         }
         vkDestroyCommandPool(device, pool, nullptr);
      } catch (const std::exception &e) { errors[t] = e.what(); }
   };
   std::vector<std::thread> pool;
   for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker, t);
   for (auto &thread : pool) thread.join();
   unsigned total = 0;
   for (unsigned t = 0; t < threads; ++t) {
      if (!errors[t].empty()) throw std::runtime_error("Thread " + std::to_string(t) + ": " + errors[t]);
      total += verified[t];
   }
   return {{"threads", threads}, {"submissionsVerified", total}, {"passed", total == threads * iterations}};
}
