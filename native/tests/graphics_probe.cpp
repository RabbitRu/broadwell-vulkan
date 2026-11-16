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
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_0;
      VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
      VkInstance instance{}; check(create(&ci, nullptr, &instance), "Create instance");
#define INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); require(name != nullptr, "Missing " #name)
      INSTANCE(vkEnumeratePhysicalDevices); INSTANCE(vkGetPhysicalDeviceProperties); INSTANCE(vkCreateDevice);
      INSTANCE(vkGetDeviceProcAddr); INSTANCE(vkDestroyInstance);
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
      VkDevice device{}; check(vkCreateDevice(physical, &di, nullptr, &device), "Create hardware device");
      auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(vkGetDeviceProcAddr(device, "vkGetDeviceQueue"));
      auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
      require(get_queue && destroy, "Missing device entry points");
      VkQueue queue{}; get_queue(device, 0, 0, &queue);
      report["cases"] = test_graphics(device, queue, vkGetDeviceProcAddr);
      report["texturedDepthCases"] = test_graphics(device, queue, vkGetDeviceProcAddr, true);
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
