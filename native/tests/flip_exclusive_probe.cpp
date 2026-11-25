/* Experiment: frames on the screen without the compositor and without D3D.
 *
 * What DXGI does in its full-screen state (tools/dev/trace-d3d11-present.ps1
 * -Fullscreen), with this project's own device: primaries (allocations with
 * the Primary flag for the output), exclusive ownership of the output
 * (D3DKMTSetVidPnSourceOwner), a mode set to the first primary and one
 * D3DKMTPresent with the Flip flag per frame. DWM does not compose then.
 *
 * Three primaries are filled once (solid red, green, blue, with a marker
 * that tells the tiling) and flipped in turn. Reported: the kernel's layout
 * of a primary, the time of each present, the output's present statistics.
 * Ownership is released before exit; a watchdog thread ends the process if
 * anything blocks, and the kernel releases the output with the device.
 *
 * Usage: FlipExclusiveProbe.exe [frames] [flip interval 0..4] [hold ms after the last flip] [options]
 *   options: nowindow (no hWindow in the presents), shared (owner type shared, no exclusivity),
 *            noowner (flip without taking ownership) */
#define NOMINMAX
#include "hasvk_wddm.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace hasvk::wddm;

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_ERASEBKGND) return 1;
   return DefWindowProcW(window, message, w, l);
}
static void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }

/* The calls are made here one by one (not through Device::acquire_output and
 * Device::flip) so that each status is reported; Intel's description of a
 * primary is the transport's primary_metadata(). */
struct Primary {
   D3DKMT_HANDLE resource = 0, allocation = 0;
   uint64_t pitch = 0, size = 0, va = 0;
   uint32_t tile = 0;
   void *cpu = nullptr;
};

int main(int argc, char **argv)
{
   const int frames = argc > 1 ? atoi(argv[1]) : 120;
   const int interval = argc > 2 ? atoi(argv[2]) : 1;
   const int hold_ms = argc > 3 ? atoi(argv[3]) : 0;
   bool with_window = true, take_owner = true; int owner_type = D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE;
   for (int a = 4; a < argc; ++a) {
      if (!strcmp(argv[a], "nowindow")) with_window = false;
      if (!strcmp(argv[a], "shared")) owner_type = D3DKMT_VIDPNSOURCEOWNER_SHARED;
      if (!strcmp(argv[a], "noowner")) take_owner = false;
   }
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
   /* Never leave the screen owned by a stuck process. */
   std::thread([] { Sleep(40000); fprintf(stderr, "watchdog: 40 s, ending the process\n"); fflush(stderr); TerminateProcess(GetCurrentProcess(), 9); }).detach();
   const uint32_t width = (uint32_t)GetSystemMetrics(SM_CXSCREEN), height = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
   int result = 0;
   try {
      Device device;
      printf("adapter: %ls; screen %ux%u; %d frames, flip interval %d\n", device.adapter_name.c_str(), width, height, frames, interval);
      HWND window = nullptr;
      if (with_window) {
         WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"FlipExclusiveProbe";
         RegisterClassW(&wc);
         window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"flip exclusive probe", WS_POPUP | WS_VISIBLE, 0, 0, (int)width, (int)height,
                                  nullptr, nullptr, wc.hInstance, nullptr);
         if (!window) throw std::runtime_error("CreateWindowEx");
         SetForegroundWindow(window); pump();
      }

      Primary primaries[3];
      const uint32_t colours[3] = {0xffff0000, 0xff00ff00, 0xff0000ff};   /* A8R8G8B8 */
      for (int i = 0; i < 3; ++i) {
         Primary &p = primaries[i];
         auto metadata = primary_metadata(width, height);
         D3DDDI_ALLOCATIONINFO2 info{};
         info.pPrivateDriverData = metadata.data(); info.PrivateDriverDataSize = (UINT)metadata.size();
         info.VidPnSourceId = 0; info.Flags.Primary = 1;
         D3DKMT_CREATEALLOCATION create{};
         create.hDevice = device.handle(); create.NumAllocations = 1; create.pAllocationInfo2 = &info; create.Flags.CreateResource = 1;
         check(D3DKMTCreateAllocation2(&create), "CreateAllocation2 (primary)");
         p.resource = create.hResource; p.allocation = info.hAllocation;
         uint32_t flags1c, align; memcpy(&flags1c, metadata.data() + 0x1c, 4); memcpy(&align, metadata.data() + 0x58, 4);
         memcpy(&p.pitch, metadata.data() + 0xc0, 8); memcpy(&p.size, metadata.data() + 0xd0, 8); memcpy(&p.tile, metadata.data() + 0x170, 4);
         if (!i) printf("primary: pitch %llu, size %llu, tile mode %u (1 = X, 2 = Y), flags %#x, alignment %#x\n", (unsigned long long)p.pitch,
                        (unsigned long long)p.size, p.tile, flags1c, align);
         if (!p.size || (p.size & 4095) || p.pitch < width * 4ull) throw std::runtime_error("Unexpected primary layout");
         D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue = device.queue(); map.hAllocation = p.allocation;
         map.MinimumAddress = gpu_min; map.MaximumAddress = gpu_max; map.SizeInPages = p.size / 4096; map.Protection.Value = 1;
         check(D3DKMTMapGpuVirtualAddress(&map), "MapGpuVirtualAddress (primary)");
         device.wait_paging(map.PagingFenceValue);
         p.va = map.VirtualAddress;
         D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue = device.queue(); resident.NumAllocations = 1; resident.AllocationList = &p.allocation;
         check(D3DKMTMakeResident(&resident), "MakeResident (primary)");
         device.wait_paging(resident.PagingFenceValue);
      }
      /* A primary cannot be locked for the CPU (Lock2: STATUS_INVALID_PARAMETER):
       * the GPU fills it, one MI_STORE_DATA_IMM (qword) per 8 bytes. Solid
       * colour, and white in the first 4 KiB of memory: an X-tiled surface
       * shows that as a 128x8 block at the top left, a linear one as 1024
       * pixels of the first row. */
      {
         const uint64_t size = primaries[0].size, fill_bytes = (size / 8) * 20 + 8, fill_alloc = (fill_bytes + 65535) & ~65535ull;
         Buffer fill(device, linear_metadata(fill_alloc), fill_alloc);
         Buffer entry(device, linear_metadata(8192), 8192);
         const uint64_t fill_va = fill.address() & 0xffffffffffffull;
         auto *jump = static_cast<uint32_t *>(entry.cpu());
         jump[0] = 0x18800101; jump[1] = (uint32_t)fill_va; jump[2] = (uint32_t)(fill_va >> 32); jump[3] = 0;   /* MI_BATCH_BUFFER_START (PPGTT), MI_NOOP */
         const std::vector<uint8_t> header = command_header_8192();
         for (int i = 0; i < 3; ++i) {
            const uint64_t va = primaries[i].va & 0xffffffffffffull;
            auto *p = static_cast<uint32_t *>(fill.cpu());
            for (uint64_t offset = 0; offset < size; offset += 8) {
               const uint64_t a = va + offset; const uint32_t bgra = offset < 4096 ? 0xffffffffu : colours[i];
               *p++ = 0x10200003; *p++ = (uint32_t)a; *p++ = (uint32_t)(a >> 32); *p++ = bgra; *p++ = bgra;
            }
            *p++ = 0x05000000; *p++ = 0;   /* MI_BATCH_BUFFER_END, MI_NOOP */
            LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
            device.submit_and_wait(entry.address(), 16, header, 30000);
            QueryPerformanceCounter(&q1);
            printf("primary %d filled by the GPU in %.0f ms\n", i, (q1.QuadPart - q0.QuadPart) * 1e3 / qf.QuadPart);
         }
      }

      if (take_owner) {
         D3DKMT_VIDPNSOURCEOWNER_TYPE type = (D3DKMT_VIDPNSOURCEOWNER_TYPE)owner_type; D3DDDI_VIDEO_PRESENT_SOURCE_ID source = 0;
         D3DKMT_SETVIDPNSOURCEOWNER owner{}; owner.hDevice = device.handle(); owner.pType = &type; owner.pVidPnSourceId = &source; owner.VidPnSourceCount = 1;
         const LONG owned = D3DKMTSetVidPnSourceOwner(&owner);
         printf("SetVidPnSourceOwner(type %d) = %#lx\n", owner_type, (unsigned long)owned);
         if (owned < 0) throw std::runtime_error("The output could not be taken");
      }
      auto set_mode = [&](D3DKMT_HANDLE primary) {
         D3DKMT_SETDISPLAYMODE mode{}; mode.hDevice = device.handle(); mode.hPrimaryAllocation = primary;
         mode.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE; mode.DisplayOrientation = D3DDDI_ROTATION_IDENTITY;
         return D3DKMTSetDisplayMode(&mode);
      };
      printf("SetDisplayMode = %#lx\n", (unsigned long)set_mode(primaries[0].allocation));

      LARGE_INTEGER f, t0, t1, start, end; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
      double worst = 0, total = 0; int presented = 0, mode_changes = 0; LONG last = 0;
      RECT rect{0, 0, (LONG)width, (LONG)height};
      for (int n = 0; n < frames; ++n) {
         D3DKMT_PRESENT present{};
         present.hContext = device.context(); present.hWindow = window; present.VidPnSourceId = 0;
         present.hSource = primaries[(n + 1) % 3].allocation;
         present.SrcRect = rect; present.SubRectCnt = 1; present.pSrcSubRects = &rect;
         present.PresentCount = (UINT)(n + 1); present.FlipInterval = (D3DDDI_FLIPINTERVAL_TYPE)interval;
         present.Flags.Flip = 1; present.Flags.PresentCountValid = 1;
         QueryPerformanceCounter(&t0);
         last = D3DKMTPresent(&present);
         if (last == (LONG)0xC01E0005) {   /* STATUS_GRAPHICS_PRESENT_MODE_CHANGED */
            ++mode_changes; set_mode(present.hSource); last = D3DKMTPresent(&present);
         }
         QueryPerformanceCounter(&t1);
         const double ms = (t1.QuadPart - t0.QuadPart) * 1e3 / f.QuadPart;
         if (n < 4 || last < 0) printf("present %d: %#lx in %.3f ms\n", n, (unsigned long)last, ms);
         if (last < 0) break;
         ++presented; total += ms; if (ms > worst) worst = ms;
         if (window) pump();
      }
      QueryPerformanceCounter(&end);
      const double seconds = (end.QuadPart - start.QuadPart) / (double)f.QuadPart;
      printf("%d presents in %.2f s = %.1f per second; mean %.3f ms, worst %.3f ms; %d mode changes; last status %#lx\n", presented, seconds,
             presented / seconds, presented ? total / presented : 0, worst, mode_changes, (unsigned long)last);
      D3DKMT_GETDEVICESTATE state{}; state.hDevice = device.handle(); state.StateType = D3DKMT_DEVICESTATE_PRESENT; state.PresentState.VidPnSourceId = 0;
      const LONG queried = D3DKMTGetDeviceState(&state);
      printf("present state %#lx: PresentCount %u, PresentRefreshCount %u, SyncRefreshCount %u\n", (unsigned long)queried,
             state.PresentState.PresentStats.PresentCount, state.PresentState.PresentStats.PresentRefreshCount,
             state.PresentState.PresentStats.SyncRefreshCount);
      if (hold_ms > 0) { fflush(stdout); Sleep((DWORD)hold_ms); }
      if (presented != frames) result = 1;

      if (take_owner) {
         D3DKMT_SETVIDPNSOURCEOWNER release{}; release.hDevice = device.handle();
         printf("release the output: %#lx\n", (unsigned long)D3DKMTSetVidPnSourceOwner(&release));
      }
      for (Primary &p : primaries) {
         D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.hDevice = device.handle(); destroy.hResource = p.resource;
         D3DKMTDestroyAllocation2(&destroy);
      }
      if (window) { DestroyWindow(window); pump(); }
      static const wchar_t *forbidden[] = {L"d3d11.dll", L"d3d12.dll", L"igd10iumd64.dll", L"igd12umd64.dll", L"ig8icd64.dll"};
      for (const wchar_t *m : forbidden) if (GetModuleHandleW(m)) { printf("loaded: %ls\n", m); result = 1; }
   } catch (const std::exception &e) {
      fprintf(stderr, "error: %s\n", e.what());
      return 1;
   }
   printf(result ? "RESULT: FAIL\n" : "RESULT: PASS\n");
   fflush(stdout);
   return result;
}
