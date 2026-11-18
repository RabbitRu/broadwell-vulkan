/* Resource leaks over many object lifetimes in one process.
 *
 * Repeats, on one instance: create a device, allocate host-visible and
 * device-local memory, a buffer, an image, a command pool; record a fill and a
 * clear; submit with a fence and wait; destroy everything and the device. Then
 * repeats swapchain lifetimes on one window (create, present a few frames,
 * destroy). After a warm-up, process private memory, OS handle count and the
 * kernel's process memory counters must stay flat.
 *
 * Usage: IrisLeakProbe.exe <report.json> */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <psapi.h>
#include <vulkan/vulkan.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using json = nlohmann::json;
static void require(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
static void check(VkResult result, const std::string &name) { if (result != VK_SUCCESS) throw std::runtime_error(name + ": " + std::to_string(result)); }

struct Usage { uint64_t privateBytes, handles, gdiObjects; };
static Usage usage()
{
   PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
   GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
   DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
   return {pmc.PrivateUsage, handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)};
}
static json to_json(const Usage &u) { return {{"privateBytes", u.privateBytes}, {"handles", u.handles}, {"gdiObjects", u.gdiObjects}}; }

struct Api {
   PFN_vkGetInstanceProcAddr gipa{}; VkInstance instance{}; VkPhysicalDevice physical{};
   PFN_vkCreateDevice create_device{}; PFN_vkGetDeviceProcAddr gdpa{};
};
#define API(name) auto name = reinterpret_cast<PFN_##name>(a.gdpa(device, #name)); require(name != nullptr, "Missing " #name)

static VkDevice make_device(Api &a, bool swapchain)
{
   float priority = 1;
   VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
   const char *extensions[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
   if (swapchain) { di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = extensions; }
   VkDevice device{}; check(a.create_device(a.physical, &di, nullptr, &device), "Create device");
   return device;
}

/* One device lifetime with memory, buffer, image and a submitted command buffer. */
static void device_cycle(Api &a)
{
   VkDevice device = make_device(a, false);
   API(vkGetDeviceQueue); API(vkCreateBuffer); API(vkDestroyBuffer); API(vkCreateImage); API(vkDestroyImage);
   API(vkGetImageMemoryRequirements); API(vkAllocateMemory); API(vkFreeMemory); API(vkBindBufferMemory); API(vkBindImageMemory);
   API(vkMapMemory); API(vkCreateCommandPool); API(vkDestroyCommandPool); API(vkAllocateCommandBuffers);
   API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkCmdFillBuffer); API(vkCmdPipelineBarrier); API(vkCmdClearColorImage);
   API(vkCreateFence); API(vkDestroyFence); API(vkQueueSubmit); API(vkWaitForFences); API(vkDestroyDevice);
   VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
   VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = 1 << 20; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer buffer{}; check(vkCreateBuffer(device, &bi, nullptr, &buffer), "Buffer");
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = 1 << 20; ai.memoryTypeIndex = 0;
   VkDeviceMemory host{}; check(vkAllocateMemory(device, &ai, nullptr, &host), "Host-visible memory");
   check(vkBindBufferMemory(device, buffer, host, 0), "Bind buffer");
   void *map{}; check(vkMapMemory(device, host, 0, VK_WHOLE_SIZE, 0, &map), "Map");
   VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
   ii.extent = {256, 256, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
   ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   VkImage image{}; check(vkCreateImage(device, &ii, nullptr, &image), "Image");
   VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, image, &mr);
   VkMemoryAllocateInfo ia{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ia.allocationSize = mr.size; ia.memoryTypeIndex = 1;   /* device-local */
   VkDeviceMemory local{}; check(vkAllocateMemory(device, &ia, nullptr, &local), "Device-local memory");
   check(vkBindImageMemory(device, image, local, 0), "Bind image");
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool{};
   check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
   vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, 0x1eaf1eafu);
   VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
   const VkClearColorValue colour{{0.25f, 0.5f, 0.75f, 1.0f}};
   vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &b.subresourceRange);
   VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host_read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, nullptr, 0, nullptr);
   check(vkEndCommandBuffer(cmd), "End");
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; check(vkCreateFence(device, &fi, nullptr, &fence), "Fence");
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount = 1; s.pCommandBuffers = &cmd;
   check(vkQueueSubmit(queue, 1, &s, fence), "Submit"); check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull), "Wait");
   const uint32_t *words = static_cast<const uint32_t *>(map);
   require(words[0] == 0x1eaf1eafu && words[(1 << 18) - 1] == 0x1eaf1eafu, "Fill result differs");
   vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
   vkDestroyImage(device, image, nullptr); vkFreeMemory(device, local, nullptr);
   vkDestroyBuffer(device, buffer, nullptr); vkFreeMemory(device, host, nullptr);
   vkDestroyDevice(device, nullptr);
}

/* Swapchain lifetimes on one window and device: create, present, destroy. */
static void swapchain_cycles(Api &a, VkSurfaceKHR surface, VkDevice device, unsigned cycles)
{
   API(vkGetDeviceQueue); API(vkCreateSwapchainKHR); API(vkDestroySwapchainKHR); API(vkGetSwapchainImagesKHR);
   API(vkAcquireNextImageKHR); API(vkQueuePresentKHR); API(vkCreateCommandPool); API(vkDestroyCommandPool);
   API(vkAllocateCommandBuffers); API(vkBeginCommandBuffer); API(vkEndCommandBuffer); API(vkResetCommandBuffer);
   API(vkCmdPipelineBarrier); API(vkCmdClearColorImage); API(vkQueueSubmit); API(vkDeviceWaitIdle);
   API(vkCreateSemaphore); API(vkDestroySemaphore); API(vkCreateFence); API(vkDestroyFence); API(vkWaitForFences); API(vkResetFences);
   auto caps_query = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(a.gipa(a.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
   VkQueue queue{}; vkGetDeviceQueue(device, 0, 0, &queue);
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool{}; check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Pool");
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd{}; check(vkAllocateCommandBuffers(device, &ca, &cmd), "Command buffer");
   VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acquired{}, rendered{};
   check(vkCreateSemaphore(device, &si, nullptr, &acquired), "Semaphore"); check(vkCreateSemaphore(device, &si, nullptr, &rendered), "Semaphore");
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
   VkFence fence{}; check(vkCreateFence(device, &fi, nullptr, &fence), "Fence");
   for (unsigned c = 0; c < cycles; ++c) {
      VkSurfaceCapabilitiesKHR caps{}; check(caps_query(a.physical, surface, &caps), "Capabilities");
      VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.surface = surface; ci.minImageCount = 3;
      ci.imageFormat = c % 2 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM; ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      ci.imageExtent = caps.currentExtent; ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; ci.clipped = VK_TRUE;
      VkSwapchainKHR chain{}; check(vkCreateSwapchainKHR(device, &ci, nullptr, &chain), "Create swapchain");
      uint32_t count = 0; check(vkGetSwapchainImagesKHR(device, chain, &count, nullptr), "Images");
      std::vector<VkImage> images(count); check(vkGetSwapchainImagesKHR(device, chain, &count, images.data()), "Images");
      for (int f = 0; f < 4; ++f) {
         uint32_t index = 0;
         check(vkAcquireNextImageKHR(device, chain, 5000000000ull, acquired, VK_NULL_HANDLE, &index), "Acquire");
         check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull), "Wait frame"); check(vkResetFences(device, 1, &fence), "Reset fence");
         check(vkResetCommandBuffer(cmd, 0), "Reset");
         VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd, &begin), "Begin");
         VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
         b.image = images[index]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
         const VkClearColorValue colour{{0.1f * f, 0.5f, 0.75f, 1.0f}};
         vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &b.subresourceRange);
         b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
         check(vkEndCommandBuffer(cmd), "End");
         const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
         VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.waitSemaphoreCount = 1; s.pWaitSemaphores = &acquired; s.pWaitDstStageMask = &stage;
         s.commandBufferCount = 1; s.pCommandBuffers = &cmd; s.signalSemaphoreCount = 1; s.pSignalSemaphores = &rendered;
         check(vkQueueSubmit(queue, 1, &s, fence), "Submit frame");
         VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; p.waitSemaphoreCount = 1; p.pWaitSemaphores = &rendered;
         p.swapchainCount = 1; p.pSwapchains = &chain; p.pImageIndices = &index;
         check(vkQueuePresentKHR(queue, &p), "Present");
      }
      check(vkDeviceWaitIdle(device), "Idle");
      vkDestroySwapchainKHR(device, chain, nullptr);
      MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
   }
   vkDestroyFence(device, fence, nullptr); vkDestroySemaphore(device, acquired, nullptr); vkDestroySemaphore(device, rendered, nullptr);
   vkDestroyCommandPool(device, pool, nullptr);
}
#undef API

int wmain(int argc, wchar_t **argv)
{
   if (argc != 2) return 2;
   json report{{"passed", false}};
   int status = 1;
   HWND hwnd{};
   try {
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader missing");
      Api a;
      a.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      const char *instance_extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
      VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
      ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instance_extensions;
      check(reinterpret_cast<PFN_vkCreateInstance>(a.gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &a.instance), "Create instance");
      uint32_t count = 1;
      check(reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(a.gipa(a.instance, "vkEnumeratePhysicalDevices"))(a.instance, &count, &a.physical), "Enumerate");
      a.create_device = reinterpret_cast<PFN_vkCreateDevice>(a.gipa(a.instance, "vkCreateDevice"));
      a.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(a.gipa(a.instance, "vkGetDeviceProcAddr"));

      /* Devices: 10 warm-up cycles, then 150 measured. */
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 10; ++i) device_cycle(a);
      const Usage device_before = usage();
      const unsigned device_cycles = 150;
      for (unsigned i = 0; i < device_cycles; ++i) device_cycle(a);
      const Usage device_after = usage();
      report["devices"] = {{"cycles", device_cycles}, {"before", to_json(device_before)}, {"after", to_json(device_after)},
         {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()}};

      /* Swapchains on one window and device: 5 warm-up, then 60 measured. */
      WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"IrisLeakProbe";
      RegisterClassW(&wc);
      hwnd = CreateWindowExW(0, wc.lpszClassName, L"HasVK/WDDM leak probe (closes itself)", WS_OVERLAPPEDWINDOW, 120, 120, 320, 240,
                             nullptr, nullptr, wc.hInstance, nullptr);
      require(hwnd != nullptr, "CreateWindow failed");
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; sci.hinstance = wc.hInstance; sci.hwnd = hwnd;
      VkSurfaceKHR surface{};
      check(reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(a.gipa(a.instance, "vkCreateWin32SurfaceKHR"))(a.instance, &sci, nullptr, &surface), "Surface");
      VkDevice device = make_device(a, true);
      const auto swap_start = std::chrono::steady_clock::now();
      swapchain_cycles(a, surface, device, 5);
      const Usage swap_before = usage();
      const unsigned swap_cycles = 60;
      swapchain_cycles(a, surface, device, swap_cycles);
      const Usage swap_after = usage();
      reinterpret_cast<PFN_vkDestroyDevice>(a.gdpa(device, "vkDestroyDevice"))(device, nullptr);
      reinterpret_cast<PFN_vkDestroySurfaceKHR>(a.gipa(a.instance, "vkDestroySurfaceKHR"))(a.instance, surface, nullptr);
      report["swapchains"] = {{"cycles", swap_cycles}, {"framesPerCycle", 4}, {"before", to_json(swap_before)}, {"after", to_json(swap_after)},
         {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - swap_start).count()}};
      reinterpret_cast<PFN_vkDestroyInstance>(a.gipa(a.instance, "vkDestroyInstance"))(a.instance, nullptr);

      /* Flat after warm-up: under 64 KiB of private memory and no handles or
       * GDI objects per 10 cycles on average. */
      auto flat = [](const Usage &b, const Usage &e, unsigned cycles, const char *what) {
         const int64_t bytes = static_cast<int64_t>(e.privateBytes) - static_cast<int64_t>(b.privateBytes);
         const int64_t handles = static_cast<int64_t>(e.handles) - static_cast<int64_t>(b.handles);
         const int64_t gdi = static_cast<int64_t>(e.gdiObjects) - static_cast<int64_t>(b.gdiObjects);
         require(bytes < static_cast<int64_t>(cycles) * 6554, std::string(what) + ": private memory grows by " + std::to_string(bytes) + " bytes");
         require(handles < static_cast<int64_t>(cycles) / 10 + 4, std::string(what) + ": handle count grows by " + std::to_string(handles));
         require(gdi < static_cast<int64_t>(cycles) / 10 + 4, std::string(what) + ": GDI objects grow by " + std::to_string(gdi));
      };
      flat(device_before, device_after, device_cycles, "device lifetimes");
      flat(swap_before, swap_after, swap_cycles, "swapchain lifetimes");
      report["passed"] = true;
      status = 0;
   } catch (const std::exception &e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
   if (hwnd) DestroyWindow(hwnd);
   std::ofstream(argv[1]) << report.dump(2) << '\n';
   std::cout << report.dump(2) << '\n';
   return status;
}
