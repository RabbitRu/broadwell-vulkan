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
extern "C" int hasvk_wddm_bo_create(hasvk_wddm_device *device, uint64_t bytes,
                                     uint64_t requested, int writable, hasvk_wddm_bo **out)
{
   if (!device || !out) return failure("Missing allocation arguments");
   *out = nullptr;
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      if (!bytes || (bytes & 4095) || bytes > hasvk::wddm::kMaxLinearAllocationBytes ||
          (requested & 4095)) throw std::runtime_error("Invalid linear allocation size/address");
      auto bo = std::make_unique<hasvk_wddm_bo>();
      bo->device = device;
      bo->reservation = 0;
      if (requested) {
         for (const auto &entry : device->ranges) {
            if (requested >= entry.first && requested - entry.first <= entry.second->size() &&
                bytes <= entry.second->size() - (requested - entry.first)) bo->reservation = entry.first;
         }
         if (!bo->reservation) throw std::runtime_error("Exact mapping must belong to a reserved VA range");
         for (const auto *pool : device->pools)
            if (requested < pool->address + pool->bytes && pool->address < requested + bytes)
               throw std::runtime_error("Allocation overlaps a state pool");
      }
      bo->native = std::make_unique<Buffer>(device->native, linear_metadata(bytes), bytes, writable ? 1 : 0, requested);
      device->buffers.insert(bo.get());
      *out = bo.release();
   });
}
extern "C" int hasvk_wddm_bo_destroy(hasvk_wddm_bo *bo)
{
   if (!bo) return 0;
   return checked([&] {
      std::lock_guard<std::mutex> lock(bo->device->mutex);
      bo->device->buffers.erase(bo);
      delete bo;
   });
}
extern "C" void *hasvk_wddm_bo_map(hasvk_wddm_bo *bo) { return bo ? bo->native->cpu() : nullptr; }
extern "C" uint64_t hasvk_wddm_bo_address(const hasvk_wddm_bo *bo) { return bo ? bo->native->address() : 0; }
extern "C" uint64_t hasvk_wddm_bo_size(const hasvk_wddm_bo *bo) { return bo ? bo->native->size() : 0; }
extern "C" int hasvk_wddm_submit_wait(hasvk_wddm_device *device, hasvk_wddm_bo *commands,
                                       uint32_t offset, uint32_t bytes)
{
   if (!device || !commands || commands->device != device) return failure("Invalid submission device/BO");
   return checked([&] {
      std::lock_guard<std::mutex> lock(device->mutex);
      if (commands->native->size() != 8192 || offset > 8192 || bytes > 8192 - offset || (offset & 7))
         throw std::runtime_error("Submission entry must fit the calibrated 8192-byte command BO");
      for (const auto *pool : device->pools) {
         for (size_t i = 0; i < pool->chunks.size(); ++i) {
            if (pool->chunks[i]) memcpy(pool->chunks[i]->cpu(), static_cast<const char *>(pool->cpu) + i * 65536, 65536);
         }
      }
      device->native.submit_and_wait(commands->native->address() + offset, bytes, command_header_8192());
   });
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
