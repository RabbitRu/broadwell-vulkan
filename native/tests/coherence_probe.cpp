/* Is GPU access to host-visible memory coherent with the CPU caches?
 *
 * The driver does no CPU cache write-back before submissions and no invalidation
 * after completion. For buffers that stay in
 * L2 (64 KiB), L3 (1 MiB) and exceed it (4 MiB):
 *  - CPU writes a fresh pattern into src (cache-hot), CPU reads dst (its lines
 *    cached with old data), the GPU copies src to dst, the CPU reads dst:
 *    both directions must see the other side's latest writes;
 *  - the CPU writes dst (dirty lines), the GPU fills it with a new value, the
 *    CPU touches other memory and reads dst: no stale or written-back word.
 * Any stale word is a mismatch.
 *
 * Usage: IrisCoherenceProbe.exe <report.json> */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using json = nlohmann::json;
static void require(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
static void check(VkResult result, const std::string &name) { if (result != VK_SUCCESS) throw std::runtime_error(name + ": " + std::to_string(result)); }

int wmain(int argc, wchar_t **argv)
{
   if (argc != 2) return 2;
   json report{{"passed", false}};
   int status = 1;
   try {
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader missing");
      auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
      VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
      VkInstance instance{}; check(reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance), "Instance");
      uint32_t count = 1; VkPhysicalDevice physical{};
      check(reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &count, &physical), "Enumerate");
      float priority = 1;
      VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
      VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
      VkDevice device{}; check(reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(physical, &di, nullptr, &device), "Device");
      auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));
#define API(name) auto name = reinterpret_cast<PFN_##name>(gdpa(device, #name)); require(name != nullptr, "Missing " #name)
      API(vkGetDeviceQueue); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkAllocateMemory); API(vkFreeMemory); API(vkBindBufferMemory);
      API(vkMapMemory); API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer);
      API(vkEndCommandBuffer); API(vkResetCommandBuffer); API(vkCmdCopyBuffer); API(vkCmdFillBuffer); API(vkCmdPipelineBarrier);
      API(vkCreateFence); API(vkDestroyFence); API(vkResetFences); API(vkQueueSubmit); API(vkWaitForFences); API(vkDestroyDevice);
#undef API
      VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
      VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      VkCommandPool pool{}; check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
      VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fi, nullptr, &fence), "Fence");
      json cases = json::array(); uint64_t total_mismatches = 0;
      for (const VkDeviceSize bytes : {VkDeviceSize(64) << 10, VkDeviceSize(1) << 20, VkDeviceSize(4) << 20}) {
         VkBuffer buffers[2]{}; VkDeviceMemory memory[2]{}; volatile uint32_t *maps[2]{};
         for (int b = 0; b < 2; ++b) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            check(vkCreateBuffer(device, &bi, nullptr, &buffers[b]), "Buffer");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes; ai.memoryTypeIndex = 0;   /* HOST_VISIBLE|COHERENT|CACHED */
            check(vkAllocateMemory(device, &ai, nullptr, &memory[b]), "Memory"); check(vkBindBufferMemory(device, buffers[b], memory[b], 0), "Bind");
            void *map{}; check(vkMapMemory(device, memory[b], 0, VK_WHOLE_SIZE, 0, &map), "Map"); maps[b] = static_cast<volatile uint32_t *>(map);
         }
         const size_t words = static_cast<size_t>(bytes / 4);
         uint64_t mismatches = 0, sink = 0;
         const unsigned iterations = bytes <= (1u << 20) ? 200 : 60;
         const auto start = std::chrono::steady_clock::now();
         auto run = [&](bool copy, uint32_t value) {
            check(vkResetCommandBuffer(cmd, 0), "Reset");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
            if (copy) { VkBufferCopy region{0, 0, bytes}; vkCmdCopyBuffer(cmd, buffers[0], buffers[1], 1, &region); }
            else vkCmdFillBuffer(cmd, buffers[1], 0, bytes, value);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(cmd), "End");
            VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
            check(vkResetFences(device, 1, &fence), "Reset fence"); check(vkQueueSubmit(queue, 1, &s, fence), "Submit");
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "Wait");
         };
         for (unsigned i = 0; i < iterations; ++i) {
            /* CPU -> GPU -> CPU through a copy. */
            const uint32_t seed = 0x9e3779b9u * (i + 1) ^ static_cast<uint32_t>(bytes);
            for (size_t w = 0; w < words; ++w) maps[0][w] = seed + static_cast<uint32_t>(w) * 2654435761u;
            for (size_t w = 0; w < words; ++w) sink += maps[1][w];   /* stale lines of dst in the CPU caches */
            run(true, 0);
            for (size_t w = 0; w < words; ++w) if (maps[1][w] != seed + static_cast<uint32_t>(w) * 2654435761u) ++mismatches;
            /* GPU write over lines the CPU just wrote (dirty in its caches): a
             * late write-back of those lines would overwrite the GPU's data. */
            const uint32_t value = 0x5a000000u | i;
            for (size_t w = 0; w < words; ++w) maps[1][w] = 0xdeadbeefu ^ static_cast<uint32_t>(w);
            run(false, value);
            for (size_t w = 0; w < words; ++w) sink += maps[0][w];   /* push other lines through the caches */
            for (size_t w = 0; w < words; ++w) if (maps[1][w] != value) ++mismatches;
         }
         const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
         cases.push_back({{"bytes", bytes}, {"iterations", iterations}, {"mismatches", mismatches}, {"seconds", seconds}, {"sink", sink & 1}});
         total_mismatches += mismatches;
         for (int b = 0; b < 2; ++b) { vkDestroyBuffer(device, buffers[b], nullptr); vkFreeMemory(device, memory[b], nullptr); }
      }
      report["cases"] = cases; report["mismatches"] = total_mismatches;
      vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr);
      reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
      report["passed"] = total_mismatches == 0;
      status = total_mismatches == 0 ? 0 : 1;
   } catch (const std::exception &e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
   std::ofstream(argv[1]) << report.dump(2) << '\n';
   std::cout << report.dump(2) << '\n';
   return status;
}
