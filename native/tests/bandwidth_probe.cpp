/* GPU memory throughput on this ICD, timed by GPU timestamps: vkCmdFillBuffer
 * and vkCmdCopyBuffer over large buffers, for device-local (type 1) and
 * host-visible (type 0) memory. Shows whether GPU access to our WDDM
 * allocations runs at DRAM speed (dual-channel DDR3: ~25 GB/s peak).
 * Usage: bandwidth_probe.exe */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <stdexcept>
#include <string>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

int main()
{
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   VkPhysicalDeviceProperties props; ((PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties"))(pd, &props);
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) auto name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdFillBuffer); F(vkCmdCopyBuffer); F(vkCmdPipelineBarrier);
   F(vkCreateQueryPool); F(vkCmdResetQueryPool); F(vkCmdWriteTimestamp); F(vkGetQueryPoolResults); F(vkQueueSubmit); F(vkQueueWaitIdle);
   F(vkDestroyBuffer); F(vkFreeMemory);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   const VkDeviceSize bytes = 128ull << 20;
   for (uint32_t type : {1u, 0u}) {
      VkBuffer b[2]; VkDeviceMemory m[2];
      for (int i = 0; i < 2; ++i) {
         VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
         CHECK(vkCreateBuffer(d, &bi, nullptr, &b[i]));
         VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = bytes; ai.memoryTypeIndex = type;
         CHECK(vkAllocateMemory(d, &ai, nullptr, &m[i])); CHECK(vkBindBufferMemory(d, b[i], m[i], 0));
      }
      for (int op = 0; op < 2; ++op) {
         double best = 1e30;
         for (int rep = 0; rep < 5; ++rep) {
            CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
            vkCmdResetQueryPool(cmd, qp, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
            if (op == 0) vkCmdFillBuffer(cmd, b[0], 0, bytes, 0x12345678u);
            else { VkBufferCopy region{0, 0, bytes}; vkCmdCopyBuffer(cmd, b[0], b[1], 1, &region); }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
            CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
            CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
            uint64_t ts[2]; CHECK(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
            if (ms < best) best = ms;
         }
         const double moved = op == 0 ? bytes : 2.0 * bytes;   /* copy reads and writes */
         printf("memory type %u %s 128 MiB: %.2f ms -> %.1f GB/s (%s)\n", type, op == 0 ? "fill" : "copy", best, moved / (best / 1000) / 1e9,
                op == 0 ? "written" : "read+written");
      }
      for (int i = 0; i < 2; ++i) { vkDestroyBuffer(d, b[i], nullptr); vkFreeMemory(d, m[i], nullptr); }
   }
   return 0;
}
