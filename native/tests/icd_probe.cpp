#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <tlhelp32.h>
#include <vulkan/vulkan.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cwctype>
#include <vector>
using json = nlohmann::json;
namespace fs = std::filesystem;
static void require(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }
static void check(VkResult result, const char *name)
{ if (result != VK_SUCCESS) throw std::runtime_error(std::string(name) + ": " + std::to_string(result)); }
static json forbidden_modules()
{
   HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
   require(snapshot != INVALID_HANDLE_VALUE, "Module snapshot failed");
   MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
   if (!Module32FirstW(snapshot, &entry)) { CloseHandle(snapshot); throw std::runtime_error("Cannot enumerate modules"); }
   json found = json::array();
   do {
      std::wstring name(entry.szModule); std::transform(name.begin(), name.end(), name.begin(), towlower);
      if (name.rfind(L"igd", 0) == 0 || name.rfind(L"d3d12", 0) == 0) found.push_back(fs::path(name).u8string());
   } while (Module32NextW(snapshot, &entry));
   DWORD error = GetLastError(); CloseHandle(snapshot);
   require(error == ERROR_NO_MORE_FILES, "Module enumeration incomplete");
   return found;
}
static unsigned test_memory(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get)
{
#define MEMORY(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   MEMORY(vkCreateBuffer); MEMORY(vkDestroyBuffer); MEMORY(vkGetBufferMemoryRequirements);
   MEMORY(vkAllocateMemory); MEMORY(vkFreeMemory); MEMORY(vkBindBufferMemory);
   MEMORY(vkMapMemory); MEMORY(vkUnmapMemory); MEMORY(vkFlushMappedMemoryRanges); MEMORY(vkInvalidateMappedMemoryRanges);
   MEMORY(vkGetDeviceMemoryCommitment); MEMORY(vkQueueSubmit); MEMORY(vkQueueWaitIdle);
#undef MEMORY
   using MemoryStats = int (*)(VkDevice, uint64_t *, uint32_t *);
   auto stats = reinterpret_cast<MemoryStats>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_query_memory"));
   require(stats != nullptr, "Missing WDDM allocation diagnostics");
   uint64_t baseline_bytes{}; uint32_t baseline_count{};
   require(!stats(device, &baseline_bytes, &baseline_count), "Cannot query initial WDDM allocation usage");
   const uint64_t sizes[] = {1, 15, 17, 63, 4095, 4096, 4097, 65535, 65536, 65537, 1048589, 4194287};
   struct Allocation { VkBuffer buffers[2]{}; VkDeviceMemory memory{}; uint8_t *map{}; uint64_t size{}; uint8_t salt{}; };
   unsigned cases = 0;
   for (unsigned cycle = 0; cycle < 2; ++cycle) {
      std::vector<Allocation> live;
      uint64_t expected_bytes = baseline_bytes;
      for (uint64_t size : sizes) {
         Allocation allocation{}; allocation.salt = static_cast<uint8_t>(size ^ (cycle * 71));
         VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = size;
         bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
         for (auto &buffer : allocation.buffers) check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Create Vulkan buffer");
         VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, allocation.buffers[0], &requirements);
         require(requirements.size >= size && requirements.alignment >= 16 && requirements.memoryTypeBits == 1, "Invalid buffer requirements");
         const uint64_t stride = (requirements.size + requirements.alignment - 1) & ~(requirements.alignment - 1);
         allocation.size = 3 * stride + requirements.alignment;
         VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = allocation.size;
         check(vkAllocateMemory(device, &ai, nullptr, &allocation.memory), "Allocate real WDDM-backed Vulkan memory");
         expected_bytes += (allocation.size + 4095) & ~4095ull;
         for (unsigned i = 0; i < 2; ++i) check(vkBindBufferMemory(device, allocation.buffers[i], allocation.memory, (i + 1) * stride), "Bind buffer at nonzero offset");
         void *map{}; check(vkMapMemory(device, allocation.memory, 0, VK_WHOLE_SIZE, 0, &map), "Map Vulkan memory");
         allocation.map = static_cast<uint8_t *>(map);
         require(reinterpret_cast<uintptr_t>(map) % 4096 == 0, "KMT mapping alignment mismatch");
         for (uint64_t i = 0; i < allocation.size; ++i) allocation.map[i] = static_cast<uint8_t>((i * 37 + allocation.salt) ^ (i >> 8));
         VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = allocation.memory; range.size = VK_WHOLE_SIZE;
         check(vkFlushMappedMemoryRanges(device, 1, &range), "Flush coherent Vulkan memory");
         live.push_back(allocation);
      }
      uint64_t actual_bytes{}; uint32_t actual_count{};
      require(!stats(device, &actual_bytes, &actual_count) && actual_bytes == expected_bytes && actual_count == baseline_count + live.size(), "WDDM allocation accounting mismatch");
      VkMemoryAllocateInfo full{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; full.allocationSize = (128ull << 20) + 4096;   /* above the largest single WDDM allocation */
      VkDeviceMemory rejected{};
      require(vkAllocateMemory(device, &full, nullptr, &rejected) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !rejected, "Oversized allocation was not rejected");
      // Keep all allocations resident/mapped during a real GPU submission.
      // This tests lifetime/cache handling, not shader access to these buffers.
      VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO}; check(vkQueueSubmit(queue, 1, &empty, VK_NULL_HANDLE), "Submit with live application allocations");
      check(vkQueueWaitIdle(queue), "Wait with live application allocations");
      for (auto &allocation : live) {
         VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; range.memory = allocation.memory; range.size = VK_WHOLE_SIZE;
         check(vkInvalidateMappedMemoryRanges(device, 1, &range), "Invalidate coherent Vulkan memory");
         for (uint64_t i = 0; i < allocation.size; ++i)
            require(allocation.map[i] == static_cast<uint8_t>((i * 37 + allocation.salt) ^ (i >> 8)), "Mapped Vulkan data/guard changed");
         vkUnmapMemory(device, allocation.memory);
         void *partial{};
         check(vkMapMemory(device, allocation.memory, 16, allocation.size - 16, 0, &partial), "Remap memory at nonzero offset");
         const auto bytes = static_cast<uint8_t *>(partial);
         for (uint64_t i = 16; i < allocation.size; ++i)
            require(bytes[i - 16] == static_cast<uint8_t>((i * 37 + allocation.salt) ^ (i >> 8)), "Remapped Vulkan data mismatch");
         vkUnmapMemory(device, allocation.memory);
         for (auto buffer : allocation.buffers) vkDestroyBuffer(device, buffer, nullptr);
         vkFreeMemory(device, allocation.memory, nullptr);
         ++cases;
      }
      require(!stats(device, &actual_bytes, &actual_count) && actual_bytes == baseline_bytes && actual_count == baseline_count, "WDDM Vulkan allocations leaked");
   }
   return cases;
}
static unsigned test_descriptors(VkDevice device, PFN_vkGetDeviceProcAddr get)
{
#define DESCRIPTOR(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   DESCRIPTOR(vkCreateDescriptorSetLayout); DESCRIPTOR(vkDestroyDescriptorSetLayout);
   DESCRIPTOR(vkCreatePipelineLayout); DESCRIPTOR(vkDestroyPipelineLayout);
   DESCRIPTOR(vkCreateDescriptorPool); DESCRIPTOR(vkDestroyDescriptorPool); DESCRIPTOR(vkResetDescriptorPool);
   DESCRIPTOR(vkAllocateDescriptorSets); DESCRIPTOR(vkFreeDescriptorSets); DESCRIPTOR(vkUpdateDescriptorSets);
   DESCRIPTOR(vkCreateBuffer); DESCRIPTOR(vkDestroyBuffer); DESCRIPTOR(vkGetBufferMemoryRequirements);
   DESCRIPTOR(vkAllocateMemory); DESCRIPTOR(vkFreeMemory); DESCRIPTOR(vkBindBufferMemory);
#undef DESCRIPTOR
   using Inspect = int (*)(VkDescriptorSet, uint32_t, uint32_t, VkBuffer, uint64_t, uint64_t);
   auto inspect = reinterpret_cast<Inspect>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_check_buffer_descriptor"));
   require(inspect != nullptr, "Missing descriptor diagnostics");
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 1024;
   bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Create descriptor buffer");
   VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 8192;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate descriptor buffer memory");
   check(vkBindBufferMemory(device, buffer, memory, 4096), "Bind descriptor buffer at nonzero offset");
   VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
   };
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 2; li.pBindings = bindings;
   VkDescriptorSetLayout layout{}; check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout), "Create HasVK descriptor layout");
   VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pi.setLayoutCount = 1; pi.pSetLayouts = &layout;
   pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &push;
   VkPipelineLayout pipeline{}; check(vkCreatePipelineLayout(device, &pi, nullptr, &pipeline), "Create HasVK pipeline layout");
   VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}};
   VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
   pool_info.maxSets = 2; pool_info.poolSizeCount = 2; pool_info.pPoolSizes = pool_sizes;
   VkDescriptorPool pool{}; check(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool), "Create HasVK descriptor pool");
   unsigned checks = 0;
   for (unsigned cycle = 0; cycle < 3; ++cycle) {
      VkDescriptorSetLayout layouts[2] = {layout, layout}; VkDescriptorSet sets[2]{};
      VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool;
      allocate.descriptorSetCount = 2; allocate.pSetLayouts = layouts;
      check(vkAllocateDescriptorSets(device, &allocate, sets), "Allocate HasVK descriptor sets");
      VkDescriptorBufferInfo storage[2] = {{buffer, 16 + cycle * 16, 192}, {buffer, 256, VK_WHOLE_SIZE}};
      VkDescriptorBufferInfo uniform{buffer, 64, 256};
      VkWriteDescriptorSet writes[2]{};
      writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = sets[0]; writes[0].dstBinding = 0;
      writes[0].descriptorCount = 2; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = storage;
      writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = sets[0]; writes[1].dstBinding = 3;
      writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; writes[1].pBufferInfo = &uniform;
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
      VkCopyDescriptorSet copies[2]{};
      for (unsigned i = 0; i < 2; ++i) {
         copies[i].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET; copies[i].srcSet = sets[0]; copies[i].dstSet = sets[1];
         copies[i].srcBinding = copies[i].dstBinding = i ? 3 : 0; copies[i].descriptorCount = i ? 1 : 2;
      }
      vkUpdateDescriptorSets(device, 0, nullptr, 2, copies);
      for (auto set : sets) {
         for (unsigned i = 0; i < 2; ++i) {
            require(!inspect(set, 0, i, buffer, storage[i].offset, storage[i].range), "SSBO descriptor/address mismatch"); ++checks;
         }
         require(!inspect(set, 3, 0, buffer, uniform.offset, uniform.range), "Dynamic UBO descriptor mismatch"); ++checks;
      }
      if (cycle != 1) check(vkFreeDescriptorSets(device, pool, 2, sets), "Free descriptor sets");
      check(vkResetDescriptorPool(device, pool, 0), "Reset descriptor pool and reuse allocations");
   }
   vkDestroyDescriptorPool(device, pool, nullptr); vkDestroyPipelineLayout(device, pipeline, nullptr);
   vkDestroyDescriptorSetLayout(device, layout, nullptr); vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, memory, nullptr);
   return checks;
}
static uint32_t compute_reference(const std::string &algorithm, uint32_t value)
{
   if (algorithm == "xor32") return value ^ 0x9e3779b9u;
   if (algorithm == "lcg32") return value * 1664525u + 1013904223u;
   value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; return value ^ (value >> 16);
}
#include "icd_image_test.hpp"
static json execute_compute(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get,
   VkPipeline pipeline, VkPipelineLayout pipeline_layout, VkDescriptorSetLayout layout,
   const std::string &algorithm, unsigned local, unsigned cache_pass)
{
#define COMPUTE(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   COMPUTE(vkCreateBuffer); COMPUTE(vkDestroyBuffer); COMPUTE(vkGetBufferMemoryRequirements); COMPUTE(vkBindBufferMemory);
   COMPUTE(vkAllocateMemory); COMPUTE(vkFreeMemory); COMPUTE(vkMapMemory); COMPUTE(vkUnmapMemory);
   COMPUTE(vkCreateDescriptorPool); COMPUTE(vkDestroyDescriptorPool); COMPUTE(vkAllocateDescriptorSets); COMPUTE(vkUpdateDescriptorSets);
   COMPUTE(vkCreateCommandPool); COMPUTE(vkDestroyCommandPool); COMPUTE(vkAllocateCommandBuffers); COMPUTE(vkBeginCommandBuffer); COMPUTE(vkEndCommandBuffer);
   COMPUTE(vkCmdBindPipeline); COMPUTE(vkCmdBindDescriptorSets); COMPUTE(vkCmdDispatch); COMPUTE(vkCmdPipelineBarrier);
   COMPUTE(vkCreateFence); COMPUTE(vkDestroyFence); COMPUTE(vkResetFences); COMPUTE(vkQueueSubmit); COMPUTE(vkWaitForFences);
#undef COMPUTE
   constexpr uint64_t buffer_size = 131072, bind_offset = 4096, descriptor_offset = 64;
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = buffer_size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Create GPU compute buffer");
   VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device, buffer, &requirements);
   const uint64_t allocation_bytes = bind_offset + ((requirements.size + 4095) & ~4095ull) + 4096;
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = allocation_bytes;
   VkDeviceMemory memory{}; check(vkAllocateMemory(device, &ai, nullptr, &memory), "Allocate GPU compute memory");
   check(vkBindBufferMemory(device, buffer, memory, bind_offset), "Bind GPU compute buffer");
   void *mapped{}; check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map compute readback");
   auto words = static_cast<uint32_t *>(mapped);
   VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &pool_size;
   VkDescriptorPool descriptor_pool{}; check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool), "Create compute descriptor pool");
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = descriptor_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
   VkDescriptorSet descriptor{}; check(vkAllocateDescriptorSets(device, &dai, &descriptor), "Allocate compute descriptor set");
   VkDescriptorBufferInfo view{buffer, descriptor_offset, VK_WHOLE_SIZE};
   VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = descriptor; write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &view; vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
   VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool command_pool{}; check(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool), "Create compute command pool");
   VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = command_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
   VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &cai, &command), "Allocate HasVK command buffer");
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fi, nullptr, &fence), "Create compute fence");
   const std::vector<unsigned> counts = local == 1 ? std::vector<unsigned>{1} : std::vector<unsigned>{1, local - 1, local, local + 1, 257, 16381};
   json results = json::array();
   bool fence_used = false;
   for (unsigned count : counts) {
      std::vector<uint32_t> expected(allocation_bytes / 4, 0xcdcdef67u);
      const unsigned descriptor_start = (bind_offset + descriptor_offset) / 4;
      const unsigned data_start = descriptor_start + (local == 1 ? 0 : 16);
      if (local != 1) expected[descriptor_start] = count;
      uint32_t seed = 0x815ac629u ^ (count * 0x9e3779b9u) ^ (cache_pass * 1723) ^ local;
      for (char c : algorithm) seed = (seed ^ static_cast<uint8_t>(c)) * 16777619u;
      const uint32_t initial_seed = seed;
      for (unsigned i = 0; i < count; ++i) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; expected[data_start + i] = seed; }
      std::copy(expected.begin(), expected.end(), words);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vkBeginCommandBuffer(command, &begin), "Begin HasVK compute command buffer");
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor, 0, nullptr);
      const unsigned groups = (count + local - 1) / local;
      vkCmdDispatch(command, groups, 1, 1);
      VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = buffer; barrier.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
      check(vkEndCommandBuffer(command), "Finish HasVK compute command buffer");
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
         if (fence_used) check(vkResetFences(device, 1, &fence), "Reset compute fence");
         for (unsigned i = 0; i < count; ++i) expected[data_start + i] = compute_reference(algorithm, expected[data_start + i]);
         VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
         VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
         VkResult waited = submitted == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 20000000000ull) : submitted;
         size_t mismatch = 0;
         while (mismatch < expected.size() && words[mismatch] == expected[mismatch]) ++mismatch;
         if (waited != VK_SUCCESS || mismatch != expected.size()) {
            fs::path failure = fs::current_path() / "diagnostics" / ("compute-failure-" + std::to_string(GetTickCount64())); fs::create_directories(failure);
            std::ofstream(failure / "actual.bin", std::ios::binary).write(reinterpret_cast<const char *>(words), allocation_bytes);
            std::ofstream(failure / "expected.bin", std::ios::binary).write(reinterpret_cast<const char *>(expected.data()), allocation_bytes);
            using Batch = int (*)(VkCommandBuffer, const void **, uint32_t *);
            auto query = reinterpret_cast<Batch>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_query_command_buffer"));
            const void *data{}; uint32_t bytes{};
            if (query && !query(command, &data, &bytes)) std::ofstream(failure / "command.bin", std::ios::binary).write(static_cast<const char *>(data), bytes);
            json diagnostic = {{"algorithm", algorithm}, {"localSize", local}, {"count", count}, {"groups", groups}, {"cachePass", cache_pass},
               {"repeat", repeat}, {"inputSeed", initial_seed}, {"waitResult", waited}, {"mismatchWord", mismatch}, {"dataStartWord", data_start}};
            if (mismatch < expected.size()) { diagnostic["expectedValue"] = expected[mismatch]; diagnostic["actualValue"] = words[mismatch]; }
            std::ofstream(failure / "failure.json") << diagnostic.dump(2);
            throw std::runtime_error("Vulkan GPU compute failed: " + failure.u8string());
         }
         fence_used = true;
      }
      results.push_back({{"algorithm", algorithm}, {"localSizeX", local}, {"count", count}, {"groups", groups}, {"cacheRoundtrip", cache_pass != 0},
         {"inputSeed", initial_seed}, {"firstResult", words[data_start]}, {"lastResult", words[data_start + count - 1]},
         {"guardWordsVerified", expected.size() - count}, {"sameCommandBufferExecutions", 2}, {"passed", true}});
   }
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, command_pool, nullptr);
   vkDestroyDescriptorPool(device, descriptor_pool, nullptr); vkDestroyBuffer(device, buffer, nullptr);
   vkUnmapMemory(device, memory); vkFreeMemory(device, memory, nullptr);
   return results;
}
static json test_compute_pipelines(VkDevice device, VkQueue queue, PFN_vkGetDeviceProcAddr get, json &hardware_results)
{
#define PIPELINE(name) auto name = reinterpret_cast<PFN_##name>(get(device, #name)); require(name != nullptr, "Missing " #name)
   PIPELINE(vkCreateShaderModule); PIPELINE(vkDestroyShaderModule); PIPELINE(vkCreateComputePipelines); PIPELINE(vkDestroyPipeline);
   PIPELINE(vkCreateDescriptorSetLayout); PIPELINE(vkDestroyDescriptorSetLayout); PIPELINE(vkCreatePipelineLayout); PIPELINE(vkDestroyPipelineLayout);
   PIPELINE(vkCreatePipelineCache); PIPELINE(vkDestroyPipelineCache); PIPELINE(vkGetPipelineCacheData);
#undef PIPELINE
   using Inspect = int (*)(VkPipeline, uint32_t *);
   auto inspect = reinterpret_cast<Inspect>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_query_compute_pipeline"));
   require(inspect != nullptr, "Missing pipeline ISA diagnostics");
   wchar_t executable[32768]; DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
   require(length && length < 32768, "Cannot locate test shaders");
   const fs::path directory = fs::path(executable).parent_path() / "shaders";
   VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
   VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount = 1; li.pBindings = &binding;
   VkDescriptorSetLayout layout{}; check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout), "Create compute descriptor layout");
   VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pi.setLayoutCount = 1; pi.pSetLayouts = &layout;
   VkPipelineLayout pipeline_layout{}; check(vkCreatePipelineLayout(device, &pi, nullptr, &pipeline_layout), "Create compute pipeline layout");
   VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; VkPipelineCache cache{};
   check(vkCreatePipelineCache(device, &ci, nullptr, &cache), "Create pipeline cache");
   json results = json::array();
   for (unsigned pass = 0; pass < 2; ++pass) {
      for (const char *algorithm : {"xor32", "lcg32", "mix32"}) {
         for (unsigned local : {1, 7, 32, 64}) {
            std::string name = std::string(algorithm) + (local == 1 ? "" : "-local" + std::to_string(local));
            std::ifstream stream(directory / (name + ".spv"), std::ios::binary | std::ios::ate);
            require(stream.good(), "Prepared SPIR-V file is missing");
            const auto bytes = stream.tellg(); require(bytes > 0 && bytes % 4 == 0 && bytes < 4194304, "Invalid SPIR-V file size");
            std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4); stream.seekg(0); stream.read(reinterpret_cast<char *>(code.data()), bytes);
            require(stream.good() && code[0] == 0x07230203, "Cannot read SPIR-V input");
            VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize = static_cast<size_t>(bytes); si.pCode = code.data();
            VkShaderModule shader{}; check(vkCreateShaderModule(device, &si, nullptr, &shader), "Create real Vulkan shader module");
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = shader; info.stage.pName = "main"; info.layout = pipeline_layout; info.basePipelineIndex = -1;
            VkPipeline pipeline{}; check(vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline), "Compile SPIR-V through real HasVK pipeline");
            vkDestroyShaderModule(device, shader, nullptr);
            uint32_t data[12]{};
            require(!inspect(pipeline, data) && data[0] && data[1] == local && data[2] == 1 && data[3] == 1 && data[4] && data[5] >= 2 && data[10],
               "Invalid compute pipeline metadata or ELK ISA");
            results.push_back({{"module", name}, {"cacheRoundtrip", pass != 0}, {"kernelBytes", data[0]}, {"localSizeX", data[1]},
               {"progMask", data[4]}, {"bindingTableEntries", data[5]}, {"nrParams", data[6]}, {"scratchBytes", data[7]},
               {"sharedBytes", data[8]}, {"kernelOffset", data[9]}, {"pipelineBatchBytes", data[10]}, {"isaValidated", true}});
            hardware_results.push_back(execute_compute(device, queue, get, pipeline, pipeline_layout, layout, algorithm, local, pass));
            vkDestroyPipeline(device, pipeline, nullptr);
         }
      }
      if (pass == 0) {
         size_t bytes = 0; check(vkGetPipelineCacheData(device, cache, &bytes, nullptr), "Query pipeline cache data size");
         require(bytes > sizeof(VkPipelineCacheHeaderVersionOne), "Pipeline cache contains no compiled shader objects");
         std::vector<uint8_t> serialized(bytes); check(vkGetPipelineCacheData(device, cache, &bytes, serialized.data()), "Serialize pipeline cache");
         vkDestroyPipelineCache(device, cache, nullptr);
         ci.initialDataSize = bytes; ci.pInitialData = serialized.data();
         check(vkCreatePipelineCache(device, &ci, nullptr, &cache), "Import compiled pipeline cache");
      }
   }
   vkDestroyPipelineCache(device, cache, nullptr); vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
   vkDestroyDescriptorSetLayout(device, layout, nullptr);
   return results;
}
int wmain(int argc, wchar_t **argv)
{
   if (argc != 2) return 2;
   json report = {{"schema", 1}, {"passed", false}, {"scope", "HasVK ICD experimental hardware compute"},
      {"vulkanComputeValidated", false}, {"hardwareVkcubeValidated", false}};
   int status = 1;
   try {
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader not found");
      auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      require(gipa != nullptr, "Loader lacks vkGetInstanceProcAddr");
      auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "IrisHasvkIcdProbe"; app.apiVersion = VK_API_VERSION_1_0;
      VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
      VkInstance instance{}; check(create_instance(&ci, nullptr, &instance), "vkCreateInstance");
#define INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); require(name != nullptr, "Missing " #name)
      INSTANCE(vkDestroyInstance); INSTANCE(vkEnumeratePhysicalDevices); INSTANCE(vkGetPhysicalDeviceProperties);
      INSTANCE(vkGetPhysicalDeviceFeatures); INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
      INSTANCE(vkGetPhysicalDeviceMemoryProperties); INSTANCE(vkEnumerateDeviceExtensionProperties);
      INSTANCE(vkCreateDevice); INSTANCE(vkGetDeviceProcAddr);
      uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate count");
      require(count == 1, "Expected exactly the locally selected HasVK adapter");
      VkPhysicalDevice physical{}; check(vkEnumeratePhysicalDevices(instance, &count, &physical), "Enumerate adapter");
      VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
      require(properties.vendorID == 0x8086 && properties.deviceID == 0x162b &&
         std::string(properties.deviceName).find("HasVK/WDDM") != std::string::npos, "A different Vulkan driver was selected");
      report["deviceName"] = properties.deviceName; report["apiVersion"] = properties.apiVersion;
      VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(physical, &features);
      require(!features.sparseBinding && !features.depthBounds && !features.textureCompressionASTC_LDR, "Unsupported features were exposed");
      uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
      require(family_count == 1, "Expected one queue family");
      VkQueueFamilyProperties family{}; vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, &family);
      require(family.queueCount == 1 && (family.queueFlags & VK_QUEUE_COMPUTE_BIT), "Unexpected queue capabilities");
      VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      require(memory.memoryTypeCount == 1 && memory.memoryHeapCount == 1, "Invalid memory configuration");
      uint32_t extension_count = 0; check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr), "Device extensions");
      {  /* Exactly the extensions this port has verified (anv_wddm_device.c verified_device_extensions). */
         static const char *const expected[] = {"VK_EXT_host_query_reset", "VK_KHR_16bit_storage", "VK_KHR_bind_memory2",
            "VK_KHR_dedicated_allocation", "VK_KHR_descriptor_update_template", "VK_KHR_device_group", "VK_KHR_driver_properties",
            "VK_KHR_external_memory", "VK_KHR_external_semaphore", "VK_KHR_get_memory_requirements2", "VK_KHR_maintenance1",
            "VK_KHR_maintenance2", "VK_KHR_maintenance3", "VK_KHR_multiview", "VK_KHR_relaxed_block_layout",
            "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_shader_draw_parameters", "VK_KHR_storage_buffer_storage_class",
            "VK_KHR_swapchain", "VK_KHR_variable_pointers"};
         std::vector<VkExtensionProperties> extensions(extension_count);
         check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data()), "Query device extensions");
         std::vector<std::string> names; for (const auto &extension : extensions) names.push_back(extension.extensionName);
         std::sort(names.begin(), names.end());
         require(names == std::vector<std::string>(std::begin(expected), std::end(expected)), "Device extensions differ from the verified list");
         report["deviceExtensions"] = names;
      }
      require(VK_API_VERSION_MAJOR(properties.apiVersion) == 1 && VK_API_VERSION_MINOR(properties.apiVersion) == 1, "ICD must report Vulkan 1.1");
      float priority = 1;
      VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
      VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
      VkPhysicalDeviceFeatures unsupported{}; unsupported.sparseBinding = VK_TRUE; di.pEnabledFeatures = &unsupported;
      VkDevice rejected{};
      require(vkCreateDevice(physical, &di, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT && !rejected, "Unsupported feature was not rejected");
      di.pEnabledFeatures = nullptr;
      const char *bad_extension = "VK_HASVK_not_implemented";
      di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &bad_extension;
      require(vkCreateDevice(physical, &di, nullptr, &rejected) == VK_ERROR_EXTENSION_NOT_PRESENT && !rejected, "Unsupported extension was not rejected");
      di.enabledExtensionCount = 0; di.ppEnabledExtensionNames = nullptr;
      uint64_t total_gpu_submits = 0;
      unsigned memory_cases = 0;
      unsigned pool_reads = 0;
      unsigned descriptor_checks = 0;
      json pipeline_results = json::array();
      json compute_results = json::array();
      json storage_image_results = json::array();
      for (unsigned iteration = 0; iteration < 3; ++iteration) {
         VkDevice device{}; check(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
#define DEVICE(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); require(name != nullptr, "Missing " #name)
         DEVICE(vkDestroyDevice); DEVICE(vkGetDeviceQueue); DEVICE(vkQueueSubmit); DEVICE(vkQueueWaitIdle); DEVICE(vkDeviceWaitIdle);
         DEVICE(vkCreateFence); DEVICE(vkDestroyFence); DEVICE(vkGetFenceStatus); DEVICE(vkResetFences); DEVICE(vkWaitForFences);
         DEVICE(vkCreateSemaphore); DEVICE(vkDestroySemaphore); DEVICE(vkCreateCommandPool);
         VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue); require(queue != VK_NULL_HANDLE, "Missing queue");
         using PoolTest = int (*)(VkDevice, uint32_t *);
         auto pool_test = reinterpret_cast<PoolTest>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_test_state_pools"));
         uint32_t reads{}; require(pool_test && !pool_test(device, &reads) && reads == 90, "Mesa state pool GPU readback failed");
         pool_reads += reads;
         descriptor_checks += test_descriptors(device, vkGetDeviceProcAddr);
         pipeline_results.push_back(test_compute_pipelines(device, queue, vkGetDeviceProcAddr, compute_results));
         storage_image_results.push_back(test_storage_images(device, queue, vkGetDeviceProcAddr));
         memory_cases += test_memory(device, queue, vkGetDeviceProcAddr);
         VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
         check(vkCreateFence(device, &fi, nullptr, &fence), "Create fence");
         require(vkGetFenceStatus(device, fence) == VK_NOT_READY, "Fresh fence incorrectly signaled");
         require(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT, "Unsignaled fence did not time out");
         fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
         VkFence initially_signaled{}; check(vkCreateFence(device, &fi, nullptr, &initially_signaled), "Create initially signaled fence");
         VkFence pair[] = {fence, initially_signaled};
         check(vkWaitForFences(device, 2, pair, VK_FALSE, 0), "Wait-any must observe the initially signaled fence");
         require(vkWaitForFences(device, 2, pair, VK_TRUE, 0) == VK_TIMEOUT, "Wait-all ignored an unsignaled fence");
         check(vkResetFences(device, 1, &initially_signaled), "Reset initially signaled fence");
         require(vkWaitForFences(device, 2, pair, VK_FALSE, 0) == VK_TIMEOUT, "Wait-any did not time out after reset");
         vkDestroyFence(device, initially_signaled, nullptr);
         VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
         for (unsigned i = 0; i < 32; ++i) {
            check(vkQueueSubmit(queue, i & 1 ? 1 : 0, i & 1 ? &submission : nullptr, fence), "Queue submit");
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), "GPU fence completion");
            check(vkGetFenceStatus(device, fence), "Signaled fence status");
            check(vkResetFences(device, 1, &fence), "Reset fence");
            require(vkGetFenceStatus(device, fence) == VK_NOT_READY, "Fence reset failed");
         }
         VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore semaphore{};
         check(vkCreateSemaphore(device, &si, nullptr, &semaphore), "Create semaphore");
         VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         for (unsigned i = 0; i < 3; ++i) {
            VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO}; signal.signalSemaphoreCount = 1; signal.pSignalSemaphores = &semaphore;
            check(vkQueueSubmit(queue, 1, &signal, VK_NULL_HANDLE), "Signal semaphore through GPU queue");
            VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO}; wait.waitSemaphoreCount = 1; wait.pWaitSemaphores = &semaphore; wait.pWaitDstStageMask = &stage;
            check(vkQueueSubmit(queue, 1, &wait, fence), "Wait semaphore through GPU queue");
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 15000000000ull), "Semaphore-dependent fence");
            check(vkResetFences(device, 1, &fence), "Reset dependent fence");
         }
         check(vkQueueWaitIdle(queue), "Queue wait idle"); check(vkDeviceWaitIdle(device), "Device wait idle");
         using Stats = int (*)(VkDevice, uint64_t *, uint64_t *);
         HMODULE icd = GetModuleHandleW(L"vulkan_hasvk_wddm.dll"); require(icd != nullptr, "ICD DLL not loaded");
         auto stats = reinterpret_cast<Stats>(GetProcAddress(icd, "hasvk_wddm_icd_query_stats")); require(stats != nullptr, "Missing driver diagnostics");
         uint64_t completed = 0, stamp = 0; require(!stats(device, &completed, &stamp), "Driver diagnostics failed");
         require(completed >= 38 && stamp == completed, "Vulkan submits were not confirmed by the GPU counter");
         total_gpu_submits += completed;
         vkDestroySemaphore(device, semaphore, nullptr); vkDestroyFence(device, fence, nullptr); vkDestroyDevice(device, nullptr);
      }
      vkDestroyInstance(instance, nullptr);
      report["forbiddenModules"] = forbidden_modules(); require(report["forbiddenModules"].empty(), "Intel UMD/D3D12 was loaded");
      report["deviceCreationCycles"] = 3; report["gpuConfirmedSubmissions"] = total_gpu_submits;
      report["wddmVulkanMemoryCases"] = memory_cases; report["mappedMemoryBindingAndLifetimeValidated"] = true;
      report["applicationBufferGpuShaderAccessValidated"] = true;
      report["mesaStatePoolGpuReadbacks"] = pool_reads;
      report["mesaStatePoolAndStateTableGrowthValidated"] = true;
      report["bufferDescriptorChecks"] = descriptor_checks;
      report["descriptorAndPipelineLayoutLifecycleValidated"] = true;
      report["descriptorGpuShaderAccessValidated"] = true;
      report["computeHardwareCases"] = compute_results;
      report["vulkanComputeValidated"] = true;
      report["computePipelineCases"] = pipeline_results;
      report["computePipelineCreationAndIsaValidated"] = true;
      report["pipelineCacheRoundtripValidated"] = true;
      report["storageImageHardwareCases"] = storage_image_results;
      report["storageImagesWithIndependentCpuDetileValidated"] = true;
      report["fenceStatusResetAndTimeoutValidated"] = true; report["binarySemaphoreReuseValidated"] = true;
      report["fenceWaitAnyAndAllValidated"] = true; report["bothEmptySubmissionFormsValidated"] = true;
      report["unsupportedFeaturesAndExtensionsRejected"] = true; report["shaderCommandBuffersDisabled"] = false;
      report["passed"] = true;
      FreeLibrary(loader); status = 0;
   } catch (const std::exception &error) { report["error"] = error.what(); std::cerr << error.what() << '\n'; }
   fs::create_directories(fs::path(argv[1]).parent_path());
   std::ofstream output(fs::path(argv[1]), std::ios::binary); output << report.dump(2) << '\n';
   if (!output) return 1;
   std::cout << report.dump(2) << '\n';
   return status;
}
