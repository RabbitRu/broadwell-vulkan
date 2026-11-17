/* Experiment: zero-copy windowed present without D3D.
 *
 * The Intel OpenGL ICD presents a frame by writing it into a shared surface
 * the size of the window and submitting with PresentRedirected and the token
 * (0x80000000 | HWND) << 32 | shared handle (see tools/trace-gl-present.ps1).
 * Its surface comes from dwmapi!DwmpDxGetWindowSharedSurface, which uses D3D11.
 * Here the surface is created by our own transport from the traced Intel
 * descriptor and D3D runtime data; frames are solid colours written by the
 * CPU. Pass: the composed window (screen pixels and PrintWindow) shows each
 * colour. The window closes itself.
 *
 * Usage: PresentRedirectProbe.exe <surface-descriptor.bin> <surface-runtime.bin> */
#define NOMINMAX
#include "hasvk_wddm.hpp"
#include <dwmapi.h>
#include <dxgiformat.h>
#include <intrin.h>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace hasvk::wddm;

static std::vector<uint8_t> load(const char *path, size_t bytes)
{
   std::ifstream file(path, std::ios::binary);
   std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
   if (data.size() != bytes) throw std::runtime_error(std::string("Unexpected size: ") + path);
   return data;
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_ERASEBKGND) return 1;   // never let GDI paint over the surface
   return DefWindowProcW(window, message, w, l);
}
static void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
/* BGRA of the composed screen and of PrintWindow at the window centre. */
static void sample(HWND window, uint32_t &screen, uint32_t &printed)
{
   RECT r; GetWindowRect(window, &r);
   HDC desktop = GetDC(nullptr);
   const COLORREF s = GetPixel(desktop, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
   ReleaseDC(nullptr, desktop);
   screen = GetRValue(s) << 16 | GetGValue(s) << 8 | GetBValue(s);
   HDC dc = GetDC(window), memory = CreateCompatibleDC(dc);
   HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right - r.left, r.bottom - r.top);
   HGDIOBJ old = SelectObject(memory, bitmap);
   PrintWindow(window, memory, 2 /* PW_RENDERFULLCONTENT */);
   const COLORREF p = GetPixel(memory, (r.right - r.left) / 2, (r.bottom - r.top) / 2);
   printed = GetRValue(p) << 16 | GetGValue(p) << 8 | GetBValue(p);
   SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
}

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: PresentRedirectProbe <descriptor.bin> <runtime.bin>\n"); return 2; }
   int failures = 0;
   bool dpi_aware = true;   // option "nodpi": DPI-virtualized like most applications (vkcube)
   for (int a = 3; a < argc; ++a) if (!strcmp(argv[a], "nodpi")) dpi_aware = false;
   if (dpi_aware) SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // physical screen coordinates
   printf("DPI aware: %s, system DPI %u\n", dpi_aware ? "yes" : "no", GetDpiForSystem());
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);            // a sleeping display makes every present "occluded"
   Sleep(1500);
   try {
      auto descriptor = load(argv[1], 1096), runtime = load(argv[2], 104);
      uint32_t width, height; memcpy(&width, descriptor.data() + 0x28, 4); memcpy(&height, descriptor.data() + 0x30, 4);
      /* Option "size=WxH": the same descriptor and runtime data for another window size. */
      for (int a = 3; a < argc; ++a) if (!strncmp(argv[a], "size=", 5) && sscanf_s(argv[a] + 5, "%ux%u", &width, &height) == 2) {
         memcpy(descriptor.data() + 0x28, &width, 4); memcpy(descriptor.data() + 0x30, &height, 4);
         memcpy(runtime.data() + 0x3c, &width, 4); memcpy(runtime.data() + 0x40, &height, 4);
      }
      /* Option "glcontext": context as the Intel OpenGL ICD creates it
       * (ClientHint OPENGL; private data = pointer to a 0x2cdc-byte buffer, its size, 0x100). */
      bool gl_context = false; int hint = -1;
      for (int a = 3; a < argc; ++a) {
         if (!strcmp(argv[a], "glcontext")) gl_context = true;
         if (!strncmp(argv[a], "hint=", 5)) hint = atoi(argv[a] + 5);   // override the client hint alone
      }
      if (hint < 0) hint = gl_context ? 1 : 4;
      Device device((uint32_t)hint, {}, gl_context);
      printf("context: ClientHint=%d, presentable private data: %s\n", hint, gl_context ? "yes" : "no");
      printf("adapter: %ls; surface %ux%u\n", device.adapter_name.c_str(), width, height);

      WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PresentRedirectProbe";
      wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc);
      /* Option "framed": an ordinary overlapped window whose client area is
       * width x height; "framed-window" sizes the surface to the whole window
       * rectangle instead (as dwmapi did for the traced OpenGL window). */
      bool framed = false, window_sized = false;
      for (int a = 3; a < argc; ++a) { if (!strcmp(argv[a], "framed")) framed = true; if (!strcmp(argv[a], "framed-window")) framed = window_sized = true; }
      RECT outer{0, 0, (LONG)width, (LONG)height};
      if (framed) AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOPMOST);
      HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"present redirect probe", (framed ? WS_OVERLAPPEDWINDOW : WS_POPUP) | WS_VISIBLE,
                                    120, 120, outer.right - outer.left, outer.bottom - outer.top, nullptr, nullptr, wc.hInstance, nullptr);
      if (!window) throw std::runtime_error("CreateWindowEx");
      if (window_sized) {
         width = outer.right - outer.left; height = outer.bottom - outer.top;
         memcpy(descriptor.data() + 0x28, &width, 4); memcpy(descriptor.data() + 0x30, &height, 4);
         memcpy(runtime.data() + 0x3c, &width, 4); memcpy(runtime.data() + 0x40, &height, 4);
      }
      printf("window %s, surface %ux%u\n", framed ? "framed" : "popup", width, height);
      /* Option "covered": another topmost window fully covers the probe window. */
      for (int a = 3; a < argc; ++a) if (!strcmp(argv[a], "covered")) {
         RECT r; GetWindowRect(window, &r);
         HWND cover = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"cover", WS_POPUP | WS_VISIBLE, r.left - 20, r.top - 20,
                                      r.right - r.left + 40, r.bottom - r.top + 40, nullptr, nullptr, wc.hInstance, nullptr);
         printf("covered by %p\n", (void *)cover);
      }
      pump(); DwmFlush(); Sleep(100); pump();
      /* Option "pixelformat": mark the window as an OpenGL window first
       * (gdi32 SetPixelFormat; for this experiment only, it loads opengl32). */
      for (int a = 3; a < argc; ++a) if (!strcmp(argv[a], "pixelformat")) {
         HDC dc = GetDC(window);
         PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
         const int format = ChoosePixelFormat(dc, &pfd);
         printf("SetPixelFormat(%d) = %d\n", format, format ? SetPixelFormat(dc, format, &pfd) : 0);
         ReleaseDC(window, dc);
      }
      uint32_t screen0, printed0; sample(window, screen0, printed0);
      printf("before any present: screen=%06x printwindow=%06x\n", screen0, printed0);

      /* Mode "own": the shared surface is created here, as D3D11 created it in
       * the trace, and the token is built from HWND and handle. Mode "dwmapi":
       * surface and token (update id) come from DwmpDxGetWindowSharedSurface,
       * called before every frame as opengl32 does. */
      const bool use_dwmapi = argc > 3 && !strcmp(argv[3], "dwmapi");
      typedef HRESULT(WINAPI *GetSharedSurface)(HWND, LUID, HMONITOR, DWORD, DXGI_FORMAT *, HANDLE *, UINT64 *);
      auto get_shared_surface = reinterpret_cast<GetSharedSurface>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmpDxGetWindowSharedSurface"));
      D3DKMT_CREATEALLOCATION create{};
      D3DDDI_ALLOCATIONINFO2 info{};
      uint64_t update_id = 0;
      auto dwm_surface = [&](unsigned frame) {
         DXGI_FORMAT format{}; HANDLE handle{}; UINT64 id = 0;
         const HRESULT hr = get_shared_surface(window, device.adapter_luid(), nullptr, 0, &format, &handle, &id);
         printf("frame %u: DwmpDxGetWindowSharedSurface hr=%#lx format=%d handle=%p updateId=%#llx\n", frame, (unsigned long)hr, (int)format, handle,
                (unsigned long long)id);
         if (FAILED(hr) || !handle) throw std::runtime_error("DwmpDxGetWindowSharedSurface failed");
         if (create.hGlobalShare && create.hGlobalShare != (D3DKMT_HANDLE)(uintptr_t)handle) throw std::runtime_error("window surface changed");
         create.hGlobalShare = (D3DKMT_HANDLE)(uintptr_t)handle; update_id = id;
      };
      if (use_dwmapi) {
         if (!get_shared_surface) throw std::runtime_error("dwmapi has no DwmpDxGetWindowSharedSurface");
         dwm_surface(0);
      } else {
         info.pPrivateDriverData = descriptor.data(); info.PrivateDriverDataSize = 1096;
         create.hDevice = device.handle(); create.NumAllocations = 1; create.pAllocationInfo2 = &info;
         create.pPrivateRuntimeData = runtime.data(); create.PrivateRuntimeDataSize = 104;
         create.Flags.CreateResource = 1; create.Flags.CreateShared = 1;
         check(D3DKMTCreateAllocation2(&create), "CreateAllocation2 (shared surface)");
      }
      printf("surface (%s): hResource=%#x hGlobalShare=%#x\n", use_dwmapi ? "dwmapi" : "own", create.hResource, create.hGlobalShare);

      /* What the kernel driver filled in (pitch, size, layout), as the ICD reads it back. */
      D3DKMT_QUERYRESOURCEINFO query{}; query.hDevice = device.handle(); query.hGlobalShare = create.hGlobalShare;
      check(D3DKMTQueryResourceInfo(&query), "QueryResourceInfo");
      std::vector<uint8_t> opened_runtime(query.PrivateRuntimeDataSize), opened(query.TotalPrivateDriverDataSize);
      D3DDDI_OPENALLOCATIONINFO2 open_info{};
      D3DKMT_OPENRESOURCE open{}; open.hDevice = device.handle(); open.hGlobalShare = create.hGlobalShare; open.NumAllocations = 1;
      open.pOpenAllocationInfo2 = &open_info; open.pPrivateRuntimeData = opened_runtime.data(); open.PrivateRuntimeDataSize = (UINT)opened_runtime.size();
      open.pTotalPrivateDriverDataBuffer = opened.data(); open.TotalPrivateDriverDataBufferSize = (UINT)opened.size();
      check(D3DKMTOpenResource2(&open), "OpenResource2");
      uint64_t pitch = 0, size = 0; memcpy(&pitch, opened.data() + 0xc0, 8); memcpy(&size, opened.data() + 0xd0, 8);
      uint32_t layout0, layout1; memcpy(&layout0, opened.data() + 0x18, 4); memcpy(&layout1, opened.data() + 0x1c, 4);
      printf("kernel layout: pitch=%#llx size=%#llx flags=%08x %08x\n", (unsigned long long)pitch, (unsigned long long)size, layout0, layout1);

      /* Not CPU-lockable (Lock2 fails with STATUS_INVALID_PARAMETER): map it for
       * the GPU and fill it there. A solid colour does not depend on tiling. */
      if (!size || (size & 4095)) throw std::runtime_error("Unexpected surface size");
      D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue = device.queue(); map.hAllocation = open_info.hAllocation;
      map.MinimumAddress = gpu_min; map.MaximumAddress = gpu_max; map.SizeInPages = size / 4096; map.Protection.Value = 1;
      check(D3DKMTMapGpuVirtualAddress(&map), "MapGpuVirtualAddress (shared surface)");
      device.wait_paging(map.PagingFenceValue);
      D3DKMT_HANDLE surface_allocation = open_info.hAllocation;
      D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue = device.queue(); resident.NumAllocations = 1; resident.AllocationList = &surface_allocation;
      check(D3DKMTMakeResident(&resident), "MakeResident (shared surface)");
      device.wait_paging(resident.PagingFenceValue);
      const uint64_t surface_va = map.VirtualAddress & 0xffffffffffffull;
      printf("surface GPU VA %#llx\n", (unsigned long long)surface_va);

      /* fill: one MI_STORE_DATA_IMM (qword) per 8 bytes, then MI_BATCH_BUFFER_END. */
      const uint64_t fill_bytes = (size / 8) * 20 + 8, fill_alloc = (fill_bytes + 65535) & ~65535ull;
      Buffer fill(device, linear_metadata(fill_alloc), fill_alloc);
      Buffer entry(device, linear_metadata(8192), 8192);
      const uint64_t fill_va = fill.address() & 0xffffffffffffull;
      auto *jump = static_cast<uint32_t *>(entry.cpu());
      jump[0] = 0x18800101; jump[1] = (uint32_t)fill_va; jump[2] = (uint32_t)(fill_va >> 32); jump[3] = 0;   // MI_BATCH_BUFFER_START (PPGTT), MI_NOOP
      uint64_t token = (0x80000000ull | (uint32_t)(uintptr_t)window) << 32 | create.hGlobalShare;
      printf("present token %#llx\n", (unsigned long long)token);

      /* Option "glheader": submission header as the Intel OpenGL ICD sends it (client 6). */
      std::vector<uint8_t> header = command_header_8192();
      for (int a = 3; a < argc; ++a) if (!strcmp(argv[a], "glheader")) {
         const uint32_t fields[][2] = {{0, 0x00141433}, {4, 0x428}, {8, 6}, {20, 0x2000}};
         for (const auto &field : fields) memcpy(header.data() + field[0], &field[1], 4);
         printf("using the OpenGL ICD submission header\n");
      }
      /* Records the fill batch: colour_at(byte offset) gives the RGB of each 8 bytes. */
      auto record_fill = [&](auto &&colour_at) {
         auto *p = static_cast<uint32_t *>(fill.cpu());
         for (uint64_t offset = 0; offset < size; offset += 8) {
            const uint64_t a = surface_va + offset; const uint32_t bgra = 0xff000000u | colour_at(offset);
            *p++ = 0x10200003; *p++ = (uint32_t)a; *p++ = (uint32_t)(a >> 32); *p++ = bgra; *p++ = bgra;
         }
         return p;
      };
      auto screen_at = [&](int x, int y) {
         RECT r; GetWindowRect(window, &r); HDC desktop = GetDC(nullptr);
         const COLORREF s = GetPixel(desktop, r.left + x, r.top + y); ReleaseDC(nullptr, desktop);
         return (uint32_t)(GetRValue(s) << 16 | GetGValue(s) << 8 | GetBValue(s));
      };
      const uint32_t colours[3] = {0xff8000, 0x20c040, 0x3050e0};   // RGB
      for (unsigned f = 0; f < 4; ++f) {
         if (use_dwmapi) { if (f) dwm_surface(f); token = update_id; }
         /* Frame 3 (layout): blue, bytes [512,1024) yellow, [4096,4608) magenta.
          * Linear (pitch 0x500) puts yellow at (130,0); X-tiling at (2,1);
          * Y-tiling at (5,10), with magenta at (33,10) if tiles are row-major. */
         auto *p = f < 3 ? record_fill([&](uint64_t) { return colours[f]; })
                         : record_fill([](uint64_t o) { return o >= 512 && o < 1024 ? 0xffff00u : o >= 4096 && o < 4608 ? 0xff00ffu : 0x0000ffu; });
         /* Read back the first and the middle dword through the GPU (MI_COPY_MEM_MEM) into entry + 4096. */
         const uint64_t probes[2] = {0, size / 2};
         for (unsigned i = 0; i < 2; ++i) {
            const uint64_t dst = (entry.address() & 0xffffffffffffull) + 4096 + 4 * i, src = surface_va + probes[i];
            *p++ = 0x17000003; *p++ = (uint32_t)dst; *p++ = (uint32_t)(dst >> 32); *p++ = (uint32_t)src; *p++ = (uint32_t)(src >> 32);
         }
         *p++ = 0x05000000; *p++ = 0;   // MI_BATCH_BUFFER_END, MI_NOOP
         if ((p - static_cast<uint32_t *>(fill.cpu())) & 1) *p++ = 0;
         static_cast<uint32_t *>(entry.cpu())[1024] = 0xdeadbeef; static_cast<uint32_t *>(entry.cpu())[1025] = 0xdeadbeef;
         device.submit_and_wait(entry.address(), 16, header, 15000, token);
         printf("frame %u: GPU readback of surface %08x %08x\n", f, static_cast<uint32_t *>(entry.cpu())[1024], static_cast<uint32_t *>(entry.cpu())[1025]);
         DwmFlush(); pump(); Sleep(150); DwmFlush(); pump();
         if (f == 3) {
            const struct { const char *layout; int x, y; } points[] = {{"linear", 130, 0}, {"X-tiled", 2, 1}, {"Y-tiled", 5, 10}, {"Y next tile", 33, 10}, {"background", 200, 200}};
            for (const auto &pt : points) printf("layout: %-12s (%3d,%3d) = %06x\n", pt.layout, pt.x, pt.y, screen_at(pt.x, pt.y));
            continue;
         }
         uint32_t screen, printed; sample(window, screen, printed);
         const bool ok = screen == colours[f];
         printf("frame %u: expected %06x screen=%06x printwindow=%06x %s\n", f, colours[f], screen, printed, ok ? "PASS" : "FAIL");
         if (!ok) ++failures;
      }
      static const wchar_t *forbidden[] = {L"d3d11.dll", L"d3d12.dll", L"igd10iumd64.dll", L"igd12umd64.dll", L"igdumdim64.dll", L"ig8icd64.dll"};
      for (const wchar_t *m : forbidden) if (GetModuleHandleW(m)) { printf("loaded: %ls\n", m); if (!use_dwmapi) ++failures; }
      if (!use_dwmapi) printf("no D3D runtime or Intel UMD/ICD loaded: %s\n", failures ? "NO" : "yes");
      DestroyWindow(window); pump();
      D3DKMT_FREEGPUVIRTUALADDRESS free_va{}; free_va.hAdapter = 0; (void)free_va;   // VA is released with the allocation
      D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.hDevice = device.handle(); destroy.hResource = open.hResource; D3DKMTDestroyAllocation2(&destroy);
      if (create.hResource) { destroy = {}; destroy.hDevice = device.handle(); destroy.hResource = create.hResource; D3DKMTDestroyAllocation2(&destroy); }
   } catch (const std::exception &e) {
      fprintf(stderr, "error: %s\n", e.what());
      return 1;
   }
   printf(failures ? "RESULT: FAIL (%d frames)\n" : "RESULT: PASS\n", failures);
   return failures ? 1 : 0;
}
