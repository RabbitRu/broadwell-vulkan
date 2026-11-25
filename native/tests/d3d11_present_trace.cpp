/* Research tool: how Intel's D3D11 driver and the DXGI runtime present frames
 * (for the Vulkan present path, hypothesis E7). The same syscall hooks as
 * gl_present_trace.cpp; a D3D11 window clears and presents a few frames with
 * the chosen swap effect, then closes itself. Each Present() is also timed.
 *
 * Usage: D3D11PresentTrace.exe <log file> [frames] [discard|sequential|flip_discard|flip_sequential] [sync interval] [hook|nohook] [fullscreen] */
#define PRESENT_TRACE_NO_MAIN
#include "gl_present_trace.cpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <intrin.h>

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: D3D11PresentTrace <log> [frames] [swap effect]\n"); return 2; }
   const int frames = argc > 2 ? atoi(argv[2]) : 4;
   const char *effect_name = argc > 3 ? argv[3] : "discard";
   DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_DISCARD;
   if (!strcmp(effect_name, "sequential")) effect = DXGI_SWAP_EFFECT_SEQUENTIAL;
   else if (!strcmp(effect_name, "flip_discard")) effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   else if (!strcmp(effect_name, "flip_sequential")) effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
   const bool flip = effect == DXGI_SWAP_EFFECT_FLIP_DISCARD || effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
   const UINT sync_interval = argc > 4 ? (UINT)atoi(argv[4]) : 1;
   if (fopen_s(&out, argv[1], "w") || !out) return 3;
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
   Sleep(1500);
   HINSTANCE instance = GetModuleHandleW(nullptr);
   WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"D3D11PresentTrace";
   RegisterClassW(&wc);
   HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"D3D11 present trace", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240,
                                 nullptr, nullptr, instance, nullptr);
   if (!window) return 5;
   pump();
   /* Device creation is logged too (allocations of the swap chain's buffers).
    * "nohook" as the fifth argument: no syscall logging, timings only. */
   const bool hooked = !(argc > 5 && !strcmp(argv[5], "nohook"));
   if (hooked && !install_hooks()) return 4;
   phase = "create device";
   DXGI_SWAP_CHAIN_DESC desc{};
   desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.SampleDesc.Count = 1;
   desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   desc.BufferCount = flip ? 2 : 1;
   desc.OutputWindow = window;
   desc.Windowed = TRUE;
   desc.SwapEffect = effect;
   ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr; IDXGISwapChain *swap = nullptr;
   HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
                                              &swap, &device, nullptr, &context);
   if (FAILED(hr)) { fprintf(out, "D3D11CreateDeviceAndSwapChain failed %#lx\n", (unsigned long)hr); return 6; }
   /* "fullscreen" as the sixth argument: DXGI's full-screen state in the
    * current mode (exclusive ownership of the output, presents are flips);
    * left again before the swap chain is released. */
   const bool fullscreen = argc > 6 && !strcmp(argv[6], "fullscreen");
   if (fullscreen) {
      phase = "enter fullscreen";
      hr = swap->SetFullscreenState(TRUE, nullptr);
      { std::lock_guard<std::mutex> lock(gate); fprintf(out, "SetFullscreenState(TRUE) = %#lx\n", (unsigned long)hr); fflush(out); }
      phase = "resize buffers";
      hr = swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
      { std::lock_guard<std::mutex> lock(gate); fprintf(out, "ResizeBuffers = %#lx\n", (unsigned long)hr); fflush(out); }
      pump();
   }
   ID3D11Texture2D *back = nullptr; swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
   ID3D11RenderTargetView *rtv = nullptr; device->CreateRenderTargetView(back, nullptr, &rtv);
   { D3D11_TEXTURE2D_DESC bd; back->GetDesc(&bd);
     std::lock_guard<std::mutex> lock(gate); fprintf(out, "back buffer %ux%u format %d\n", bd.Width, bd.Height, (int)bd.Format); }
   { std::lock_guard<std::mutex> lock(gate);
     fprintf(out, "swap effect %s, %u buffers, sync interval %u, main thread tid=%lu\n", effect_name, desc.BufferCount, sync_interval,
             GetCurrentThreadId()); fflush(out); }
   light_enabled = true;
   static const char *frame_names[] = {"frame 0", "frame 1", "frame 2", "frame 3", "frame 4", "frame 5", "frame 6", "frame 7"};
   const int limit = hooked ? 8 : 200;
   LARGE_INTEGER f; QueryPerformanceFrequency(&f);
   double tsc_per_ms;
   {  /* thread cycle counts tick at the TSC rate */
      LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0); const uint64_t t0 = __rdtsc(); Sleep(100);
      QueryPerformanceCounter(&q1); const uint64_t t1 = __rdtsc();
      tsc_per_ms = (t1 - t0) / ((q1.QuadPart - q0.QuadPart) * 1e3 / f.QuadPart);
   }
   double cpu_sum = 0, wall_sum = 0; int counted = 0;
   for (int i = 0; i < frames && i < limit; ++i) {
      phase = frame_names[i < 8 ? i : 7];
      if (flip && i) {   /* flip model: the render target is the current buffer */
         rtv->Release(); back->Release();
         swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back); device->CreateRenderTargetView(back, nullptr, &rtv);
      }
      const float color[4] = {i & 1 ? 1.f : 0.f, i & 2 ? 1.f : 0.f, 0.5f, 1.f};
      context->OMSetRenderTargets(1, &rtv, nullptr);
      context->ClearRenderTargetView(rtv, color);
      /* Wall time and this thread's CPU time (thread cycles at the TSC rate). */
      ULONG64 c0, c1; QueryThreadCycleTime(GetCurrentThread(), &c0);
      LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
      hr = swap->Present(sync_interval, 0);
      QueryPerformanceCounter(&b);
      QueryThreadCycleTime(GetCurrentThread(), &c1);
      const double wall = (b.QuadPart - a.QuadPart) * 1e3 / f.QuadPart, cpu = (c1 - c0) / tsc_per_ms;
      if (i >= 2) { cpu_sum += cpu; wall_sum += wall; ++counted; }
      { std::lock_guard<std::mutex> lock(gate);
        if (hooked || i < 4) fprintf(out, "PRESENT %d returned %#lx in %.3f ms; thread CPU %.3f ms\n", i, (unsigned long)hr, wall, cpu);
        fflush(out); }
      pump();
      if (hooked) { Sleep(100); pump(); }
   }
   if (counted) fprintf(out, "MEAN of %d presents after the first two: %.3f ms wall, %.3f ms thread CPU\n", counted, wall_sum / counted, cpu_sum / counted);
   if (fullscreen) { phase = "leave fullscreen"; swap->SetFullscreenState(FALSE, nullptr); pump(); }
   light_enabled = false;
   phase = "teardown";
   rtv->Release(); back->Release(); swap->Release(); context->Release(); device->Release();
   DestroyWindow(window); pump();
   { std::lock_guard<std::mutex> lock(gate); fprintf(out, "done\n"); fflush(out); }
   return 0;
}
