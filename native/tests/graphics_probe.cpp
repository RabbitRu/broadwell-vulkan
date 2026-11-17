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
#include <vector>
using json = nlohmann::json;
namespace fs = std::filesystem;
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static void check(VkResult result, const char *message) {
   if (result != VK_SUCCESS) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}
#include "icd_graphics_test.hpp"
#include "icd_query_test.hpp"
#include "icd_draw_test.hpp"
#include "icd_copy_test.hpp"
#include "icd_format_test.hpp"
#include "icd_compute_test.hpp"
#include "icd_event_test.hpp"
#include "icd_timeline_test.hpp"
#include "icd_submit_test.hpp"
int wmain(int argc, wchar_t **argv)
{
   if (argc != 2) return 2;
   json report{{"passed", false}, {"scope", "Vulkan offscreen graphics experiment"}, {"hardwareVkcubeValidated", false}};
   int status = 1;
   try {
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader missing");
      auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      require(gipa != nullptr, "Loader entry point missing");
      auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
      VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
      VkInstance instance{}; check(create(&ci, nullptr, &instance), "Create instance");
#define INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); require(name != nullptr, "Missing " #name)
      INSTANCE(vkEnumeratePhysicalDevices); INSTANCE(vkGetPhysicalDeviceProperties); INSTANCE(vkCreateDevice);
      INSTANCE(vkGetDeviceProcAddr); INSTANCE(vkDestroyInstance); INSTANCE(vkGetPhysicalDeviceFeatures); INSTANCE(vkGetPhysicalDeviceFormatProperties); INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
#undef INSTANCE
      uint32_t count = 1; VkPhysicalDevice physical{};
      check(vkEnumeratePhysicalDevices(instance, &count, &physical), "Enumerate physical device");
      require(count == 1, "Expected one locally selected physical device");
      VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical, &properties);
      require(properties.vendorID == 0x8086 && properties.deviceID == 0x162b &&
         std::string(properties.deviceName).find("HasVK/WDDM") != std::string::npos, "Wrong ICD selected");
      float priority = 1;
      VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
      VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
      VkPhysicalDeviceFeatures supported{}; vkGetPhysicalDeviceFeatures(physical, &supported);
      {  /* Vulkan 1.1 feature structures as an application queries them. */
         auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
         require(features2 != nullptr, "Missing vkGetPhysicalDeviceFeatures2");
         VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
         VkPhysicalDevice16BitStorageFeatures storage16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}; storage16.pNext = &multiview;
         VkPhysicalDeviceVariablePointersFeatures pointers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES}; pointers.pNext = &storage16;
         VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES}; draw.pNext = &pointers;
         VkPhysicalDeviceFeatures2 all{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; all.pNext = &draw;
         features2(physical, &all);
         report["vulkan11Features"] = {{"multiview", multiview.multiview != 0}, {"storageBuffer16BitAccess", storage16.storageBuffer16BitAccess != 0},
            {"variablePointersStorageBuffer", pointers.variablePointersStorageBuffer != 0}, {"shaderDrawParameters", draw.shaderDrawParameters != 0}};
         /* All false when the ICD manifest declares 1.0: the loader then emulates
          * vkGetPhysicalDeviceFeatures2 and leaves pNext untouched. */
         require(multiview.multiview && storage16.storageBuffer16BitAccess && pointers.variablePointersStorageBuffer && draw.shaderDrawParameters,
            "Vulkan 1.1 features not visible through the loader");
      }
      require(supported.occlusionQueryPrecise == VK_TRUE && supported.pipelineStatisticsQuery == VK_TRUE, "Query features not advertised");
      uint32_t families = 1; VkQueueFamilyProperties family{}; vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, &family);
      require(families == 1 && family.timestampValidBits >= 32 && family.timestampValidBits < 64 && properties.limits.timestampComputeAndGraphics == VK_TRUE &&
         properties.limits.timestampPeriod > 0, "Timestamps not advertised");
      VkPhysicalDeviceFeatures enabled{}; enabled.occlusionQueryPrecise = enabled.pipelineStatisticsQuery = VK_TRUE; di.pEnabledFeatures = &enabled;
      static const char *const device_extensions[] = {"VK_KHR_timeline_semaphore"};
      di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = device_extensions;
      VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
      timeline_features.timelineSemaphore = VK_TRUE; di.pNext = &timeline_features;
      VkDevice device{}; check(vkCreateDevice(physical, &di, nullptr, &device), "Create hardware device");
      auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(vkGetDeviceProcAddr(device, "vkGetDeviceQueue"));
      auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
      require(get_queue && destroy, "Missing device entry points");
      VkQueue queue{}; get_queue(device, 0, 0, &queue);
      report["cases"] = test_graphics(device, queue, vkGetDeviceProcAddr);
      {
         static const char *const core[] = {
#include "vk10_commands.inc"
         };
         using HasEntry = int (*)(VkDevice, const char *);
         auto has_entry = reinterpret_cast<HasEntry>(GetProcAddress(GetModuleHandleW(L"vulkan_hasvk_wddm.dll"), "hasvk_wddm_icd_has_entrypoint"));
         require(has_entry != nullptr, "Missing entry-point diagnostics");
         json missing = json::array();
         for (const char *name : core) {
            const int present = has_entry(device, name); require(present >= 0, "Entry-point diagnostics failed");
            if (!present) missing.push_back(name);
         }
         report["coreCommands"] = sizeof(core) / sizeof(core[0]); report["missingCoreCommands"] = missing;
         /* Later core versions: which commands already resolve (the ICD still reports 1.0). */
         static const char *const v11[] = {
#include "vk11_commands.inc"
         };
         static const char *const v12[] = {
#include "vk12_commands.inc"
         };
         static const char *const v13[] = {
#include "vk13_commands.inc"
         };
         json later = json::object();
         for (const auto &version : {std::make_pair("1.1", std::vector<const char *>(std::begin(v11), std::end(v11))),
                                     std::make_pair("1.2", std::vector<const char *>(std::begin(v12), std::end(v12))),
                                     std::make_pair("1.3", std::vector<const char *>(std::begin(v13), std::end(v13)))}) {
            json absent = json::array();
            for (const char *name : version.second) if (has_entry(device, name) <= 0) absent.push_back(name);
            later[version.first] = {{"commands", version.second.size()}, {"missing", absent}};
         }
         report["laterCoreCommands"] = later;
      }
      report["texturedDepthCases"] = test_graphics(device, queue, vkGetDeviceProcAddr, true);
      report["submitCost"] = test_submit_cost(device, queue, vkGetDeviceProcAddr);
      report["timelineCases"] = test_timelines(device, queue, vkGetDeviceProcAddr);
      report["eventCases"] = test_events(device, queue, vkGetDeviceProcAddr);
      report["computeCases"] = test_compute(device, queue, vkGetDeviceProcAddr);
      report["formatCoverage"] = test_formats(physical, vkGetPhysicalDeviceFormatProperties, device, queue, vkGetDeviceProcAddr);
      report["largestAllocation"] = test_largest_allocation(device, queue, vkGetDeviceProcAddr);
      report["largeMemory"] = test_large_memory(device, queue, vkGetDeviceProcAddr);
      report["transfer"] = test_copies(device, queue, vkGetDeviceProcAddr);
      report["drawCases"] = test_draws(device, queue, vkGetDeviceProcAddr);
      report["queryCases"] = test_queries(device, queue, vkGetDeviceProcAddr, properties.limits.timestampPeriod, family.timestampValidBits);
      destroy(device, nullptr); vkDestroyInstance(instance, nullptr);
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      require(snapshot != INVALID_HANDLE_VALUE, "Module inventory failed");
      MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
      require(Module32FirstW(snapshot, &entry) != 0, "Module inventory empty");
      do {
         std::wstring name(entry.szModule);
         std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
         require(name.rfind(L"igd", 0) != 0 && name.rfind(L"d3d12", 0) != 0, "Intel UMD or D3D12 loaded");
      } while (Module32NextW(snapshot, &entry));
      const DWORD error = GetLastError(); CloseHandle(snapshot); require(error == ERROR_NO_MORE_FILES, "Module inventory incomplete");
      report["forbiddenModulesAbsent"] = true; report["passed"] = true; status = 0;
   } catch (const std::exception &error) { report["error"] = error.what(); std::cerr << error.what() << '\n'; }
   fs::create_directories(fs::path(argv[1]).parent_path());
   std::ofstream output(fs::path(argv[1]), std::ios::binary); output << report.dump(2) << '\n';
   std::cout << report.dump(2) << '\n'; return output ? status : 1;
}
