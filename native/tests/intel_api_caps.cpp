/* What Intel's own driver offers on this machine through Direct3D 11,
 * Direct3D 12 and OpenGL (why GravityMark starts on none of them). This runs
 * Intel's UMDs in its own process; it does not touch the Vulkan ICD.
 * Build: clang++ -std=c++17 intel_api_caps.cpp -ld3d11 -ldxgi -lopengl32 -lgdi32 -luser32 */
#define NOMINMAX
#include <windows.h>
#include <d3d11_3.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static void d3d11_caps()
{
   ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr; D3D_FEATURE_LEVEL level{};
   const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
   printf("D3D11CreateDevice: hr=0x%08lx feature level %x.%x\n", hr, (level >> 12) & 0xf, (level >> 8) & 0xf);
   if (FAILED(hr)) return;
   D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
   hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2));
   printf("OPTIONS2 hr=0x%08lx: TypedUAVLoadAdditionalFormats=%d ROVs=%d ConservativeRaster=%d StandardSwizzle=%d UnifiedMemory=%d\n",
          hr, o2.TypedUAVLoadAdditionalFormats, o2.ROVsSupported, o2.ConservativeRasterizationTier, o2.StandardSwizzle, o2.UnifiedMemoryArchitecture);
   D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
   hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
   printf("OPTIONS3 hr=0x%08lx: VPAndRTArrayIndexFromAnyShaderFeedingRasterizer=%d\n", hr, o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
   for (DXGI_FORMAT f : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16_FLOAT}) {
      D3D11_FEATURE_DATA_FORMAT_SUPPORT2 s{f, 0};
      device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &s, sizeof(s));
      printf("format %d: UAV typed load=%d typed store=%d\n", f, !!(s.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD),
             !!(s.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE));
   }
   /* The same operation as GravityMark's failing occluder kernel: reading a
    * two-channel float UAV. One channel is always allowed. */
   HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
   auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
   if (!compile) { printf("d3dcompiler_47.dll missing\n"); return; }
   const char *sources[] = {
      "RWTexture2D<float> t : register(u0); [numthreads(16,16,1)] void main(uint3 i : SV_DispatchThreadID) { t[i.xy] = t[i.xy] * 0.5; }",
      "RWTexture2D<float2> t : register(u0); [numthreads(16,16,1)] void main(uint3 i : SV_DispatchThreadID) { t[i.xy] = t[i.xy] * 0.5; }",
      "RWTexture2D<float2> t : register(u0); [numthreads(16,16,1)] void main(uint3 i : SV_DispatchThreadID) { t[i.xy] = float2(i.xy); }",
   };
   const char *names[] = {"read+write float (R32)", "read+write float2 (RG32: needs typed UAV load of RG32F)", "write-only float2"};
   for (int k = 0; k < 3; ++k) {
      ID3DBlob *code = nullptr, *errors = nullptr;
      hr = compile(sources[k], strlen(sources[k]), "cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
      if (FAILED(hr)) { printf("compile %s: hr=0x%08lx %s\n", names[k], hr, errors ? (const char *)errors->GetBufferPointer() : ""); continue; }
      ID3D11ComputeShader *cs = nullptr;
      hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
      printf("CreateComputeShader %s: hr=0x%08lx%s\n", names[k], hr, hr == E_INVALIDARG ? " (E_INVALIDARG)" : "");
      if (cs) cs->Release();
      code->Release();
   }
   context->Release(); device->Release();
}

static void d3d12_caps()
{
   HMODULE d3d12 = LoadLibraryA("d3d12.dll");
   auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
   if (!create) { printf("d3d12.dll missing\n"); return; }
   IDXGIFactory4 *factory = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&factory));
   IDXGIAdapter1 *adapter = nullptr;
   for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
      printf("adapter %u: %ls vendor=%04x device=%04x flags=%u\n", i, d.Description, d.VendorId, d.DeviceId, d.Flags);
      if (d.VendorId == 0x8086) break;
      adapter->Release(); adapter = nullptr;
   }
   for (D3D_FEATURE_LEVEL level : {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1}) {
      ID3D12Device *device = nullptr;
      HRESULT hr = create(adapter, level, __uuidof(ID3D12Device), reinterpret_cast<void **>(&device));
      printf("D3D12CreateDevice at %x.%x: hr=0x%08lx\n", (level >> 12) & 0xf, (level >> 8) & 0xf, hr);
      if (device) device->Release();
   }
   if (adapter) adapter->Release();
   if (factory) factory->Release();
}

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int *);
static void gl_caps()
{
   WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "IntelApiCapsGl"; wc.style = CS_OWNDC;
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);   /* never shown */
   HDC dc = GetDC(hwnd);
   PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
   HGLRC legacy = wglCreateContext(dc); wglMakeCurrent(dc, legacy);
   typedef const unsigned char *(WINAPI *GetString)(unsigned);
   auto get = reinterpret_cast<GetString>(GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString"));
   printf("OpenGL legacy context: %s | %s | %s\n", get(0x1F00), get(0x1F01), get(0x1F02));
   auto attribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
   for (int version : {46, 45, 44, 43}) {
      const int list[] = {0x2091, version / 10, 0x2092, version % 10, 0x9126, 0x00000001 /* core */, 0};
      HGLRC ctx = attribs ? attribs(dc, nullptr, list) : nullptr;
      printf("wglCreateContextAttribsARB %d.%d core: %s (error %lu)\n", version / 10, version % 10, ctx ? "ok" : "FAILED", ctx ? 0 : GetLastError());
      if (ctx) wglDeleteContext(ctx);
   }
   wglMakeCurrent(nullptr, nullptr); wglDeleteContext(legacy); ReleaseDC(hwnd, dc); DestroyWindow(hwnd);
}

int main()
{
   printf("== Direct3D 11\n"); d3d11_caps();
   printf("== Direct3D 12\n"); d3d12_caps();
   printf("== OpenGL\n"); gl_caps();
   return 0;
}
