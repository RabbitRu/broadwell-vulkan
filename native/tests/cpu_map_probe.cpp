/* CPU speed on mapped GPU memory: sequential writes (memset), sequential reads
 * (sum), 16-byte stores scattered over the buffer and 4-byte read-modify-write,
 * on every host-visible Vulkan memory type of the ICD, on Intel's D3D11
 * dynamic buffer (Map WRITE_DISCARD) and on ordinary process memory. A
 * write-combined or uncached mapping shows as reads and scattered stores
 * many times slower than process memory.
 * Usage: cpu_map_probe.exe (VK_DRIVER_FILES selects the ICD) */
#define NOMINMAX
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d11.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

static const size_t kBytes = 32u << 20;

static double now_ms()
{
   LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
   return t.QuadPart * 1000.0 / f.QuadPart;
}

/* Best of five for each pattern, in GB/s (scattered: million stores/s). */
static void measure(const char *what, void *map, size_t bytes)
{
   volatile uint64_t sink = 0;
   double best[4] = {1e30, 1e30, 1e30, 1e30};
   const size_t scattered = 1u << 20;
   for (int rep = 0; rep < 5; ++rep) {
      double t0 = now_ms();
      memset(map, rep, bytes);
      double t1 = now_ms();
      uint64_t sum = 0; const uint64_t *q = static_cast<const uint64_t *>(map);
      for (size_t i = 0; i < bytes / 8; i += 4) sum += q[i] + q[i + 1] + q[i + 2] + q[i + 3];
      double t2 = now_ms();
      uint32_t x = 12345u + rep; char *c = static_cast<char *>(map);
      for (size_t i = 0; i < scattered; ++i) {
         x = x * 1664525u + 1013904223u;
         const size_t at = (size_t)(x % (uint32_t)(bytes / 16)) * 16;
         const uint64_t v[2] = {i, x};
         memcpy(c + at, v, 16);
      }
      double t3 = now_ms();
      uint32_t *w = static_cast<uint32_t *>(map);
      for (size_t i = 0; i < scattered; ++i) { x = x * 1664525u + 1013904223u; w[x % (uint32_t)(bytes / 4)] += 1; }
      double t4 = now_ms();
      sink += sum;
      best[0] = std::min(best[0], t1 - t0); best[1] = std::min(best[1], t2 - t1);
      best[2] = std::min(best[2], t3 - t2); best[3] = std::min(best[3], t4 - t3);
   }
   printf("%-44s write %6.2f GB/s, read %6.2f GB/s, scattered 16 B stores %6.1f M/s, 4 B read-modify-write %6.1f M/s\n", what,
          bytes / (best[0] / 1000) / 1e9, bytes / (best[1] / 1000) / 1e9, scattered / (best[2] / 1000) / 1e6,
          scattered / (best[3] / 1000) / 1e6);
   (void)sink;
}

static int vulkan()
{
   HMODULE loader = LoadLibraryA("vulkan-1.dll");
   if (!loader) { printf("no vulkan-1.dll\n"); return 1; }
   auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
   VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
   VkInstance instance;
   if (((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&ici, nullptr, &instance)) { printf("vkCreateInstance failed\n"); return 1; }
   uint32_t n = 1; VkPhysicalDevice pd;
   ((PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &pd);
   if (!n) { printf("no Vulkan device\n"); return 1; }
   VkPhysicalDeviceMemoryProperties mp;
   ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
   float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueCount = 1; q.pQueuePriorities = &priority;
   VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
   VkDevice d;
   if (((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(pd, &dci, nullptr, &d)) { printf("vkCreateDevice failed\n"); return 1; }
   auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   auto vkAllocateMemory = (PFN_vkAllocateMemory)gdpa(d, "vkAllocateMemory");
   auto vkMapMemory = (PFN_vkMapMemory)gdpa(d, "vkMapMemory");
   auto vkFreeMemory = (PFN_vkFreeMemory)gdpa(d, "vkFreeMemory");
   for (uint32_t type = 0; type < mp.memoryTypeCount; ++type) {
      const VkMemoryPropertyFlags f = mp.memoryTypes[type].propertyFlags;
      if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = kBytes; ai.memoryTypeIndex = type;
      VkDeviceMemory m; void *map = nullptr;
      if (vkAllocateMemory(d, &ai, nullptr, &m) || vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &map)) { printf("type %u: allocation failed\n", type); continue; }
      char what[96];
      snprintf(what, sizeof what, "Vulkan type %u (%s%s%s)", type, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "local " : "",
               f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached " : "", f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "coherent" : "");
      measure(what, map, kBytes);
      vkFreeMemory(d, m, nullptr);
   }
   return 0;
}

static int d3d11()
{
   ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
   if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
      printf("D3D11CreateDevice failed\n"); return 1;
   }
   D3D11_BUFFER_DESC desc{}; desc.ByteWidth = (UINT)kBytes; desc.Usage = D3D11_USAGE_DYNAMIC;
   desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   ID3D11Buffer *buffer = nullptr;
   if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) { printf("CreateBuffer failed\n"); return 1; }
   D3D11_MAPPED_SUBRESOURCE mapped{};
   if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { printf("Map failed\n"); return 1; }
   measure("D3D11 dynamic vertex buffer (WRITE_DISCARD)", mapped.pData, kBytes);
   context->Unmap(buffer, 0);
   desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
   ID3D11Buffer *staging = nullptr;
   if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging)) && SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ_WRITE, 0, &mapped))) {
      measure("D3D11 staging buffer (READ_WRITE)", mapped.pData, kBytes);
      context->Unmap(staging, 0); staging->Release();
   }
   buffer->Release(); context->Release(); device->Release();
   return 0;
}

int main()
{
   SetThreadAffinityMask(GetCurrentThread(), 1);
   void *plain = VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
   measure("process memory (VirtualAlloc)", plain, kBytes);
   void *wc = VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE);
   if (wc) measure("process memory, PAGE_WRITECOMBINE", wc, kBytes);
   const int a = vulkan();
   const int b = d3d11();
   return a | b;
}
