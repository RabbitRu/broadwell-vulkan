#include "hasvk_wddm.hpp"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <emmintrin.h>

namespace hasvk::wddm {
using Microsoft::WRL::ComPtr;
static_assert(sizeof(void *) == 8, "Only x64 is supported");
static_assert(sizeof(D3DDDI_ALLOCATIONINFO2) == 96, "Allocation ABI changed");
static_assert(offsetof(D3DKMT_SUBMITCOMMAND, pPrivateDriverData) == 288, "Submit ABI changed");
static_assert(offsetof(D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2, CpuEventHandle) == 400, "Signal ABI changed");
static_assert(sizeof(D3DDDI_MAPGPUVIRTUALADDRESS) == 104, "GPU map ABI changed");

static std::string message(const char *operation, LONG status)
{
   char code[32];
   snprintf(code, sizeof(code), " (0x%08lx)", static_cast<unsigned long>(status));
   return std::string(operation) + code;
}
Error::Error(const char *operation, LONG code) : std::runtime_error(message(operation, code)), status(code) {}
void check(LONG status, const char *operation) { if (status < 0) throw Error(operation, status); }
uint64_t canonical(uint64_t address)
{
   return address & (1ull << 47) ? address | 0xffff000000000000ull : address;
}

Device::Device() : Device(4 /* D3DKMT_CLIENTHINT_VULKAN */, {}) {}
Device::Device(uint32_t client_hint, std::vector<uint8_t> context_private)
{
   try {
      ComPtr<IDXGIFactory1> factory;
      check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
      LUID luid{};
      bool found = false;
      for (UINT i = 0;; ++i) {
         ComPtr<IDXGIAdapter1> adapter;
         HRESULT hr = factory->EnumAdapters1(i, &adapter);
         if (hr == DXGI_ERROR_NOT_FOUND) break;
         check(hr, "EnumAdapters1");
         DXGI_ADAPTER_DESC1 desc{};
         check(adapter->GetDesc1(&desc), "GetDesc1");
         if (desc.VendorId == 0x8086 && desc.DeviceId == 0x162b && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            luid = desc.AdapterLuid;
            adapter_name = desc.Description;
            found = true;
            break;
         }
      }
      if (!found) throw std::runtime_error("Hardware Iris 6100 (8086:162b) not found");
      luid_ = luid;
      D3DKMT_OPENADAPTERFROMLUID open{};
      open.AdapterLuid = luid;
      check(D3DKMTOpenAdapterFromLuid(&open), "OpenAdapterFromLuid");
      adapter_ = open.hAdapter;
      D3DKMT_QUERYADAPTERINFO query{};
      query.hAdapter = adapter_;
      query.Type = KMTQAITYPE_UMD_DRIVER_VERSION;
      query.pPrivateDriverData = &driver_version_;
      query.PrivateDriverDataSize = sizeof(driver_version_);
      check(D3DKMTQueryAdapterInfo(&query), "Query UMD driver version");
      if (driver_version_ != supported_driver)
         throw std::runtime_error("Intel metadata profile requires driver 20.19.15.5171");
      D3DKMT_CREATEDEVICE create{};
      create.hAdapter = adapter_;
      check(D3DKMTCreateDevice(&create), "CreateDevice");
      device_ = create.hDevice;
      paging_.hDevice = device_;
      check(D3DKMTCreatePagingQueue(&paging_), "CreatePagingQueue");
      D3DKMT_CREATECONTEXTVIRTUAL context{};
      context.hDevice = device_;
      context.ClientHint = static_cast<D3DKMT_CLIENTHINT>(client_hint);
      if (!context_private.empty()) {
         context.pPrivateDriverData = context_private.data();
         context.PrivateDriverDataSize = static_cast<UINT>(context_private.size());
      }
      check(D3DKMTCreateContextVirtual(&context), "CreateContextVirtual");
      context_ = context.hContext;
      completed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!completed_) throw Error("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
   } catch (...) { close(); throw; }
}

void Device::wait_paging(uint64_t fence, DWORD timeout_ms)
{
   const ULONGLONG start = GetTickCount64();
   auto *value = static_cast<volatile uint64_t *>(paging_.FenceValueCPUVirtualAddress);
   while (*value < fence) {
      if (GetTickCount64() - start >= timeout_ms) {
         abort_context();
         throw std::runtime_error("GPU paging fence timed out");
      }
      Sleep(1);
   }
}

void Device::submit_and_wait(uint64_t commands, uint32_t bytes,
                             const std::vector<uint8_t> &header, DWORD timeout_ms, uint64_t present_token)
{
   if (lost_) throw std::runtime_error("Device/context is lost");
   uint32_t signature = 0;
   if (header.size() == 80) memcpy(&signature, header.data(), 4);
   /* 0x00141433: header variant of the Intel OpenGL ICD (present experiments). */
   if ((signature != 0x00171433 && signature != 0x00141433) || !bytes || bytes > 8180 || (bytes & 7))
      throw std::runtime_error("Unsupported stage-2 submission/header");
   D3DKMT_SUBMITCOMMAND submit{};
   submit.Commands = commands;
   submit.CommandLength = bytes;
   submit.BroadcastContextCount = 1;
   submit.BroadcastContext[0] = context_;
   submit.pPrivateDriverData = const_cast<uint8_t *>(header.data());
   submit.PrivateDriverDataSize = static_cast<UINT>(header.size());
   if (present_token) { submit.Flags.PresentRedirected = 1; submit.PresentHistoryToken = present_token; }
   // From this point allocations must not be destroyed until completion or
   // context cancellation. Every error path first destroys the context.
   try {
      MemoryBarrier();
      // The private allocation profile does not establish a documented CPU
      // cache policy. Publish CPU writes explicitly instead of assuming KMT
      // Lock2 mappings and every GPU access path are mutually snooped.
      LARGE_INTEGER t1, t2;
      for (const Buffer *buffer : buffers_) if (buffer->cpu_tracking() & 1) buffer->flush();
      _mm_mfence();
      QueryPerformanceCounter(&t1);
      check(D3DKMTSubmitCommand(&submit), "SubmitCommand");
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
      signal.hContext = context_;
      signal.Flags.EnqueueCpuEvent = 1;
      signal.CpuEventHandle = completed_;
      check(D3DKMTSignalSynchronizationObject2(&signal), "Enqueue completion event");
      DWORD wait = WaitForSingleObject(completed_, timeout_ms);
      if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU completion event timed out or failed");
      MemoryBarrier();
      const uint32_t state = execution_state();
      if (state != D3DKMT_DEVICEEXECUTION_ACTIVE)
         throw std::runtime_error("WDDM device is not active after submission (execution state " + std::to_string(state) + ")");
      QueryPerformanceCounter(&t2);
      for (const Buffer *buffer : buffers_) if (buffer->cpu_tracking() & 2) buffer->flush();
      _mm_mfence();
      kernel_ticks += t2.QuadPart - t1.QuadPart;
   } catch (...) { abort_context(); throw; }
}
void Buffer::flush(uint64_t offset, uint64_t bytes) const
{
   if (offset > bytes_ || bytes > bytes_ - offset) throw std::runtime_error("Flush range outside the buffer");
   /* Every flush, implicit (around submissions) or explicit, is counted in
    * the device profile. */
   LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
   for (uint64_t line = offset & ~63ull; line < offset + bytes; line += 64) _mm_clflush(static_cast<const char *>(cpu_) + line);
   _mm_mfence();
   QueryPerformanceCounter(&t1);
   device_.flush_ticks += t1.QuadPart - t0.QuadPart; device_.flushed_bytes += bytes;
}
uint32_t Device::execution_state()
{
   D3DKMT_GETDEVICESTATE request{};
   request.hDevice = device_; request.StateType = D3DKMT_DEVICESTATE_EXECUTION;
   check(D3DKMTGetDeviceState(&request), "GetDeviceState execution");
   return static_cast<uint32_t>(request.ExecutionState);
}

uint64_t Device::reserve_va(uint64_t bytes, uint64_t requested_address)
{
   if (lost_ || !bytes || (bytes & 0xffff) || (requested_address & 0xffff) ||
       requested_address > gpu_max || bytes > gpu_max - requested_address)
      throw std::runtime_error("Invalid VA reservation");
   D3DDDI_RESERVEGPUVIRTUALADDRESS reserve{};
   reserve.hAdapter = adapter_;
   reserve.BaseAddress = requested_address;
   reserve.MinimumAddress = requested_address ? 0 : gpu_min;
   reserve.MaximumAddress = requested_address ? 0 : gpu_max;
   reserve.Size = bytes;
   check(D3DKMTReserveGpuVirtualAddress(&reserve), "ReserveGpuVirtualAddress");
   return reserve.VirtualAddress;
}
void Device::free_va(uint64_t address, uint64_t bytes)
{
   D3DKMT_FREEGPUVIRTUALADDRESS request{};
   request.hAdapter = adapter_;
   request.BaseAddress = address;
   request.Size = bytes;
   check(D3DKMTFreeGpuVirtualAddress(&request), "FreeGpuVirtualAddress");
}
VaRange::VaRange(Device &device, uint64_t bytes, uint64_t requested_address)
   : device_(device), address_(device.reserve_va(bytes, requested_address)), bytes_(bytes) {}
VaRange::~VaRange()
{
   if (device_.cancellation_failed_) return;
   try { device_.free_va(address_, bytes_); }
   catch (const Error &error) { device_.note_cleanup(error.status, "FreeGpuVirtualAddress range"); }
}
void Device::abort_context() noexcept
{
   lost_ = true;
   if (context_) {
      D3DKMT_DESTROYCONTEXT destroy{context_};
      LONG status = D3DKMTDestroyContext(&destroy);
      note_cleanup(status, "DestroyContext");
      if (status >= 0) context_ = 0;
      else cancellation_failed_ = true;
   }
}
void Device::close() noexcept
{
   abort_context();
   if (completed_) { CloseHandle(completed_); completed_ = nullptr; }
   if (paging_.hPagingQueue) {
      D3DDDI_DESTROYPAGINGQUEUE destroy{paging_.hPagingQueue};
      note_cleanup(D3DKMTDestroyPagingQueue(&destroy), "DestroyPagingQueue");
      paging_.hPagingQueue = 0;
   }
   if (device_) { D3DKMT_DESTROYDEVICE destroy{device_}; note_cleanup(D3DKMTDestroyDevice(&destroy), "DestroyDevice"); device_ = 0; }
   if (adapter_) { D3DKMT_CLOSEADAPTER close_adapter{adapter_}; note_cleanup(D3DKMTCloseAdapter(&close_adapter), "CloseAdapter"); adapter_ = 0; }
}
Device::~Device() { close(); }
void Device::note_cleanup(LONG status, const char *operation) noexcept
{
   if (status < 0) {
      ++cleanup_errors_;
      fprintf(stderr, "Cleanup %s: 0x%08lx\n", operation, static_cast<unsigned long>(status));
   }
}
void Device::shutdown()
{
   close();
   if (cleanup_errors_) throw std::runtime_error("WDDM resource cleanup failed; see stderr");
}

Buffer::Buffer(Device &device, const std::vector<uint8_t> &metadata, uint64_t bytes,
               uint64_t protection, uint64_t requested_va)
   : device_(device), bytes_(bytes), owns_va_(requested_va == 0)
{
   if (device.lost() || metadata.size() != 1096 || !bytes || bytes > kMaxLinearAllocationBytes || (bytes & 4095))
      throw std::runtime_error("Invalid Intel allocation metadata/size");
   uint64_t described_bytes;
   memcpy(&described_bytes, metadata.data() + 40, sizeof(described_bytes));
   if (!described_bytes || described_bytes > kMaxLinearAllocationBytes || ((described_bytes + 4095) & ~4095ull) != bytes)
      throw std::runtime_error("Intel descriptor size does not match allocation");
   try {
      auto mutable_metadata = metadata;
      D3DDDI_ALLOCATIONINFO2 info{};
      info.pPrivateDriverData = mutable_metadata.data();
      info.PrivateDriverDataSize = static_cast<UINT>(mutable_metadata.size());
      D3DKMT_CREATEALLOCATION create{};
      create.hDevice = device_.handle();
      create.NumAllocations = 1;
      create.pAllocationInfo2 = &info;
      check(D3DKMTCreateAllocation2(&create), "CreateAllocation2");
      allocation_ = info.hAllocation;
      D3DKMT_LOCK2 lock{};
      lock.hDevice = device_.handle(); lock.hAllocation = allocation_;
      check(D3DKMTLock2(&lock), "Lock2");
      cpu_ = lock.pData;
      D3DDDI_MAPGPUVIRTUALADDRESS map{};
      map.hPagingQueue = device_.queue();
      map.hAllocation = allocation_;
      map.MinimumAddress = requested_va ? 0 : gpu_min; map.MaximumAddress = requested_va ? 0 : gpu_max;
      map.BaseAddress = requested_va;
      map.SizeInPages = bytes / 4096;
      map.Protection.Value = protection;
      check(D3DKMTMapGpuVirtualAddress(&map), "MapGpuVirtualAddress");
      address_ = map.VirtualAddress;
      device_.wait_paging(map.PagingFenceValue);
      if (requested_va && address_ != requested_va) throw std::runtime_error("Exact GPU VA mapping was not honored");
      D3DDDI_MAKERESIDENT resident{};
      resident.hPagingQueue = device_.queue();
      resident.NumAllocations = 1; resident.AllocationList = &allocation_;
      check(D3DKMTMakeResident(&resident), "MakeResident");
      device_.wait_paging(resident.PagingFenceValue);
      device_.buffers_.push_back(this);
   } catch (...) { close(); throw; }
}
void Buffer::close() noexcept
{
   auto &buffers = device_.buffers_;
   buffers.erase(std::remove(buffers.begin(), buffers.end(), this), buffers.end());
   // If cancellation failed, leave allocations to device/process teardown.
   // They may still be referenced by GPU work and must not be freed here.
   if (device_.cancellation_failed_) return;
   if (cpu_) {
      D3DKMT_UNLOCK2 unlock{};
      unlock.hDevice = device_.handle(); unlock.hAllocation = allocation_;
      device_.note_cleanup(D3DKMTUnlock2(&unlock), "Unlock2"); cpu_ = nullptr;
   }
   if (address_ && owns_va_) {
      try { device_.free_va(address_, bytes_); }
      catch (const Error &error) { device_.note_cleanup(error.status, "FreeGpuVirtualAddress"); }
   }
   address_ = 0;
   if (allocation_) {
      D3DKMT_DESTROYALLOCATION2 destroy{};
      destroy.hDevice = device_.handle(); destroy.phAllocationList = &allocation_; destroy.AllocationCount = 1;
      device_.note_cleanup(D3DKMTDestroyAllocation2(&destroy), "DestroyAllocation2"); allocation_ = 0;
   }
}
Buffer::~Buffer() { close(); }
} // namespace hasvk::wddm
