/* A Vulkan window that covers the primary screen without a frame, as a game
 * in "borderless" mode: the case this ICD presents by exclusive flips
 * (mesa/anv_wddm_present.c). Every frame is cleared to a colour that changes
 * slowly, with a white rectangle at (100,50)-(400,250); the last <hold>
 * seconds keep one colour (printed), so a screen capture can be compared.
 * tools\dev\test-flip-probe.ps1 reads DWM's GPU load meanwhile: none while
 * the frames are flipped.
 *
 * Usage: flip_wsi_probe.exe <seconds> <immediate|fifo|mailbox> [hold seconds] [options]
 *   srgb          B8G8R8A8_SRGB swapchain
 *   size=WxH      a window of that size at the top left (not the whole screen: composed)
 *   cover=<s>     after s seconds a small topmost window is shown for 2 s (composed meanwhile) */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("%s = %d\n", #x, r_); return 1; } } while (0)

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_ERASEBKGND) return 1;
   return DefWindowProcW(window, message, w, l);
}
static void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: flip_wsi_probe <seconds> <immediate|fifo|mailbox> [hold seconds] [srgb] [size=WxH] [cover=<s>]\n"); return 2; }
   const double seconds = atof(argv[1]), hold = argc > 3 ? atof(argv[3]) : 0;
   const VkPresentModeKHR mode = !strcmp(argv[2], "fifo") ? VK_PRESENT_MODE_FIFO_KHR : !strcmp(argv[2], "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                                                                    : VK_PRESENT_MODE_IMMEDIATE_KHR;
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
   int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
   bool srgb = false; double cover_at = -1;
   for (int a = 4; a < argc; ++a) {
      if (!strcmp(argv[a], "srgb")) srgb = true;
      if (!strncmp(argv[a], "size=", 5)) sscanf_s(argv[a] + 5, "%dx%d", &width, &height);
      if (!strncmp(argv[a], "cover=", 6)) cover_at = atof(argv[a] + 6);
   }
   HINSTANCE instance_handle = GetModuleHandleW(nullptr);
   WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance_handle; wc.lpszClassName = L"FlipWsiProbe";
   wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc);
   HWND window = CreateWindowExW(0, wc.lpszClassName, L"flip WSI probe", WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr, instance_handle, nullptr);
   if (!window) return 3;
   SetForegroundWindow(window); pump();

   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   const char *instance_extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = instance_extensions;
   VkInstance instance; CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance));
#define I(name) auto name = (PFN_##name)gipa(instance, #name)
   I(vkEnumeratePhysicalDevices); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkCreateWin32SurfaceKHR);
   I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(instance, &n, &pd);
   const char *device_extensions[] = {"VK_KHR_swapchain"};
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.dynamicRendering = VK_TRUE;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &features13; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = device_extensions;
   VkDevice d; CHECK(vkCreateDevice(pd, &dci, nullptr, &d));
#define F(name) auto name = (PFN_##name)vkGetDeviceProcAddr(d, #name)
   F(vkGetDeviceQueue); F(vkCreateSwapchainKHR); F(vkGetSwapchainImagesKHR); F(vkAcquireNextImageKHR); F(vkQueuePresentKHR);
   F(vkCreateImageView); F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkEndCommandBuffer);
   F(vkResetCommandBuffer); F(vkCmdPipelineBarrier); F(vkCmdBeginRendering); F(vkCmdEndRendering); F(vkQueueSubmit); F(vkCreateSemaphore);
   F(vkCreateFence); F(vkWaitForFences); F(vkResetFences); F(vkDeviceWaitIdle); F(vkDestroySwapchainKHR); F(vkDestroyDevice);
   VkQueue queue; vkGetDeviceQueue(d, 0, 0, &queue);
   VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; sci.hinstance = instance_handle; sci.hwnd = window;
   VkSurfaceKHR surface; CHECK(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface));
   VkSurfaceCapabilitiesKHR caps; CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
   const VkFormat format = srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
   VkSwapchainCreateInfoKHR swi{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
   swi.surface = surface; swi.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount; swi.imageFormat = format;
   swi.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; swi.imageExtent = caps.currentExtent; swi.imageArrayLayers = 1;
   swi.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; swi.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
   swi.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; swi.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; swi.presentMode = mode; swi.clipped = VK_TRUE;
   VkSwapchainKHR swapchain; CHECK(vkCreateSwapchainKHR(d, &swi, nullptr, &swapchain));
   uint32_t image_count = 0; vkGetSwapchainImagesKHR(d, swapchain, &image_count, nullptr);
   std::vector<VkImage> images(image_count); vkGetSwapchainImagesKHR(d, swapchain, &image_count, images.data());
   std::vector<VkImageView> views(image_count);
   for (uint32_t i = 0; i < image_count; ++i) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      CHECK(vkCreateImageView(d, &vi, nullptr, &views[i]));
   }
   VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   VkCommandPool pool; CHECK(vkCreateCommandPool(d, &pi, nullptr, &pool));
   VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool = pool; ca.commandBufferCount = 1;
   VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d, &ca, &cmd));
   VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VkSemaphore acquired, rendered;
   CHECK(vkCreateSemaphore(d, &semi, nullptr, &acquired)); CHECK(vkCreateSemaphore(d, &semi, nullptr, &rendered));
   VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(d, &fi, nullptr, &fence));
   printf("window %dx%d, swapchain %ux%u, %u images, %s, present mode %s; window %s\n", width, height, caps.currentExtent.width, caps.currentExtent.height,
          image_count, srgb ? "B8G8R8A8_SRGB" : "B8G8R8A8_UNORM", argv[2], GetForegroundWindow() == window ? "in the foreground" : "NOT in the foreground");

   LARGE_INTEGER f, start, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
   unsigned frames = 0; HWND cover = nullptr; double elapsed = 0; float last[3] = {0, 0, 0};
   for (;;) {
      QueryPerformanceCounter(&now); elapsed = (now.QuadPart - start.QuadPart) / (double)f.QuadPart;
      if (elapsed >= seconds + hold) break;
      if (cover_at >= 0 && !cover && elapsed >= cover_at && elapsed < cover_at + 2)
         cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"cover", WS_POPUP | WS_VISIBLE, 600, 600, 300, 200, nullptr, nullptr, instance_handle, nullptr);
      if (cover && elapsed >= cover_at + 2) { DestroyWindow(cover); cover = nullptr; cover_at = -1; }
      uint32_t index = 0;
      const VkResult got = vkAcquireNextImageKHR(d, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
      if (got < 0) { printf("vkAcquireNextImageKHR = %d\n", got); break; }
      /* A slow colour cycle; one fixed colour during the hold. */
      const float t = elapsed < seconds ? (float)((frames % 240) / 240.0) : 0.25f;
      const float colour[3] = {t, 0.5f, 1.0f - t};
      memcpy(last, colour, sizeof(last));
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = images[index]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      for (int pass = 0; pass < 2; ++pass) {
         VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; att.imageView = views[index];
         att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
         if (pass) att.clearValue.color = {{1, 1, 1, 1}}; else att.clearValue.color = {{colour[0], colour[1], colour[2], 1}};
         VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &att;
         ri.renderArea = pass ? VkRect2D{{100, 50}, {300, 200}} : VkRect2D{{0, 0}, caps.currentExtent};
         vkCmdBeginRendering(cmd, &ri); vkCmdEndRendering(cmd);
      }
      b.oldLayout = b.newLayout; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.srcAccessMask = b.dstAccessMask; b.dstAccessMask = 0;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      CHECK(vkEndCommandBuffer(cmd));
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.waitSemaphoreCount = 1; s.pWaitSemaphores = &acquired; s.pWaitDstStageMask = &stage;
      s.commandBufferCount = 1; s.pCommandBuffers = &cmd; s.signalSemaphoreCount = 1; s.pSignalSemaphores = &rendered;
      CHECK(vkQueueSubmit(queue, 1, &s, fence));
      VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered;
      present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
      const VkResult shown = vkQueuePresentKHR(queue, &present);
      if (shown < 0) { printf("vkQueuePresentKHR = %d\n", shown); break; }
      CHECK(vkWaitForFences(d, 1, &fence, VK_TRUE, 10000000000ull)); CHECK(vkResetFences(d, 1, &fence));
      ++frames; pump();
   }
   printf("%u frames in %.1f s = %.1f FPS; last colour %02x%02x%02x (R G B, %s)\n", frames, elapsed, frames / elapsed,
          (unsigned)(last[0] * 255 + 0.5f), (unsigned)(last[1] * 255 + 0.5f), (unsigned)(last[2] * 255 + 0.5f), srgb ? "linear values" : "as stored");
   vkDeviceWaitIdle(d);
   F(vkDestroyImageView); F(vkDestroyCommandPool); F(vkDestroySemaphore); F(vkDestroyFence);
   I(vkDestroySurfaceKHR); I(vkDestroyInstance);
   for (VkImageView view : views) vkDestroyImageView(d, view, nullptr);
   vkDestroyCommandPool(d, pool, nullptr); vkDestroySemaphore(d, acquired, nullptr); vkDestroySemaphore(d, rendered, nullptr);
   vkDestroyFence(d, fence, nullptr);
   vkDestroySwapchainKHR(d, swapchain, nullptr);
   vkDestroyDevice(d, nullptr);
   vkDestroySurfaceKHR(instance, surface, nullptr); vkDestroyInstance(instance, nullptr);
   DestroyWindow(window); pump();
   return 0;
}
