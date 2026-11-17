/* Device loss on a real GPU hang, and recovery without restarting the process.
 *
 * A compute shader runs about 4e9 atomic additions (hang.comp), far beyond the
 * 2-second Windows TDR, so Windows resets the GPU. Expected: the fence wait
 * returns VK_ERROR_DEVICE_LOST, later calls report the loss without blocking,
 * the device can be destroyed, and a new VkDevice in the same process works.
 *
 * Usage: IrisDeviceLostProbe.exe <report.json> <hang.spv> */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
using json = nlohmann::json;
static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
static void check(VkResult result, const char *name) { if (result != VK_SUCCESS) throw std::runtime_error(std::string(name) + ": " + std::to_string(result)); }

struct Instance {
   PFN_vkGetInstanceProcAddr gipa{}; VkInstance instance{}; VkPhysicalDevice physical{};
   PFN_vkCreateDevice create_device{}; PFN_vkGetDeviceProcAddr gdpa{};
};
static VkDevice create_device(Instance &in, bool dynamic_rendering)
{
   float priority = 1;
   VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
   VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
   static const char *const extensions[] = {"VK_KHR_dynamic_rendering"};
   VkPhysicalDeviceDynamicRenderingFeatures dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES}; dr.dynamicRendering = VK_TRUE;
   if (dynamic_rendering) { di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = extensions; di.pNext = &dr; }
   VkDevice device{}; check(in.create_device(in.physical, &di, nullptr, &device), "Create device");
   return device;
}
#define API(name) auto name = reinterpret_cast<PFN_##name>(in.gdpa(device, #name)); require(name != nullptr, "Missing " #name)

/* Fill a buffer on the GPU and read it back: the device works. */
static void check_working(Instance &in, VkDevice device)
{
   API(vkGetDeviceQueue); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkAllocateMemory); API(vkFreeMemory); API(vkBindBufferMemory);
   API(vkMapMemory); API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer);
   API(vkEndCommandBuffer); API(vkCmdFillBuffer); API(vkQueueSubmit); API(vkQueueWaitIdle);
   VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Buffer");
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 4096;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Memory"); check(vkBindBufferMemory(device, buffer, memory, 0), "Bind");
   void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map");
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{}; check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
   VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
   vkCmdFillBuffer(cmd, buffer, 0, 4096, 0x600d0001u); check(vkEndCommandBuffer(cmd), "End");
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE), "Submit"); check(vkQueueWaitIdle(queue), "Wait idle");
   const uint32_t *words = static_cast<const uint32_t *>(map);
   for (unsigned i = 0; i < 1024; ++i) require(words[i] == 0x600d0001u, "Fill after recovery differs");
   vkDestroyCommandPool(device, pool, nullptr); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
}

/* Dispatches the endless compute shader. */
static json hang(Instance &in, VkDevice device, const std::vector<uint32_t> &spirv)
{
   API(vkGetDeviceQueue); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkAllocateMemory); API(vkFreeMemory); API(vkBindBufferMemory);
   API(vkMapMemory); API(vkCreateShaderModule); API(vkDestroyShaderModule); API(vkCreateDescriptorSetLayout); API(vkDestroyDescriptorSetLayout);
   API(vkCreatePipelineLayout); API(vkDestroyPipelineLayout); API(vkCreateComputePipelines); API(vkDestroyPipeline);
   API(vkCreateDescriptorPool); API(vkDestroyDescriptorPool); API(vkAllocateDescriptorSets); API(vkUpdateDescriptorSets);
   API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer);
   API(vkCmdBindPipeline); API(vkCmdBindDescriptorSets); API(vkCmdDispatch); API(vkCreateFence); API(vkDestroyFence);
   API(vkQueueSubmit); API(vkWaitForFences); API(vkGetFenceStatus); API(vkDeviceWaitIdle);
   VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 4096; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Buffer");
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 4096;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Memory"); check(vkBindBufferMemory(device, buffer, memory, 0), "Bind");
   void *map{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map), "Map");
   static_cast<uint32_t *>(map)[0] = 0xffffffffu; static_cast<uint32_t *>(map)[1] = 0;   // iterations, counter
   VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = spirv.size() * 4; si.pCode = spirv.data();
   VkShaderModule module{}; check(vkCreateShaderModule(device, &si, nullptr, &module), "Shader");
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout set_layout{}; check(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout), "Set layout");
   VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &set_layout;
   VkPipelineLayout layout{}; check(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "Pipeline layout");
   VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cpi.layout = layout;
   cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
   VkPipeline pipeline{}; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline), "Pipeline");
   VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
   VkDescriptorPool dpool{}; check(vkCreateDescriptorPool(device, &dpi, nullptr, &dpool), "Descriptor pool");
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &set_layout;
   VkDescriptorSet set{}; check(vkAllocateDescriptorSets(device, &dai, &set), "Descriptor set");
   VkDescriptorBufferInfo dbi{buffer, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = set; write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &dbi;
   vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{}; check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
   VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
   vkCmdDispatch(cmd, 1, 1, 1);
   check(vkEndCommandBuffer(cmd), "End");
   VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fc, nullptr, &fence), "Fence");
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &s, fence), "Hang submit");
   const auto start = std::chrono::steady_clock::now();
   const VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull);
   const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
   std::cerr << "fence wait after hang: " << waited << " in " << seconds << " s\n";
   json result{{"fenceWait", waited}, {"seconds", seconds}};
   /* After the loss: further calls report it and nothing blocks. */
   VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
   result["submitAfterLoss"] = vkQueueSubmit(queue, 1, &empty, VK_NULL_HANDLE);
   result["deviceWaitIdleAfterLoss"] = vkDeviceWaitIdle(device);
   result["fenceStatusAfterLoss"] = vkGetFenceStatus(device, fence);
   result["counterAtLoss"] = static_cast<uint32_t *>(map)[1];
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr); vkDestroyDescriptorPool(device, dpool, nullptr);
   vkDestroyPipeline(device, pipeline, nullptr); vkDestroyPipelineLayout(device, layout, nullptr);
   vkDestroyDescriptorSetLayout(device, set_layout, nullptr); vkDestroyShaderModule(device, module, nullptr);
   vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
   return result;
}
#undef API

int wmain(int argc, wchar_t **argv)
{
   if (argc != 3) return 2;
   json report{{"passed", false}};
   int status = 1;
   try {
      std::ifstream file(argv[2], std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      require(!bytes.empty() && bytes.size() % 4 == 0, "Cannot read hang.spv");
      std::vector<uint32_t> spirv(bytes.size() / 4); memcpy(spirv.data(), bytes.data(), bytes.size());
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader missing");
      Instance in;
      in.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      auto create = reinterpret_cast<PFN_vkCreateInstance>(in.gipa(VK_NULL_HANDLE, "vkCreateInstance"));
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
      VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
      check(create(&ci, nullptr, &in.instance), "Create instance");
      auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(in.gipa(in.instance, "vkEnumeratePhysicalDevices"));
      in.create_device = reinterpret_cast<PFN_vkCreateDevice>(in.gipa(in.instance, "vkCreateDevice"));
      in.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(in.gipa(in.instance, "vkGetDeviceProcAddr"));
      uint32_t count = 1; check(enumerate(in.instance, &count, &in.physical), "Enumerate");
      VkDevice first = create_device(in, false);
      check_working(in, first);
      report["beforeHang"] = "working";
      report["hang"] = hang(in, first, spirv);
      reinterpret_cast<PFN_vkDestroyDevice>(in.gdpa(first, "vkDestroyDevice"))(first, nullptr);
      report["lostDeviceDestroyed"] = true;
      VkDevice second = create_device(in, false);
      check_working(in, second);
      report["newDeviceAfterLoss"] = "working";
      reinterpret_cast<PFN_vkDestroyDevice>(in.gdpa(second, "vkDestroyDevice"))(second, nullptr);
      reinterpret_cast<PFN_vkDestroyInstance>(in.gipa(in.instance, "vkDestroyInstance"))(in.instance, nullptr);
      const json &h = report["hang"];
      report["passed"] = h["fenceWait"] == VK_ERROR_DEVICE_LOST && h["seconds"].get<double>() < 20 &&
         h["submitAfterLoss"] == VK_ERROR_DEVICE_LOST && h["deviceWaitIdleAfterLoss"] == VK_ERROR_DEVICE_LOST;
      status = report["passed"] ? 0 : 1;
   } catch (const std::exception &e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
   std::ofstream(argv[1]) << report.dump(2) << '\n';
   std::cout << report.dump(2) << '\n';
   return status;
}
