/* Window-system integration on the real window path (redirected present).
 *
 * A window of its own (closed by the probe itself) shows swapchain frames
 * cleared to a known colour. Checked:
 *  - every advertised surface format (B8G8R8A8/R8G8B8A8, UNORM/SRGB): every
 *    pixel of the client area, captured with PrintWindow, is the expected
 *    colour (sRGB formats show the encoded value);
 *  - FIFO is paced by composition, IMMEDIATE and MAILBOX are not;
 *  - after a resize, present reports SUBOPTIMAL and acquire OUT_OF_DATE, also
 *    while minimized; a swapchain recreated with oldSwapchain shows the new size.
 *
 * Usage: IrisWsiProbe.exe <report.json> */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using json = nlohmann::json;
static void require(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
static void check(VkResult result, const std::string &name) { if (result != VK_SUCCESS) throw std::runtime_error(name + ": " + std::to_string(result)); }

/* Console display state from GUID_CONSOLE_DISPLAY_STATE (0 off, 1 on,
 * 2 dimmed; -1 unknown). While it is off DWM does not compose: the kernel
 * refuses redirected presents (PRESENT_OCCLUDED), frames go through GDI and
 * composition pacing cannot be observed. */
static int g_display_state = -1;
static const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   if (msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE) {
      const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(lp);
      if (IsEqualGUID(setting->PowerSetting, kConsoleDisplayState) && setting->DataLength >= sizeof(DWORD))
         g_display_state = static_cast<int>(*reinterpret_cast<const DWORD *>(setting->Data));
      return TRUE;
   }
   return DefWindowProcW(hwnd, msg, wp, lp);
}
/* Display on and the session not locked (the input desktop is ours). */
static bool composition_observable()
{
   HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
   if (!desk) return false;
   CloseDesktop(desk);
   return g_display_state != 0;
}

static void pump()
{
   MSG msg;
   while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
static void settle(unsigned ms)
{
   const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
   while (std::chrono::steady_clock::now() < end) { pump(); Sleep(10); }
}
static void set_client_size(HWND hwnd, int width, int height)
{
   RECT r{0, 0, width, height};
   AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
   SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
   pump();
}
/* Client area as DWM shows it (PrintWindow with full content), 0x00RRGGBB. */
static std::vector<uint32_t> capture(HWND hwnd, int &width, int &height)
{
   RECT client; GetClientRect(hwnd, &client);
   width = client.right; height = client.bottom;
   HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
   HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
   HGDIOBJ old = SelectObject(dc, bitmap);
   const BOOL printed = PrintWindow(hwnd, dc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
   SelectObject(dc, old);
   BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = width; bi.bmiHeader.biHeight = -height;
   bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
   std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
   GetDIBits(dc, bitmap, 0, height, pixels.data(), &bi, DIB_RGB_COLORS);
   DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
   require(printed != 0, "PrintWindow failed");
   for (uint32_t &p : pixels) p &= 0xffffffu;
   return pixels;
}
static uint8_t unorm(float v) { return static_cast<uint8_t>(std::lround(v * 255.0f)); }
static uint8_t srgb(float v) { return unorm(v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f); }

struct Vk {
   PFN_vkGetInstanceProcAddr gipa{}; VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
   VkSurfaceKHR surface{}; VkCommandPool pool{}; VkCommandBuffer cmd{}; VkFence fence{}; VkSemaphore acquired{}, rendered{};
#define F(name) PFN_##name name{}
   F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); F(vkGetPhysicalDeviceSurfaceFormatsKHR); F(vkGetPhysicalDeviceSurfacePresentModesKHR);
   F(vkGetPhysicalDeviceSurfaceSupportKHR);
   F(vkCreateSwapchainKHR); F(vkDestroySwapchainKHR); F(vkGetSwapchainImagesKHR); F(vkAcquireNextImageKHR); F(vkQueuePresentKHR);
   F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkResetCommandBuffer); F(vkCmdPipelineBarrier); F(vkCmdClearColorImage);
   F(vkQueueSubmit); F(vkWaitForFences); F(vkResetFences); F(vkDeviceWaitIdle); F(vkQueueWaitIdle);
#undef F
};

/* Records and submits: clear image to colour, transition for present. */
static void render(Vk &vk, VkImage image, const VkClearColorValue &colour)
{
   check(vk.vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 5000000000ull), "Wait previous frame");
   check(vk.vkResetFences(vk.device, 1, &vk.fence), "Reset fence");
   check(vk.vkResetCommandBuffer(vk.cmd, 0), "Reset command buffer");
   VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   check(vk.vkBeginCommandBuffer(vk.cmd, &begin), "Begin");
   const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image; b.subresourceRange = range;
   b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   vk.vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
   vk.vkCmdClearColorImage(vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
   vk.vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
   check(vk.vkEndCommandBuffer(vk.cmd), "End");
   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
   VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.waitSemaphoreCount = 1; s.pWaitSemaphores = &vk.acquired; s.pWaitDstStageMask = &wait_stage;
   s.commandBufferCount = 1; s.pCommandBuffers = &vk.cmd; s.signalSemaphoreCount = 1; s.pSignalSemaphores = &vk.rendered;
   check(vk.vkQueueSubmit(vk.queue, 1, &s, vk.fence), "Submit frame");
}

struct Chain { VkSwapchainKHR handle{}; std::vector<VkImage> images; VkExtent2D extent{}; };
static Chain create_chain(Vk &vk, VkFormat format, VkPresentModeKHR mode, VkSwapchainKHR old)
{
   VkSurfaceCapabilitiesKHR caps{}; check(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps), "Surface capabilities");
   Chain c; c.extent = caps.currentExtent;
   VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; ci.surface = vk.surface;
   ci.minImageCount = std::max(3u, caps.minImageCount); ci.imageFormat = format; ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   ci.imageExtent = c.extent; ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = mode; ci.clipped = VK_TRUE; ci.oldSwapchain = old;
   check(vk.vkCreateSwapchainKHR(vk.device, &ci, nullptr, &c.handle), "Create swapchain");
   uint32_t count = 0; check(vk.vkGetSwapchainImagesKHR(vk.device, c.handle, &count, nullptr), "Swapchain image count");
   c.images.resize(count); check(vk.vkGetSwapchainImagesKHR(vk.device, c.handle, &count, c.images.data()), "Swapchain images");
   return c;
}
/* One frame; returns the present result (the acquire must succeed). */
static VkResult frame(Vk &vk, Chain &c, const VkClearColorValue &colour)
{
   uint32_t index = 0;
   check(vk.vkAcquireNextImageKHR(vk.device, c.handle, 5000000000ull, vk.acquired, VK_NULL_HANDLE, &index), "Acquire");
   render(vk, c.images[index], colour);
   VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; p.waitSemaphoreCount = 1; p.pWaitSemaphores = &vk.rendered;
   p.swapchainCount = 1; p.pSwapchains = &c.handle; p.pImageIndices = &index;
   return vk.vkQueuePresentKHR(vk.queue, &p);
}
/* Every pixel of the client area equals expected (+-1 per channel). A child
 * window is captured through its top-level parent (DWM composes only that):
 * the child's rectangle inside the parent's client area. */
static json verify(HWND hwnd, const Chain &c, uint32_t expected)
{
   settle(150);
   int width = 0, height = 0;
   std::vector<uint32_t> pixels;
   const HWND root = GetAncestor(hwnd, GA_ROOT);
   if (root != hwnd) {
      int root_width = 0, root_height = 0;
      const std::vector<uint32_t> all = capture(root, root_width, root_height);
      RECT r; GetClientRect(hwnd, &r); MapWindowPoints(hwnd, root, reinterpret_cast<POINT *>(&r), 2);
      width = r.right - r.left; height = r.bottom - r.top;
      require(r.left >= 0 && r.top >= 0 && r.right <= root_width && r.bottom <= root_height, "Child window outside its parent");
      for (int y = r.top; y < r.bottom; ++y) for (int x = r.left; x < r.right; ++x) pixels.push_back(all[static_cast<size_t>(y) * root_width + x]);
   } else {
      pixels = capture(hwnd, width, height);
   }
   require(width == static_cast<int>(c.extent.width) && height == static_cast<int>(c.extent.height), "Client area differs from the swapchain");
   uint64_t mismatches = 0; uint32_t first_bad = 0;
   for (uint32_t p : pixels) {
      bool same = true;
      for (int shift = 0; shift < 24; shift += 8)
         same = same && std::abs(static_cast<int>((p >> shift) & 0xff) - static_cast<int>((expected >> shift) & 0xff)) <= 1;
      if (!same && !mismatches++) first_bad = p;
   }
   char text[64]; snprintf(text, sizeof(text), "%06x", expected);
   json r{{"width", width}, {"height", height}, {"expected", text}, {"mismatches", mismatches}};
   if (mismatches) { snprintf(text, sizeof(text), "%06x", first_bad); r["firstMismatch"] = text; }
   return r;
}
static const char *format_name(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM"; case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
   case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM"; case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
   default: return "other";
   }
}

int wmain(int argc, wchar_t **argv)
{
   if (argc != 2) return 2;
   json report{{"passed", false}};
   int status = 1;
   HWND hwnd{};
   SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);   /* a sleeping display occludes presents */
   try {
      WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"IrisWsiProbe";
      wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      RegisterClassW(&wc);
      RECT r{0, 0, 256, 192}; AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
      hwnd = CreateWindowExW(0, wc.lpszClassName, L"HasVK/WDDM WSI probe (closes itself)", WS_OVERLAPPEDWINDOW, 80, 80,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
      require(hwnd != nullptr, "CreateWindow failed");
      HPOWERNOTIFY power = RegisterPowerSettingNotification(hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE); settle(300);   /* the current display state arrives right away */
      report["displayState"] = g_display_state;
      report["compositionObservable"] = composition_observable();

      Vk vk;
      HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      require(loader != nullptr, "System Vulkan loader missing");
      vk.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
      const char *instance_extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
      VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
      ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instance_extensions;
      check(reinterpret_cast<PFN_vkCreateInstance>(vk.gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &vk.instance), "Create instance");
#define I(name) vk.name = reinterpret_cast<PFN_##name>(vk.gipa(vk.instance, #name)); require(vk.name != nullptr, "Missing " #name)
      I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfaceFormatsKHR); I(vkGetPhysicalDeviceSurfacePresentModesKHR);
      I(vkGetPhysicalDeviceSurfaceSupportKHR);
#undef I
      uint32_t count = 1;
      check(reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vk.gipa(vk.instance, "vkEnumeratePhysicalDevices"))(vk.instance, &count, &vk.physical), "Enumerate");
      VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; sci.hinstance = wc.hInstance; sci.hwnd = hwnd;
      check(reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vk.gipa(vk.instance, "vkCreateWin32SurfaceKHR"))(vk.instance, &sci, nullptr, &vk.surface), "Create surface");
      VkBool32 supported = VK_FALSE; check(vk.vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, 0, vk.surface, &supported), "Surface support");
      require(supported == VK_TRUE, "Queue cannot present");

      {  /* VK_KHR_win32_surface: min, max and current extent equal the window size. */
         VkSurfaceCapabilitiesKHR caps{}; check(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps), "Surface capabilities");
         report["extents"] = {{"current", {caps.currentExtent.width, caps.currentExtent.height}},
            {"min", {caps.minImageExtent.width, caps.minImageExtent.height}}, {"max", {caps.maxImageExtent.width, caps.maxImageExtent.height}}};
         require(caps.currentExtent.width == 256 && caps.currentExtent.height == 192, "currentExtent is not the client area");
         require(!memcmp(&caps.minImageExtent, &caps.currentExtent, sizeof(VkExtent2D)) &&
                 !memcmp(&caps.maxImageExtent, &caps.currentExtent, sizeof(VkExtent2D)), "min/maxImageExtent differ from the window size");
      }
      count = 0; check(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, nullptr), "Formats");
      std::vector<VkSurfaceFormatKHR> formats(count); check(vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, formats.data()), "Formats");
      count = 0; check(vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, nullptr), "Present modes");
      std::vector<VkPresentModeKHR> modes(count); check(vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, modes.data()), "Present modes");
      json listed = json::array(); for (const auto &f : formats) listed.push_back(format_name(f.format));
      report["surfaceFormats"] = listed; report["presentModes"] = modes;
      /* R8G8B8A8 formats are not listed unless HASVK_WDDM_PRESENT_DIRECT=0; their
       * swapchains are still created and checked below (presented by copy). */
      const VkFormat wanted[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
      for (VkFormat f : wanted)
         require(std::any_of(formats.begin(), formats.end(), [f](const VkSurfaceFormatKHR &s) { return s.format == f; }), std::string("Missing surface format ") + format_name(f));
      for (VkPresentModeKHR m : {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
         require(std::find(modes.begin(), modes.end(), m) != modes.end(), "Missing present mode " + std::to_string(m));

      float priority = 1;
      VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &priority;
      const char *device_extensions[] = {"VK_KHR_swapchain"};
      VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
      dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = device_extensions;
      check(reinterpret_cast<PFN_vkCreateDevice>(vk.gipa(vk.instance, "vkCreateDevice"))(vk.physical, &dci, nullptr, &vk.device), "Create device");
      auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk.gipa(vk.instance, "vkGetDeviceProcAddr"));
#define D(name) vk.name = reinterpret_cast<PFN_##name>(gdpa(vk.device, #name)); require(vk.name != nullptr, "Missing " #name)
      D(vkCreateSwapchainKHR); D(vkDestroySwapchainKHR); D(vkGetSwapchainImagesKHR); D(vkAcquireNextImageKHR); D(vkQueuePresentKHR);
      D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkResetCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdClearColorImage);
      D(vkQueueSubmit); D(vkWaitForFences); D(vkResetFences); D(vkDeviceWaitIdle); D(vkQueueWaitIdle);
#undef D
      reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(vk.device, "vkGetDeviceQueue"))(vk.device, 0, 0, &vk.queue);
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      check(reinterpret_cast<PFN_vkCreateCommandPool>(gdpa(vk.device, "vkCreateCommandPool"))(vk.device, &pci, nullptr, &vk.pool), "Command pool");
      VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = vk.pool; ca.commandBufferCount = 1;
      check(reinterpret_cast<PFN_vkAllocateCommandBuffers>(gdpa(vk.device, "vkAllocateCommandBuffers"))(vk.device, &ca, &vk.cmd), "Command buffer");
      VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      check(reinterpret_cast<PFN_vkCreateFence>(gdpa(vk.device, "vkCreateFence"))(vk.device, &fci, nullptr, &vk.fence), "Fence");
      auto create_semaphore = reinterpret_cast<PFN_vkCreateSemaphore>(gdpa(vk.device, "vkCreateSemaphore"));
      VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      check(create_semaphore(vk.device, &sem, nullptr, &vk.acquired), "Semaphore"); check(create_semaphore(vk.device, &sem, nullptr, &vk.rendered), "Semaphore");

      /* 1. Formats: every pixel, encoded as the format's colour space demands. */
      const VkClearColorValue colour{{0.25f, 0.5f, 0.75f, 1.0f}};
      json format_cases = json::array();
      Chain chain{};
      for (VkFormat f : wanted) {
         Chain next = create_chain(vk, f, VK_PRESENT_MODE_FIFO_KHR, chain.handle);
         if (chain.handle) { vk.vkDeviceWaitIdle(vk.device); vk.vkDestroySwapchainKHR(vk.device, chain.handle, nullptr); }
         chain = next;
         for (int i = 0; i < 3; ++i) check(frame(vk, chain, colour), std::string("Present ") + format_name(f));
         const bool is_srgb = f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
         auto enc = is_srgb ? srgb : unorm;
         const uint32_t expected = static_cast<uint32_t>(enc(0.25f)) << 16 | static_cast<uint32_t>(enc(0.5f)) << 8 | enc(0.75f);
         json seen = verify(hwnd, chain, expected); seen["format"] = format_name(f);
         format_cases.push_back(seen);
         require(seen["mismatches"] == 0, std::string("Window shows wrong pixels for ") + format_name(f));
      }
      report["formats"] = format_cases;

      /* 2. Present modes: FIFO waits for composition, the others do not. */
      json timing;
      for (VkPresentModeKHR m : {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
         Chain next = create_chain(vk, VK_FORMAT_B8G8R8A8_UNORM, m, chain.handle);
         vk.vkDeviceWaitIdle(vk.device); vk.vkDestroySwapchainKHR(vk.device, chain.handle, nullptr); chain = next;
         check(frame(vk, chain, colour), "Warm-up present");
         const auto start = std::chrono::steady_clock::now();
         for (int i = 0; i < 60; ++i) { check(frame(vk, chain, colour), "Timed present"); pump(); }
         vk.vkQueueWaitIdle(vk.queue);
         timing[m == VK_PRESENT_MODE_FIFO_KHR ? "fifoMsPerFrame" : m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediateMsPerFrame" : "mailboxMsPerFrame"] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 60;
      }
      const bool observable = composition_observable();
      timing["observable"] = observable;
      report["presentTiming"] = timing;
      /* FIFO waits for the next vblank by DWM's timing, also on the GDI
       * fallback while the display is off. */
      const double fifo = timing["fifoMsPerFrame"], immediate = timing["immediateMsPerFrame"], mailbox = timing["mailboxMsPerFrame"];
      require(fifo >= 12.0 && fifo < 40.0, "FIFO is not paced to the refresh rate");
      require(immediate < fifo * 0.75 && mailbox < fifo * 0.75, "IMMEDIATE/MAILBOX are paced like FIFO");
      if (!observable) report["note"] = "display off or session locked: frames went through the GDI fallback";

      /* 3. Resize: present SUBOPTIMAL, acquire OUT_OF_DATE, recreate with oldSwapchain. */
      json resize;
      {
         uint32_t index = 0;
         check(vk.vkAcquireNextImageKHR(vk.device, chain.handle, 5000000000ull, vk.acquired, VK_NULL_HANDLE, &index), "Acquire before resize");
         set_client_size(hwnd, 320, 200); settle(100);
         render(vk, chain.images[index], colour);
         VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; p.waitSemaphoreCount = 1; p.pWaitSemaphores = &vk.rendered;
         p.swapchainCount = 1; p.pSwapchains = &chain.handle; p.pImageIndices = &index;
         const VkResult presented = vk.vkQueuePresentKHR(vk.queue, &p);
         resize["presentAfterResize"] = presented;
         require(presented == VK_SUBOPTIMAL_KHR, "Present after resize is not SUBOPTIMAL");
         vk.vkQueueWaitIdle(vk.queue);
         const VkResult acquired = vk.vkAcquireNextImageKHR(vk.device, chain.handle, 0, vk.acquired, VK_NULL_HANDLE, &index);
         resize["acquireAfterResize"] = acquired;
         require(acquired == VK_ERROR_OUT_OF_DATE_KHR, "Acquire after resize is not OUT_OF_DATE");
         ShowWindow(hwnd, SW_SHOWMINNOACTIVE); settle(300);
         VkSurfaceCapabilitiesKHR caps{}; check(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &caps), "Capabilities while minimized");
         resize["minimizedExtent"] = {caps.currentExtent.width, caps.currentExtent.height};
         /* Minimized is not a mismatch: the old swapchain still acquires and
          * presents (applications are not pushed into a zero-extent swapchain),
          * and creating one with the 0x0 extent reports OUT_OF_DATE. */
         const VkResult minimized = vk.vkAcquireNextImageKHR(vk.device, chain.handle, 1000000000ull, vk.acquired, VK_NULL_HANDLE, &index);
         resize["acquireWhileMinimized"] = minimized;
         require(minimized == VK_SUCCESS, "Acquire while minimized failed");
         render(vk, chain.images[index], colour);
         VkPresentInfoKHR mp{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; mp.waitSemaphoreCount = 1; mp.pWaitSemaphores = &vk.rendered;
         mp.swapchainCount = 1; mp.pSwapchains = &chain.handle; mp.pImageIndices = &index;
         const VkResult minimized_present = vk.vkQueuePresentKHR(vk.queue, &mp);
         resize["presentWhileMinimized"] = minimized_present;
         require(minimized_present == VK_SUCCESS, "Present while minimized failed");
         vk.vkQueueWaitIdle(vk.queue);
         VkSwapchainKHR zero{};
         VkSwapchainCreateInfoKHR zci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; zci.surface = vk.surface; zci.minImageCount = 3;
         zci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; zci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; zci.imageArrayLayers = 1;
         zci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; zci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
         zci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; zci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
         const VkResult zero_created = vk.vkCreateSwapchainKHR(vk.device, &zci, nullptr, &zero);
         resize["zeroExtentSwapchain"] = zero_created;
         require(zero_created == VK_ERROR_OUT_OF_DATE_KHR, "Zero-extent swapchain is not refused with OUT_OF_DATE");
         ShowWindow(hwnd, SW_SHOWNOACTIVATE); settle(300);
         /* Restored to the swapchain's old size? No: it was resized before, so
          * the old chain is out of date again. */
         const VkResult restored = vk.vkAcquireNextImageKHR(vk.device, chain.handle, 0, vk.acquired, VK_NULL_HANDLE, &index);
         resize["acquireAfterRestore"] = restored;
         require(restored == VK_ERROR_OUT_OF_DATE_KHR, "Acquire after restore is not OUT_OF_DATE");
         Chain next = create_chain(vk, VK_FORMAT_R8G8B8A8_SRGB, VK_PRESENT_MODE_FIFO_KHR, chain.handle);
         vk.vkDeviceWaitIdle(vk.device); vk.vkDestroySwapchainKHR(vk.device, chain.handle, nullptr); chain = next;
         require(chain.extent.width == 320 && chain.extent.height == 200, "Recreated swapchain does not have the new size");
         for (int i = 0; i < 3; ++i) check(frame(vk, chain, colour), "Present after recreation");
         const uint32_t expected = static_cast<uint32_t>(srgb(0.25f)) << 16 | static_cast<uint32_t>(srgb(0.5f)) << 8 | srgb(0.75f);
         resize["afterRecreate"] = verify(hwnd, chain, expected);
         require(resize["afterRecreate"]["mismatches"] == 0, "Window shows wrong pixels after recreation");
      }
      report["resize"] = resize;

      /* 4. Window geometry: a borderless popup and a child window of the main
       * window, each with its own surface and swapchain. */
      vk.vkDeviceWaitIdle(vk.device);
      vk.vkDestroySwapchainKHR(vk.device, chain.handle, nullptr); chain = {};
      json geometry = json::array();
      auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vk.gipa(vk.instance, "vkCreateWin32SurfaceKHR"));
      auto destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(vk.gipa(vk.instance, "vkDestroySurfaceKHR"));
      const VkSurfaceKHR main_surface = vk.surface;
      struct Shape { const char *name; DWORD style; HWND parent; int x, y, w, h; };
      for (const Shape &shape : {Shape{"borderless popup", WS_POPUP | WS_VISIBLE, nullptr, 420, 80, 200, 150},
                                 Shape{"child window", WS_CHILD | WS_VISIBLE, hwnd, 40, 30, 160, 120}}) {
         HWND other = CreateWindowExW(0, wc.lpszClassName, L"", shape.style, shape.x, shape.y, shape.w, shape.h,
                                      shape.parent, nullptr, wc.hInstance, nullptr);
         require(other != nullptr, std::string("Create ") + shape.name);
         if (!shape.parent) ShowWindow(other, SW_SHOWNOACTIVATE);
         settle(200);
         VkWin32SurfaceCreateInfoKHR oci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; oci.hinstance = wc.hInstance; oci.hwnd = other;
         check(create_surface(vk.instance, &oci, nullptr, &vk.surface), std::string("Surface for ") + shape.name);
         Chain c = create_chain(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE);
         require(c.extent.width == static_cast<uint32_t>(shape.w) && c.extent.height == static_cast<uint32_t>(shape.h),
                 std::string("Surface extent of ") + shape.name);
         /* The borderless popup is a window of exactly the swapchain's size: each
          * image is a window surface of its own, presented without a copy.
          * Every frame gets its own colour and must be the one shown, so a
          * present that left DWM on an older surface fails. */
         if (!shape.parent) {
            /* Both B8G8R8A8 encodings (R8G8B8A8 images are always copied:
             * DWM reads window surfaces as B8G8R8A8). */
            json sequence = json::array();
            for (VkFormat format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
               vk.vkDeviceWaitIdle(vk.device);
               vk.vkDestroySwapchainKHR(vk.device, c.handle, nullptr);
               c = create_chain(vk, format, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE);
               const bool encoded = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
               bool wrong = false;
               for (int i = 0; i < 6 && !wrong; ++i) {
                  const float red = (i % 3) * 0.25f + 0.125f, green = 0.875f - (i % 2) * 0.5f, blue = i * 0.125f;
                  check(frame(vk, c, VkClearColorValue{{red, green, blue, 1.0f}}), std::string("Present to ") + shape.name);
                  auto enc = [&](float v) { return static_cast<uint32_t>(encoded ? srgb(v) : unorm(v)); };
                  json step = verify(other, c, enc(red) << 16 | enc(green) << 8 | enc(blue));
                  step["format"] = format_name(format);
                  wrong = step["mismatches"] != 0;
                  sequence.push_back(step);
               }
            }
            report["popupSequence"] = sequence;
            for (const json &step : sequence) require(step["mismatches"] == 0, std::string("Stale or wrong frame in ") + shape.name);
            vk.vkDeviceWaitIdle(vk.device);
            vk.vkDestroySwapchainKHR(vk.device, c.handle, nullptr);
            c = create_chain(vk, VK_FORMAT_B8G8R8A8_UNORM, VK_PRESENT_MODE_FIFO_KHR, VK_NULL_HANDLE);
         }
         const VkClearColorValue other_colour{{0.75f, 0.25f, 0.5f, 1.0f}};
         for (int i = 0; i < 3; ++i) check(frame(vk, c, other_colour), std::string("Present to ") + shape.name);
         json seen = verify(other, c, static_cast<uint32_t>(unorm(0.75f)) << 16 | static_cast<uint32_t>(unorm(0.25f)) << 8 | unorm(0.5f));
         seen["window"] = shape.name;
         geometry.push_back(seen);
         vk.vkDeviceWaitIdle(vk.device);
         vk.vkDestroySwapchainKHR(vk.device, c.handle, nullptr);
         destroy_surface(vk.instance, vk.surface, nullptr);
         vk.surface = main_surface;
         DestroyWindow(other); pump();
         require(seen["mismatches"] == 0, std::string("Wrong pixels in ") + shape.name);
      }
      report["geometry"] = geometry;

      vk.vkDeviceWaitIdle(vk.device);
      reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(vk.device, "vkDestroySemaphore"))(vk.device, vk.acquired, nullptr);
      reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(vk.device, "vkDestroySemaphore"))(vk.device, vk.rendered, nullptr);
      reinterpret_cast<PFN_vkDestroyFence>(gdpa(vk.device, "vkDestroyFence"))(vk.device, vk.fence, nullptr);
      reinterpret_cast<PFN_vkDestroyCommandPool>(gdpa(vk.device, "vkDestroyCommandPool"))(vk.device, vk.pool, nullptr);
      reinterpret_cast<PFN_vkDestroyDevice>(gdpa(vk.device, "vkDestroyDevice"))(vk.device, nullptr);
      reinterpret_cast<PFN_vkDestroySurfaceKHR>(vk.gipa(vk.instance, "vkDestroySurfaceKHR"))(vk.instance, vk.surface, nullptr);
      reinterpret_cast<PFN_vkDestroyInstance>(vk.gipa(vk.instance, "vkDestroyInstance"))(vk.instance, nullptr);
      if (power) UnregisterPowerSettingNotification(power);
      report["passed"] = true;
      status = 0;
   } catch (const std::exception &e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
   if (hwnd) DestroyWindow(hwnd);
   SetThreadExecutionState(ES_CONTINUOUS);
   std::ofstream(argv[1]) << report.dump(2) << '\n';
   std::cout << report.dump(2) << '\n';
   return status;
}
