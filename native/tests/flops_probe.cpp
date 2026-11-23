/* GPU ALU throughput on this ICD (flops.comp), timed by GPU timestamps.
 * Iris 6100 peak: 48 EUs x 2 FPUs x SIMD4 x 2 FLOP (FMA) = 768 FLOP/clock,
 * ~770-840 GFLOPS at 1.0-1.1 GHz. Far less means a low GPU clock.
 * Usage: flops_probe.exe <flops.spv> */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
   if (argc < 2) return 2;
   FILE *file = fopen(argv[1], "rb"); fseek(file, 0, SEEK_END); long size = ftell(file); fseek(file, 0, SEEK_SET);
   std::vector<uint32_t> code(size / 4); fread(code.data(), 1, size, file); fclose(file);
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
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkCreateShaderModule); F(vkCreateDescriptorSetLayout);
   F(vkCreatePipelineLayout); F(vkCreateComputePipelines); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
   F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer);
   F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets); F(vkCmdDispatch); F(vkCreateQueryPool); F(vkCmdResetQueryPool); F(vkCmdWriteTimestamp);
   F(vkGetQueryPoolResults); F(vkQueueSubmit); F(vkQueueWaitIdle);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer; CHECK(vkCreateBuffer(d, &bi, nullptr, &buffer));
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 4096; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem));
   CHECK(vkBindBufferMemory(d, buffer, mem, 0));
   VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = code.size() * 4; si.pCode = code.data();
   VkShaderModule module; CHECK(vkCreateShaderModule(d, &si, nullptr, &module));
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &sl));
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &sl;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));
   VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.layout = layout;
   cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
   VkPipeline pipeline; CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
   VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &sl;
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
   VkDescriptorBufferInfo dbi{buffer, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
   vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   const bool clock_mode = argc > 3 && !strcmp(argv[3], "clock");
   const uint32_t groups = clock_mode ? 1 : 4096;   /* 262144 invocations */
   const int reps = argc > 2 ? atoi(argv[2]) : 6;
   void *mapped = nullptr;
   if (clock_mode) { F(vkMapMemory); CHECK(vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &mapped)); }
   for (int rep = 0; rep < reps; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, 2);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
      vkCmdDispatch(cmd, groups, 1, 1);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      uint64_t ts[2]; CHECK(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      if (clock_mode) {
         const uint64_t ticks = ((const uint32_t *)mapped)[0] | (uint64_t)((const uint32_t *)mapped)[1] << 32;
         printf("run %d: %.2f ms, %llu shader ticks -> %.0f MHz tick rate\n", rep, ms, (unsigned long long)ticks,
                ticks / (ms * 1000));
         continue;
      }
      const double flop = groups * 64.0 * 512 * 8 * 2;
      if (reps <= 20 || rep % (reps / 20) == 0)
         printf("run %d: %.2f ms -> %.0f GFLOPS\n", rep, ms, flop / (ms / 1000) / 1e9);
   }
   if (!clock_mode) {
      /* The values: 1024 invocations, one per element, against the CPU. */
      F(vkMapMemory); CHECK(vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &mapped));
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      vkCmdDispatch(cmd, 16, 1, 1);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      static const float offsets[8] = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
      double worst = 0;
      for (uint32_t x = 0; x < 1024; ++x) {
         float chain[8], sum = 0;
         for (int j = 0; j < 8; ++j) chain[j] = float(x) * 1e-7f + offsets[j];
         for (int i = 0; i < 512; ++i) for (int j = 0; j < 8; ++j) chain[j] = fmaf(chain[j], 0.9999f, 0.0001f);
         for (int j = 0; j < 8; ++j) sum += chain[j];
         worst = std::max(worst, (double)fabsf(((const float *)mapped)[x] - sum) / sum);
      }
      printf("check: worst relative error %.3g -> %s\n", worst, worst < 1e-5 ? "OK" : "WRONG");
      if (!(worst < 1e-5)) return 1;
   }
   return 0;
}
