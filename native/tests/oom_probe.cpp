/* Device memory exhaustion outside CTS, on this ICD:
 *  1. vkAllocateMemory in 64 MiB, then 1 MiB, then 64 KiB steps until it
 *     fails; every failure must be VK_ERROR_OUT_OF_DEVICE_MEMORY and the
 *     total must reach most of the reported heap;
 *  2. with the heap full, a command buffer recorded beforehand is submitted
 *     and its fill checked, and thousands of new commands (new command blocks,
 *     state pool growth: driver memory, outside the application heap) are
 *     recorded and run correctly;
 *  3. after freeing everything, fresh work runs correctly and the same
 *     amount of memory can be allocated again (no leaked accounting).
 * Prints one line per check and "PASS"/"FAIL"; exit code 0 on success.
 * Usage: oom_probe.exe */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL: %s = %d\n", #x, r_); return 1; } } while (0)

int main()
{
   setvbuf(stdout, nullptr, _IONBF, 0);
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   if (!loader) { printf("FAIL: no vulkan-1.dll\n"); return 1; }
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; CHECK(((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd));
   VkPhysicalDeviceMemoryProperties mp; ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
   const uint64_t heap = mp.memoryHeaps[0].size;
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d));
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) auto name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkAllocateMemory); F(vkFreeMemory); F(vkCreateBuffer); F(vkDestroyBuffer); F(vkBindBufferMemory);
   F(vkMapMemory); F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkFreeCommandBuffers); F(vkBeginCommandBuffer);
   F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdFillBuffer); F(vkCmdPipelineBarrier); F(vkQueueSubmit); F(vkQueueWaitIdle);
   F(vkDeviceWaitIdle); F(vkDestroyCommandPool); F(vkDestroyDevice);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));

   /* A small target buffer and two command buffers, made before the heap fills. */
   struct Target { VkBuffer buffer; VkDeviceMemory memory; uint32_t *map; };
   auto make_target = [&](Target &t) -> VkResult {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 65536; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkResult r = vkCreateBuffer(d, &bi, nullptr, &t.buffer); if (r) return r;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 65536;
      r = vkAllocateMemory(d, &ai, nullptr, &t.memory); if (r) return r;
      r = vkBindBufferMemory(d, t.buffer, t.memory, 0); if (r) return r;
      void *map; r = vkMapMemory(d, t.memory, 0, VK_WHOLE_SIZE, 0, &map); t.map = (uint32_t *)map;
      memset(map, 0, 65536); return r;
   };
   auto record_fill = [&](VkCommandBuffer cmd, VkBuffer buffer, uint32_t value, uint32_t repeats) -> VkResult {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      VkResult r = vkBeginCommandBuffer(cmd, &begin); if (r) return r;
      for (uint32_t i = 0; i < repeats; ++i) vkCmdFillBuffer(cmd, buffer, (i % 64) * 1024, 1024, value + (i % 64));
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
      return vkEndCommandBuffer(cmd);
   };
   auto submit = [&](VkCommandBuffer cmd) -> VkResult {
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
      VkResult r = vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE); if (r) return r;
      return vkQueueWaitIdle(queue);
   };
   auto filled = [](const Target &t, uint32_t value) {
      for (uint32_t i = 0; i < 16384; ++i) if (t.map[i] != value + i / 256) return false;
      return true;
   };
   Target target; CHECK(make_target(target));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 2;
   VkCommandBuffer cmds[2]; CHECK(vkAllocateCommandBuffers(d, &ca, cmds));
   CHECK(record_fill(cmds[0], target.buffer, 0x1000, 64));

   /* 1. Fill the heap. */
   const DWORD t0 = GetTickCount();
   std::vector<VkDeviceMemory> blocks;
   uint64_t total = 0; int bad_errors = 0, failures_seen = 0;
   auto fill = [&]() {
      blocks.clear(); total = 0;
      for (uint64_t step : {64ull << 20, 1ull << 20, 64ull << 10}) {
         for (;;) {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = step;
            VkDeviceMemory m; const VkResult r = vkAllocateMemory(d, &ai, nullptr, &m);
            if (r == VK_SUCCESS) { blocks.push_back(m); total += step; continue; }
            ++failures_seen;
            if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY) { ++bad_errors; printf("  allocation of %llu bytes returned %d\n", (unsigned long long)step, r); }
            break;
         }
      }
   };
   fill();
   const uint64_t first_total = total;
   printf("filled: %zu allocations, %.1f MiB of a %.1f MiB heap in %lu ms\n", blocks.size(), total / 1048576.0, heap / 1048576.0,
          GetTickCount() - t0);
   EXPECT(bad_errors == 0, "a failed allocation returned something other than VK_ERROR_OUT_OF_DEVICE_MEMORY");
   EXPECT(total >= heap * 9 / 10, "only %.1f MiB of %.1f MiB could be allocated", total / 1048576.0, heap / 1048576.0);

   /* 2. Work with the heap full. */
   VkResult r = submit(cmds[0]);
   EXPECT(r == VK_SUCCESS, "submitting a pre-recorded command buffer with the heap full returned %d", r);
   EXPECT(r != VK_SUCCESS || filled(target, 0x1000), "fill with the heap full produced wrong data");
   printf("pre-recorded submission with a full heap: %s\n", r == VK_SUCCESS && filled(target, 0x1000) ? "correct" : "wrong");
   r = record_fill(cmds[1], target.buffer, 0x2000, 20000);   /* ~20000 commands: several command blocks */
   printf("recording 20000 commands with a full heap: vkEndCommandBuffer = %d\n", r);
   /* Driver-internal memory (command blocks, state pools) is not part of the
    * application heap: recording must still work and run correctly. */
   EXPECT(r == VK_SUCCESS, "recording with the application heap full returned %d", r);
   if (r == VK_SUCCESS) {
      r = submit(cmds[1]);
      EXPECT(r == VK_SUCCESS && filled(target, 0x2000), "large command buffer recorded with a full heap ran wrong (%d)", r);
      printf("20000-command buffer with a full heap: %s\n", r == VK_SUCCESS && filled(target, 0x2000) ? "correct" : "wrong");
   }
   CHECK(vkResetCommandBuffer(cmds[1], 0));

   /* 3. Free, recover, refill. */
   for (VkDeviceMemory m : blocks) vkFreeMemory(d, m, nullptr);
   r = record_fill(cmds[1], target.buffer, 0x3000, 20000);
   EXPECT(r == VK_SUCCESS, "recording after freeing returned %d", r);
   if (r == VK_SUCCESS) {
      r = submit(cmds[1]);
      EXPECT(r == VK_SUCCESS && filled(target, 0x3000), "work after freeing ran wrong (%d)", r);
      printf("20000-command buffer after freeing: %s\n", r == VK_SUCCESS && filled(target, 0x3000) ? "correct" : "wrong");
   }
   CHECK(vkResetCommandBuffer(cmds[1], 0));
   fill();
   printf("refilled: %.1f MiB (first time %.1f MiB)\n", total / 1048576.0, first_total / 1048576.0);
   EXPECT(total == first_total, "the second fill reached %.1f MiB instead of %.1f MiB (leaked accounting)",
          total / 1048576.0, first_total / 1048576.0);
   EXPECT(bad_errors == 0, "a failed allocation returned something other than VK_ERROR_OUT_OF_DEVICE_MEMORY");
   for (VkDeviceMemory m : blocks) vkFreeMemory(d, m, nullptr);
   CHECK(vkDeviceWaitIdle(d));
   vkFreeCommandBuffers(d, pool, 2, cmds); vkDestroyCommandPool(d, pool, nullptr);
   vkDestroyBuffer(d, target.buffer, nullptr); vkFreeMemory(d, target.memory, nullptr);
   vkDestroyDevice(d, nullptr);
   printf(failures ? "FAIL: %d checks failed\n" : "PASS: heap exhaustion returns VK_ERROR_OUT_OF_DEVICE_MEMORY, work survives it, memory is reusable\n",
          failures);
   return failures ? 1 : 0;
}
