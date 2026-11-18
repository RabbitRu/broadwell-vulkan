/* Sampler throughput on this ICD (sample.comp): 1024x1024 dispatch, 16
 * bilinear RGBA8 samples per invocation, timed by GPU timestamps. Compare
 * with d3d11_sample_probe (the same load on Intel's D3D11 driver).
 * Usage: sample_probe.exe <sample.spv> [reps] */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
   if (argc < 2) return 2;
   const int reps = argc > 2 ? atoi(argv[2]) : 200;
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
   F(vkGetQueryPoolResults); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory);
   F(vkCreateImageView); F(vkCreateSampler); F(vkCmdPipelineBarrier); F(vkCmdClearColorImage);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 16384; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer; CHECK(vkCreateBuffer(d, &bi, nullptr, &buffer));
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 16384; VkDeviceMemory mem; CHECK(vkAllocateMemory(d, &ai, nullptr, &mem));
   CHECK(vkBindBufferMemory(d, buffer, mem, 0));
   VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
   ii.extent = {1024, 1024, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
   ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   VkImage image; CHECK(vkCreateImage(d, &ii, nullptr, &image));
   VkMemoryRequirements req; vkGetImageMemoryRequirements(d, image, &req);
   ai.allocationSize = req.size; VkDeviceMemory imem; CHECK(vkAllocateMemory(d, &ai, nullptr, &imem)); CHECK(vkBindImageMemory(d, image, imem, 0));
   VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
   vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageView view; CHECK(vkCreateImageView(d, &vi, nullptr, &view));
   VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
   VkSampler sampler; CHECK(vkCreateSampler(d, &sci, nullptr, &sampler));
   VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = code.size() * 4; si.pCode = code.data();
   VkShaderModule module; CHECK(vkCreateShaderModule(d, &si, nullptr, &module));
   VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = bindings;
   VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(d, &li, nullptr, &sl));
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &sl;
   VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d, &pli, nullptr, &layout));
   VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.layout = layout;
   cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
   VkPipeline pipeline; CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
   VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(d, &dpi, nullptr, &dp));
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &sl;
   VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d, &dai, &set));
   VkDescriptorImageInfo dii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkDescriptorBufferInfo dbi{buffer, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
   w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &dii;
   w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &dbi;
   vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
   VkQueryPool qp; CHECK(vkCreateQueryPool(d, &qi, nullptr, &qp));
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   {  /* fill the texture with a pattern-free colour and make it sampleable */
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = image; b.subresourceRange = vi.subresourceRange;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkClearColorValue color{}; color.float32[0] = 0.25f; color.float32[1] = 0.5f; color.float32[2] = 0.75f; color.float32[3] = 1.0f;
      vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &vi.subresourceRange);
      b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
   }
   for (int rep = 0; rep < reps; ++rep) {
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, qp, 0, 2);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
      vkCmdDispatch(cmd, 128, 128, 1);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      CHECK(vkEndCommandBuffer(cmd));
      CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
      uint64_t ts[2]; CHECK(vkGetQueryPoolResults(d, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      const double ms = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
      if (reps <= 20 || rep % (reps / 20) == 0)
         printf("run %d: %.3f ms -> %.2f Gsamples/s\n", rep, ms, 1024.0 * 1024 * 16 / (ms / 1000) / 1e9);
   }
   return 0;
}
