/* Times dispatches of atomics.spv (64 invocations x 512 same-address atomics
 * per group) for several group counts. Usage: atomics.exe atomics.spv */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
   FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(size); fread(code, 1, size, f); fclose(f);
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &instance));
   uint32_t n = 1; VkPhysicalDevice pd; ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   float priority = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d; CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, NULL, &d));
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define F(name) PFN_##name name = (PFN_##name)gdpa(d, #name)
   F(vkGetDeviceQueue); F(vkCreateBuffer); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateShaderModule);
   F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateComputePipelines); F(vkCreateDescriptorPool);
   F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCreateCommandPool); F(vkAllocateCommandBuffers);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets); F(vkCmdDispatch);
   F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkResetCommandBuffer);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer; CHECK(vkCreateBuffer(d, &bi, NULL, &buffer));
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 4096;
   VkDeviceMemory memory; CHECK(vkAllocateMemory(d, &ai, NULL, &memory)); CHECK(vkBindBufferMemory(d, buffer, memory, 0));
   volatile uint32_t *map; CHECK(vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, (void **)&map));
   VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = size; si.pCode = code;
   VkShaderModule module; CHECK(vkCreateShaderModule(d, &si, NULL, &module));
   VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(d, &li, NULL, &sl));
   VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &sl;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, NULL, &layout));
   VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.layout = layout;
   cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = module; cpi.stage.pName = "main";
   VkPipeline pipeline; CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dpool; CHECK(vkCreateDescriptorPool(d, &dpi, NULL, &dpool));
   VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &sl;
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
   VkDescriptorBufferInfo dbi = {buffer, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
   vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
   VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, NULL, &pool));
   VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   const uint32_t groups[] = {16, 64, 256, 1024};
   LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
   for (int k = 0; k < 4; ++k) {
      map[0] = 0;
      VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkResetCommandBuffer(cmd, 0)); CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
      vkCmdDispatch(cmd, groups[k], 1, 1); CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
      LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      QueryPerformanceCounter(&t1);
      const double seconds = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
      const double atomics = groups[k] * 64.0 * 512.0;
      const uint32_t expected = (uint32_t)(groups[k] * 64u * (511u * 512u / 2u));
      printf("groups=%u atomics=%.0f time=%.3f s rate=%.1f M/s counter %s\n", groups[k], atomics, seconds, atomics / seconds / 1e6,
             map[0] == expected ? "exact" : "WRONG");
   }
   return 0;
}
