#include "hasvk_wddm.h"
#include "hasvk_wddm.hpp"
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
using namespace hasvk::wddm;

struct hasvk_wddm_device {
   Device native;
   std::mutex mutex;
   std::unordered_map<uint64_t, std::unique_ptr<VaRange>> ranges;
   std::unordered_set<struct hasvk_wddm_bo *> buffers;
   std::unordered_set<struct hasvk_wddm_readonly_pool *> pools;
};
struct hasvk_wddm_bo {
   hasvk_wddm_device *device;
   uint64_t reservation;
   std::unique_ptr<Buffer> native;
};
struct hasvk_wddm_readonly_pool {
   hasvk_wddm_device *device;
   uint64_t reservation, address;
   uint32_t bytes;
   void *cpu = nullptr;
   std::vector<std::unique_ptr<Buffer>> chunks;
   ~hasvk_wddm_readonly_pool() {
      chunks.clear();
      if (cpu) VirtualFree(cpu, 0, MEM_RELEASE);
   }
};
static thread_local char diagnostic[1024];
static int failure(const char *message)
{
   snprintf(diagnostic, sizeof(diagnostic), "%s", message);
   return -1;
}
template<typename Function> static int checked(Function &&function)
{
   diagnostic[0] = 0;
   try { function(); return 0; }
   catch (const std::exception &error) { return failure(error.what()); }
   catch (...) { return failure("Unknown WDDM C++ exception"); }
}
extern "C" const char *hasvk_wddm_error(void) { return diagnostic; }
extern "C" int hasvk_wddm_verify_no_umd(void)
{
   return checked([] {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      if (snapshot == INVALID_HANDLE_VALUE) throw Error("Module snapshot", HRESULT_FROM_WIN32(GetLastError()));
      MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
      bool forbidden = false;
      BOOL found = Module32FirstW(snapshot, &entry);
      if (!found) { CloseHandle(snapshot); throw std::runtime_error("Cannot enumerate process modules"); }
      do {
         std::wstring name(entry.szModule);
         std::transform(name.begin(), name.end(), name.begin(), towlower);
         forbidden |= name.rfind(L"igd", 0) == 0 || name.rfind(L"d3d12", 0) == 0;
      } while (Module32NextW(snapshot, &entry));
      DWORD error = GetLastError(); CloseHandle(snapshot);
      if (error != ERROR_NO_MORE_FILES) throw Error("Module32NextW", HRESULT_FROM_WIN32(error));
      if (forbidden) throw std::runtime_error("Intel UMD or D3D12 is loaded in the test process");
   });
}
extern "C" int hasvk_wddm_device_create(hasvk_wddm_device **out)
{
   if (!out) return failure("Missing device output");
   *out = nullptr;
   return checked([&] { *out = new hasvk_wddm_device; });
}
extern "C" int hasvk_wddm_device_destroy(hasvk_wddm_device *device)
{
   if (!device) return 0;
   return checked([&] {
      {
         std::lock_guard<std::mutex> lock(device->mutex);
         if (!device->buffers.empty() || !device->pools.empty()) throw std::runtime_error("Device still owns WDDM allocations");
         device->ranges.clear();
         // Retain the wrapper on error, so a caller can retry destruction.
         device->native.shutdown();
      }
      delete device;
   });
}
extern "C" int hasvk_wddm_query_video_memory(uint32_t luid_low, int32_t luid_high, uint64_t *budget, uint64_t *usage)
{
   if (!budget || !usage) return failure("Missing video memory arguments");
   return checked([&] {
      D3DKMT_OPENADAPTERFROMLUID open{};
      open.AdapterLuid.LowPart = luid_low; open.AdapterLuid.HighPart = luid_high;
      const NTSTATUS opened = D3DKMTOpenAdapterFromLuid(&open);
      if (opened) throw Error("OpenAdapterFromLuid", opened);
      uint64_t best_budget = 0, best_usage = 0;
      for (const D3DKMT_MEMORY_SEGMENT_GROUP group : {D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL, D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
         D3DKMT_QUERYVIDEOMEMORYINFO query{};
         query.hProcess = nullptr;   /* this process */
         query.hAdapter = open.hAdapter;
         query.MemorySegmentGroup = group;
         if (!D3DKMTQueryVideoMemoryInfo(&query) && query.Budget > best_budget) {
            best_budget = query.Budget; best_usage = query.CurrentUsage;
         }
      }
      D3DKMT_CLOSEADAPTER close{}; close.hAdapter = open.hAdapter;
      D3DKMTCloseAdapter(&close);
      if (!best_budget) throw std::runtime_error("QueryVideoMemoryInfo reported no budget");
      *budget = best_budget; *usage = best_usage;
   });
}
extern "C" int hasvk_wddm_device_get_info(hasvk_wddm_device *device, hasvk_wddm_adapter_info *out)
{
   if (!device || !out) return failure("Missing adapter info arguments");
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      LUID luid = device->native.adapter_luid();
      *out = {device->native.driver_version(), 0x8086, 0x162b, luid.LowPart, luid.HighPart};
   });
}
extern "C" int hasvk_wddm_reserve(hasvk_wddm_device *device, uint64_t bytes, uint64_t requested, uint64_t *out)
{
   if (!device || !out) return failure("Missing VA reservation arguments");
   *out = 0;
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      auto range = std::make_unique<VaRange>(device->native, bytes, requested);
      uint64_t address = range->address();
      device->ranges.emplace(address, std::move(range));
      *out = address;
   });
}
extern "C" int hasvk_wddm_release(hasvk_wddm_device *device, uint64_t address)
{
   if (!device) return failure("Missing device");
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      if (!device->ranges.count(address)) throw std::runtime_error("Unknown VA reservation");
      for (const auto *bo : device->buffers)
         if (bo->reservation == address) throw std::runtime_error("VA reservation still has live allocations");
      for (const auto *pool : device->pools)
         if (pool->reservation == address) throw std::runtime_error("VA reservation still has state pools");
      device->ranges.erase(address);
   });
}
static int bo_create(hasvk_wddm_device *device, uint64_t bytes, uint64_t requested, int writable, bool cpu_map, hasvk_wddm_bo **out);
extern "C" int hasvk_wddm_bo_create(hasvk_wddm_device *device, uint64_t bytes,
                                     uint64_t requested, int writable, hasvk_wddm_bo **out)
{
   return bo_create(device, bytes, requested, writable, true, out);
}
extern "C" int hasvk_wddm_bo_create_unmapped(hasvk_wddm_device *device, uint64_t bytes, uint64_t requested, hasvk_wddm_bo **out)
{
   return bo_create(device, bytes, requested, 1, false, out);
}
static int bo_create(hasvk_wddm_device *device, uint64_t bytes, uint64_t requested, int writable, bool cpu_map, hasvk_wddm_bo **out)
{
   if (!device || !out) return failure("Missing allocation arguments");
   *out = nullptr;
   return checked([&] {
      if (!bytes || (bytes & 4095) || bytes > hasvk::wddm::kMaxLinearAllocationBytes ||
          (requested & 4095)) throw std::runtime_error("Invalid linear allocation size/address");
      auto bo = std::make_unique<hasvk_wddm_bo>();
      bo->device = device;
      bo->reservation = 0;
      if (requested) {
         std::lock_guard<std::mutex> lock(device->mutex);
         for (const auto &entry : device->ranges) {
            if (requested >= entry.first && requested - entry.first <= entry.second->size() &&
                bytes <= entry.second->size() - (requested - entry.first)) bo->reservation = entry.first;
         }
         if (!bo->reservation) throw std::runtime_error("Exact mapping must belong to a reserved VA range");
         for (const auto *pool : device->pools)
            if (requested < pool->address + pool->bytes && pool->address < requested + bytes)
               throw std::runtime_error("Allocation overlaps a state pool");
      }
      /* The kernel calls (CreateAllocation2, Lock2, MapGpuVirtualAddress,
       * MakeResident) run without the device lock: they can take ~10 ms on the
       * paging queue, and submissions and completion waits need the lock. */
      bo->native = std::make_unique<Buffer>(device->native, linear_metadata(bytes), bytes, writable ? 1 : 0, requested, cpu_map);
      std::lock_guard<std::mutex> lock(device->mutex);
      device->buffers.insert(bo.get());
      *out = bo.release();
   });
}
extern "C" int hasvk_wddm_bo_destroy(hasvk_wddm_bo *bo)
{
   if (!bo) return 0;
   return checked([&] {
      {
         std::lock_guard<std::mutex> lock(bo->device->mutex);
         bo->device->buffers.erase(bo);
      }
      delete bo;   /* kernel calls without the device lock, as in bo_create */
   });
}
extern "C" void *hasvk_wddm_bo_map(hasvk_wddm_bo *bo) { return bo ? bo->native->cpu() : nullptr; }
extern "C" uint64_t hasvk_wddm_bo_address(const hasvk_wddm_bo *bo) { return bo ? bo->native->address() : 0; }
extern "C" uint64_t hasvk_wddm_bo_size(const hasvk_wddm_bo *bo) { return bo ? bo->native->size() : 0; }
extern "C" int hasvk_wddm_bo_set_cpu_tracking(hasvk_wddm_bo *bo, int mode)
{
   if (!bo) return failure("Invalid BO");
   return checked([&] { std::lock_guard<std::mutex> lock(bo->device->mutex); bo->native->set_cpu_tracking(static_cast<unsigned>(mode)); });
}
extern "C" int hasvk_wddm_bo_flush(hasvk_wddm_bo *bo)
{
   if (!bo) return failure("Invalid BO");
   return checked([&] { bo->native->flush(); });
}
extern "C" int hasvk_wddm_bo_flush_range(hasvk_wddm_bo *bo, uint64_t offset, uint64_t bytes)
{
   if (!bo) return failure("Invalid BO");
   return checked([&] { bo->native->flush(offset, bytes); });
}
extern "C" int hasvk_wddm_bo_create_window_surface(hasvk_wddm_device *device, uint32_t width, uint32_t height,
                                                    hasvk_wddm_bo **out)
{
   if (!device || !out) return failure("Missing window surface arguments");
   *out = nullptr;
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      auto bo = std::make_unique<hasvk_wddm_bo>();
      bo->device = device; bo->reservation = 0;
      bo->native = std::make_unique<Buffer>(device->native, WindowSurfaceTag{}, width, height);
      device->buffers.insert(bo.get());
      *out = bo.release();
   });
}
extern "C" int hasvk_wddm_bo_window_surface_info(const hasvk_wddm_bo *bo, uint32_t *share, uint64_t *pitch)
{
   if (!bo || !share || !pitch || !bo->native->share()) return failure("Not a window surface");
   *share = bo->native->share(); *pitch = bo->native->pitch();
   return 0;
}
extern "C" int hasvk_wddm_output_screen(hasvk_wddm_device *device, uint32_t *width, uint32_t *height)
{
   if (!device || !width || !height) return failure("Invalid output arguments");
   DISPLAY_DEVICEW display{}; display.cb = sizeof(display);
   for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &display, 0); ++i) {
      if (!(display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
      D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open{};
      wcsncpy_s(open.DeviceName, display.DeviceName, _TRUNCATE);
      if (D3DKMTOpenAdapterFromGdiDisplayName(&open) < 0) return failure("The primary display's adapter cannot be opened");
      D3DKMT_CLOSEADAPTER close_adapter{open.hAdapter}; D3DKMTCloseAdapter(&close_adapter);
      const LUID luid = device->native.adapter_luid();
      if (open.AdapterLuid.LowPart != luid.LowPart || open.AdapterLuid.HighPart != luid.HighPart || open.VidPnSourceId != 0)
         return failure("The primary display is not output 0 of this adapter");
      DEVMODEW mode{}; mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayOrientation != DMDO_DEFAULT)
         return failure("The primary display's mode is unknown or rotated");
      *width = mode.dmPelsWidth; *height = mode.dmPelsHeight;
      return 0;
   }
   return failure("No primary display");
}
extern "C" int hasvk_wddm_bo_create_primary(hasvk_wddm_device *device, uint32_t width, uint32_t height, hasvk_wddm_bo **out)
{
   if (!device || !out) return failure("Missing primary arguments");
   *out = nullptr;
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      auto bo = std::make_unique<hasvk_wddm_bo>();
      bo->device = device; bo->reservation = 0;
      bo->native = std::make_unique<Buffer>(device->native, PrimaryTag{}, width, height);
      device->buffers.insert(bo.get());
      *out = bo.release();
   });
}
extern "C" int hasvk_wddm_bo_primary_info(const hasvk_wddm_bo *bo, uint64_t *pitch)
{
   if (!bo || !pitch || !bo->native->primary()) return failure("Not a primary");
   *pitch = bo->native->pitch();
   return 0;
}
extern "C" int hasvk_wddm_output_acquire(hasvk_wddm_device *device, hasvk_wddm_bo *primary)
{
   if (!device || !primary || primary->device != device || !primary->native->primary()) return failure("Invalid output arguments");
   std::lock_guard<std::mutex> lock(device->mutex);
   const LONG status = device->native.acquire_output(primary->native->allocation());
   if (status < 0) { snprintf(diagnostic, sizeof(diagnostic), "The output was not given: 0x%08lx", static_cast<unsigned long>(status)); return -1; }
   return 0;
}
extern "C" int hasvk_wddm_output_release(hasvk_wddm_device *device)
{
   if (!device) return failure("Invalid device");
   std::lock_guard<std::mutex> lock(device->mutex);
   device->native.release_output();
   return 0;
}
extern "C" int hasvk_wddm_output_owned(hasvk_wddm_device *device)
{
   if (!device) return 0;
   std::lock_guard<std::mutex> lock(device->mutex);
   return device->native.owns_output();
}
extern "C" int hasvk_wddm_present_flip(hasvk_wddm_device *device, hasvk_wddm_bo *primary, void *hwnd,
                                        uint32_t width, uint32_t height, uint32_t interval)
{
   if (!device || !primary || primary->device != device || !primary->native->primary()) return failure("Invalid flip arguments");
   std::lock_guard<std::mutex> lock(device->mutex);
   const LONG status = device->native.flip(primary->native->allocation(), static_cast<HWND>(hwnd), width, height, interval);
   if (status >= 0) return 0;
   /* Refused: the output is no longer this device's (another owner, the
    * secure desktop, a mode change). It goes back to the desktop. */
   snprintf(diagnostic, sizeof(diagnostic), "Flip refused: 0x%08lx", static_cast<unsigned long>(status));
   device->native.release_output();
   return 1;
}
extern "C" int hasvk_wddm_set_written_primaries(hasvk_wddm_device *device, hasvk_wddm_bo *const *primaries, uint32_t count)
{
   if (!device || (count && !primaries) || count > 4) return failure("Invalid written primaries");
   std::lock_guard<std::mutex> lock(device->mutex);
   D3DKMT_HANDLE handles[4]{};
   for (uint32_t i = 0; i < count; ++i) {
      if (!primaries[i] || primaries[i]->device != device || !primaries[i]->native->primary()) return failure("Not a primary");
      handles[i] = primaries[i]->native->allocation();
   }
   device->native.set_written_primaries(handles, count);
   return 0;
}
/* Waits for submission seq without holding the device lock, so other threads
 * can submit meanwhile. Submissions complete in order. */
static void wait_submission(hasvk_wddm_device *device, uint64_t seq, DWORD timeout_ms)
{
   for (;;) {
      HANDLE event; uint64_t oldest = 0;
      {
         std::lock_guard<std::mutex> lock(device->mutex);
         if (device->native.lost()) throw std::runtime_error("Device/context is lost");
         if (!(event = device->native.pending_event(seq, &oldest))) return;
      }
      const DWORD waited = WaitForSingleObject(event, timeout_ms);
      std::lock_guard<std::mutex> lock(device->mutex);
      uint64_t front = 0;
      const bool still_pending = device->native.pending_event(oldest, &front) && front == oldest;
      if (waited != WAIT_OBJECT_0 && still_pending) {
         device->native.abort_context();
         throw std::runtime_error("GPU completion event timed out or failed");
      }
      if (still_pending) device->native.retire_oldest();
   }
}
static int submit(hasvk_wddm_device *device, hasvk_wddm_bo *commands, uint32_t offset, uint32_t bytes, uint64_t present_token,
                  bool wait, uint64_t *seq_out)
{
   if (!device || !commands || commands->device != device) return failure("Invalid submission device/BO");
   return checked([&] {
      uint64_t seq;
      {
         std::lock_guard<std::mutex> lock(device->mutex);
         if (commands->native->size() != 8192 || offset > 8192 || bytes > 8192 - offset || (offset & 7))
            throw std::runtime_error("Submission entry must fit the calibrated 8192-byte command BO");
         for (const auto *pool : device->pools) {
            for (size_t i = 0; i < pool->chunks.size(); ++i) {
               if (pool->chunks[i]) memcpy(pool->chunks[i]->cpu(), static_cast<const char *>(pool->cpu) + i * 65536, 65536);
            }
         }
         seq = device->native.submit(commands->native->address() + offset, bytes, command_header_8192(), present_token);
      }
      if (seq_out) *seq_out = seq;
      if (wait) wait_submission(device, seq, 15000);
   });
}
extern "C" int hasvk_wddm_submit_present(hasvk_wddm_device *device, hasvk_wddm_bo *commands,
                                          uint32_t offset, uint32_t bytes, uint64_t present_token)
{
   if (!present_token) return failure("Missing present token");
   return submit(device, commands, offset, bytes, present_token, true, nullptr);
}
extern "C" int hasvk_wddm_submit_wait(hasvk_wddm_device *device, hasvk_wddm_bo *commands,
                                       uint32_t offset, uint32_t bytes)
{
   return submit(device, commands, offset, bytes, 0, true, nullptr);
}
extern "C" int hasvk_wddm_submit_async(hasvk_wddm_device *device, hasvk_wddm_bo *commands,
                                        uint32_t offset, uint32_t bytes, uint64_t *seq)
{
   if (!seq) return failure("Missing sequence output");
   return submit(device, commands, offset, bytes, 0, false, seq);
}
extern "C" int hasvk_wddm_submit_present_async(hasvk_wddm_device *device, hasvk_wddm_bo *commands,
                                                uint32_t offset, uint32_t bytes, uint64_t present_token, uint64_t *seq)
{
   if (!present_token || !seq) return failure("Missing present token or sequence output");
   return submit(device, commands, offset, bytes, present_token, false, seq);
}
extern "C" int hasvk_wddm_wait_submission(hasvk_wddm_device *device, uint64_t seq)
{
   if (!device) return failure("Invalid device");
   return checked([&] { wait_submission(device, seq, 15000); });
}
extern "C" uint64_t hasvk_wddm_completed_submission(hasvk_wddm_device *device)
{
   if (!device) return 0;
   std::lock_guard<std::mutex> lock(device->mutex);
   return device->native.completed_seq();
}
extern "C" int hasvk_wddm_readonly_pool_create(hasvk_wddm_device *device, uint64_t address,
                                               uint32_t size, hasvk_wddm_readonly_pool **out)
{
   if (!device || !out) return failure("Missing state pool arguments");
   *out = nullptr;
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      if (!size || size > (1u << 30) || (size & 65535) || !address || (address & 65535))
         throw std::runtime_error("Invalid state pool size/address");
      auto pool = std::make_unique<hasvk_wddm_readonly_pool>();
      pool->device = device; pool->address = address; pool->bytes = size; pool->reservation = 0;
      for (const auto &entry : device->ranges) {
         if (address >= entry.first && address - entry.first <= entry.second->size() &&
             size <= entry.second->size() - (address - entry.first)) pool->reservation = entry.first;
      }
      if (!pool->reservation) throw std::runtime_error("State pool must belong to a reserved GPU VA range");
      for (const auto *other : device->pools)
         if (address < other->address + other->bytes && other->address < address + size)
            throw std::runtime_error("State pool GPU VA ranges overlap");
      for (const auto *bo : device->buffers)
         if (address < bo->native->address() + bo->native->size() && bo->native->address() < address + size)
            throw std::runtime_error("State pool overlaps an allocation");
      pool->chunks.resize(size / 65536);
      pool->cpu = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
      if (!pool->cpu) throw Error("Reserve state pool CPU VA", HRESULT_FROM_WIN32(GetLastError()));
      device->pools.insert(pool.get());
      *out = pool.release();
   });
}
extern "C" int hasvk_wddm_readonly_pool_map(hasvk_wddm_readonly_pool *pool,
                                            uint32_t offset, uint32_t bytes, void **out)
{
   if (!pool || !out) return failure("Missing pool map arguments");
   *out = nullptr;
   return checked([&] {
      std::lock_guard<std::mutex> lock(pool->device->mutex);
      if (!bytes || offset >= pool->bytes || bytes > pool->bytes - offset)
         throw std::runtime_error("State map exceeds pool");
      uint32_t first = offset / 65536, last = (offset + bytes - 1) / 65536;
      for (uint32_t i = first; i <= last; ++i) {
         if (pool->chunks[i]) continue;
         auto bo = std::make_unique<Buffer>(pool->device->native, linear_metadata(65536), 65536, 0,
                                           pool->address + uint64_t(i) * 65536);
         void *chunk = static_cast<char *>(pool->cpu) + i * 65536;
         if (!VirtualAlloc(chunk, 65536, MEM_COMMIT, PAGE_READWRITE))
            throw Error("Commit state pool CPU memory", HRESULT_FROM_WIN32(GetLastError()));
         pool->chunks[i] = std::move(bo);
      }
      *out = static_cast<char *>(pool->cpu) + offset;
   });
}
extern "C" int hasvk_wddm_readonly_pool_destroy(hasvk_wddm_readonly_pool *pool)
{
   if (!pool) return 0;
   return checked([&] {
      std::lock_guard<std::mutex> lock(pool->device->mutex);
      pool->device->pools.erase(pool);
      delete pool;
   });
}
