/* Does DWM compose a borderless window of the screen's size, or does Windows
 * show the swap chain's buffer itself (direct or independent flip)?
 * A D3D11 window covers the primary screen for <seconds> and presents a
 * cleared frame each time; meanwhile tools\dev\test-flip-probe.ps1 reads the
 * GPU load of dwm.exe. With composition DWM copies 2560x1600 per frame; with
 * a flip it does nothing.
 *
 * Usage: d3d11_flip_probe.exe <seconds> <discard|flip_discard|flip_sequential> [sync interval] [buffers] [width height]
 * (width/height: a window of that size at the top left corner instead of the whole screen) */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
   return DefWindowProcW(window, message, w, l);
}
static void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: d3d11_flip_probe <seconds> <discard|flip_discard|flip_sequential> [interval] [buffers] [width height]\n"); return 2; }
   const double seconds = atof(argv[1]);
   const char *effect_name = argv[2];
   const UINT interval = argc > 3 ? (UINT)atoi(argv[3]) : 1;
   DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_DISCARD;
   if (!strcmp(effect_name, "flip_discard")) effect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   else if (!strcmp(effect_name, "flip_sequential")) effect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
   const bool flip = effect != DXGI_SWAP_EFFECT_DISCARD;
   const UINT buffers = argc > 4 ? (UINT)atoi(argv[4]) : flip ? 2 : 1;
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   /* physical pixels */
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
   const int width = argc > 6 ? atoi(argv[5]) : GetSystemMetrics(SM_CXSCREEN), height = argc > 6 ? atoi(argv[6]) : GetSystemMetrics(SM_CYSCREEN);
   HINSTANCE instance = GetModuleHandleW(nullptr);
   WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"D3D11FlipProbe";
   RegisterClassW(&wc);
   /* D3D11_FLIP_TOPMOST=1: above the taskbar even when this process cannot take the foreground. */
   char topmost[8] = "";
   const DWORD ex_style = GetEnvironmentVariableA("D3D11_FLIP_TOPMOST", topmost, sizeof(topmost)) && topmost[0] == '1' ? WS_EX_TOPMOST : 0;
   HWND window = CreateWindowExW(ex_style, wc.lpszClassName, L"D3D11 flip probe", WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr, instance, nullptr);
   if (!window) return 3;
   SetForegroundWindow(window); pump();
   printf("window %s, %s\n", GetForegroundWindow() == window ? "in the foreground" : "NOT in the foreground", ex_style ? "topmost" : "not topmost");
   ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
   if (FAILED(hr)) { printf("D3D11CreateDevice %#lx\n", (unsigned long)hr); return 4; }
   IDXGIDevice *dxgi_device = nullptr; device->QueryInterface(&dxgi_device);
   IDXGIAdapter *adapter = nullptr; dxgi_device->GetAdapter(&adapter);
   IDXGIFactory2 *factory = nullptr; adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
   BOOL tearing = FALSE;
   { IDXGIFactory5 *f5 = nullptr; if (SUCCEEDED(factory->QueryInterface(&f5))) { f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)); f5->Release(); } }
   DXGI_SWAP_CHAIN_DESC1 desc{};
   desc.Width = (UINT)width; desc.Height = (UINT)height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
   desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = buffers; desc.SwapEffect = effect;
   desc.Scaling = flip ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
   const bool tear = flip && tearing && interval == 0;
   desc.Flags = tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
   IDXGISwapChain1 *swap = nullptr;
   hr = factory->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &swap);
   if (FAILED(hr)) { printf("CreateSwapChainForHwnd %#lx\n", (unsigned long)hr); return 5; }
   factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
   /* D3D11_FLIP_FULLSCREEN=1: DXGI's full-screen state (same mode), left again before exit. */
   char fullscreen_env[8] = "";
   const bool fullscreen = GetEnvironmentVariableA("D3D11_FLIP_FULLSCREEN", fullscreen_env, sizeof(fullscreen_env)) && fullscreen_env[0] == '1';
   if (fullscreen) {
      hr = swap->SetFullscreenState(TRUE, nullptr);
      printf("SetFullscreenState(TRUE) = %#lx\n", (unsigned long)hr);
      if (SUCCEEDED(hr)) swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, desc.Flags);
   }
   LARGE_INTEGER f, start, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
   unsigned frames = 0;
   for (;;) {
      ID3D11Texture2D *back = nullptr; swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
      ID3D11RenderTargetView *rtv = nullptr; device->CreateRenderTargetView(back, nullptr, &rtv);
      const float level = (frames % 120) / 120.0f, color[4] = {level, 0.25f, 1.0f - level, 1.0f};
      context->OMSetRenderTargets(1, &rtv, nullptr);
      context->ClearRenderTargetView(rtv, color);
      rtv->Release(); back->Release();
      hr = swap->Present(interval, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
      if (FAILED(hr)) { printf("Present %#lx\n", (unsigned long)hr); break; }
      ++frames; pump();
      QueryPerformanceCounter(&now);
      if ((now.QuadPart - start.QuadPart) / (double)f.QuadPart >= seconds) break;
   }
   QueryPerformanceCounter(&now);
   const double elapsed = (now.QuadPart - start.QuadPart) / (double)f.QuadPart;
   printf("d3d11 %s, %u buffers, interval %u, %dx%d%s: %u frames in %.1f s = %.1f FPS\n", effect_name, buffers, interval, width, height,
          tear ? ", tearing allowed" : "", frames, elapsed, frames / elapsed);
   if (fullscreen) swap->SetFullscreenState(FALSE, nullptr);
   swap->Release(); factory->Release(); adapter->Release(); dxgi_device->Release(); context->Release(); device->Release();
   DestroyWindow(window); pump();
   return 0;
}
