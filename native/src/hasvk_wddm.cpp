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

Device::Device() : Device(4 /* D3DKMT_CLIENTHINT_VULKAN */, {}, true) {}
Device::Device(uint32_t client_hint, std::vector<uint8_t> context_private, bool presentable_context)
{
   try {
      if (presentable_context && context_private.empty()) {
         context_buffer_.assign(0x2cdc, 0);
         context_private.assign(32, 0);
         const uint64_t pointer = reinterpret_cast<uint64_t>(context_buffer_.data());
         const uint32_t bytes = static_cast<uint32_t>(context_buffer_.size()), flags = 0x100;
         memcpy(context_private.data(), &pointer, 8); memcpy(context_private.data() + 12, &bytes, 4); memcpy(context_private.data() + 20, &flags, 4);
      }
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

/* Paging operations (GPU VA mapping, residency) complete on the paging queue's
 * monitored fence. The kernel signals an event when it reaches the value:
 * polling with Sleep(1) cost a whole timer tick (~15.6 ms) per wait, twice per
 * allocation, and left CTS processes idle most of the time. */
void Device::wait_paging(uint64_t fence, DWORD timeout_ms)
{
   auto *value = static_cast<volatile uint64_t *>(paging_.FenceValueCPUVirtualAddress);
   if (*value >= fence) return;
   /* An event per wait: allocations of several threads may wait at once. */
   const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
   if (!event) throw Error("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
   struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close_event{event};
   D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
   wait.hDevice = device_;
   wait.ObjectCount = 1;
   wait.ObjectHandleArray = &paging_.hSyncObject;
   wait.FenceValueArray = &fence;
   wait.hAsyncEvent = event;
   check(D3DKMTWaitForSynchronizationObjectFromCpu(&wait), "Wait for paging fence");
   if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0 || *value < fence) {
      abort_context();
      throw std::runtime_error("GPU paging fence timed out");
   }
}

/* Creating an allocation queues GPU VA and residency updates on the paging
 * queue; while the GPU renders they execute only behind the work already
 * queued, so a CPU wait for them stalled vkAllocateMemory for 10-30 ms per
 * large allocation in games. The allocation's CPU mapping (Lock2) is usable
 * at once; only GPU access has to come after the paging fence, which submit()
 * guarantees with a GPU-side wait. */
void Device::after_paging(uint64_t fence)
{
   uint64_t pending = paging_pending_.load();
   while (pending < fence && !paging_pending_.compare_exchange_weak(pending, fence)) {}
}
void Device::submit_and_wait(uint64_t commands, uint32_t bytes,
                             const std::vector<uint8_t> &header, DWORD timeout_ms, uint64_t present_token)
{
   wait(submit(commands, bytes, header, present_token), timeout_ms);
}
HANDLE Device::pending_event(uint64_t seq, uint64_t *oldest) const
{
   if (seq <= completed_seq_ || pending_.empty()) return nullptr;
   if (oldest) *oldest = pending_.front().first;
   return pending_.front().second;
}
void Device::retire_oldest()
{
   if (pending_.empty()) return;
   try {
      MemoryBarrier();
      const uint32_t state = execution_state();
      if (state != D3DKMT_DEVICEEXECUTION_ACTIVE)
         throw std::runtime_error("WDDM device is not active after submission (execution state " + std::to_string(state) + ")");
      _mm_mfence();
   } catch (...) { abort_context(); throw; }
   completed_seq_ = pending_.front().first;
   ResetEvent(pending_.front().second);
   free_events_.push_back(pending_.front().second);
   pending_.pop_front();
}
void Device::wait(uint64_t seq, DWORD timeout_ms)
{
   while (HANDLE event = pending_event(seq)) {
      if (lost_) throw std::runtime_error("Device/context is lost");
      if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) {
         abort_context();
         throw std::runtime_error("GPU completion event timed out or failed");
      }
      retire_oldest();
   }
}
uint64_t Device::submit(uint64_t commands, uint32_t bytes, const std::vector<uint8_t> &header, uint64_t present_token)
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
   // context cancellation. Every error path first destroys the context,
   // except a redirected present the kernel refuses before queuing anything
   // (STATUS_GRAPHICS_PRESENT_MODE_CHANGED/OCCLUDED/DENIED, e.g. while the
   // window is hidden or the display sleeps): the context stays usable.
   LONG refused = 0;
   try {
      MemoryBarrier();
      /* GPU access to these allocations is coherent with the CPU caches
       * (shared LLC; native/tests/coherence_probe.cpp: CPU-written, CPU-read
       * and CPU-dirtied lines in L2/L3 and beyond, both directions): no CPU
       * cache write-back before submitting. */
      _mm_mfence();
      /* Allocations this submission may use: their paging operations first. */
      const uint64_t paging = paging_pending_.load();
      if (paging > paging_gpu_waited_.load() &&
          paging > *static_cast<volatile uint64_t *>(paging_.FenceValueCPUVirtualAddress)) {
         D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};
         wait.hContext = context_;
         wait.ObjectCount = 1;
         wait.ObjectHandleArray = &paging_.hSyncObject;
         wait.MonitoredFenceValueArray = &paging;
         check(D3DKMTWaitForSynchronizationObjectFromGpu(&wait), "GPU wait for paging fence");
      }
      if (paging > paging_gpu_waited_.load()) paging_gpu_waited_ = paging;
      const LONG submitted = D3DKMTSubmitCommand(&submit);
      if (submitted < 0 && present_token && (ULONG)submitted >= 0xC01E0005u && (ULONG)submitted <= 0xC01E0007u) {
         refused = submitted;
         throw Error("SubmitCommand (present refused)", submitted);
      }
      check(submitted, "SubmitCommand");
      /* Completion: a manual-reset CPU event per submission, set by the
       * kernel when the GPU reaches this point of the context. */
      HANDLE event;
      if (!free_events_.empty()) { event = free_events_.back(); free_events_.pop_back(); }
      else if (!(event = CreateEventW(nullptr, TRUE, FALSE, nullptr))) throw Error("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
      signal.hContext = context_;
      signal.Flags.EnqueueCpuEvent = 1;
      signal.CpuEventHandle = event;
      const LONG enqueued = D3DKMTSignalSynchronizationObject2(&signal);
      if (enqueued < 0) { free_events_.push_back(event); check(enqueued, "Enqueue completion event"); }
      pending_.emplace_back(++next_seq_, event);
   } catch (...) { if (!refused) abort_context(); throw; }
   return next_seq_;
}
void Buffer::flush(uint64_t offset, uint64_t bytes) const
{
   if (offset > bytes_ || bytes > bytes_ - offset) throw std::runtime_error("Flush range outside the buffer");
   /* GPU access is coherent with the CPU caches (see Device::submit): nothing
    * to write back or invalidate. */
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
   for (auto &entry : pending_) CloseHandle(entry.second);
   for (HANDLE event : free_events_) CloseHandle(event);
   pending_.clear(); free_events_.clear();
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
               uint64_t protection, uint64_t requested_va, bool cpu_map)
   : device_(device), bytes_(bytes), owns_va_(requested_va == 0)
{
   if (!cpu_map) cpu_tracking_ = 0;
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
      if (cpu_map) {
         D3DKMT_LOCK2 lock{};
         lock.hDevice = device_.handle(); lock.hAllocation = allocation_;
         check(D3DKMTLock2(&lock), "Lock2");
         cpu_ = lock.pData;
      }
      D3DDDI_MAPGPUVIRTUALADDRESS map{};
      map.hPagingQueue = device_.queue();
      map.hAllocation = allocation_;
      map.MinimumAddress = requested_va ? 0 : gpu_min; map.MaximumAddress = requested_va ? 0 : gpu_max;
      map.BaseAddress = requested_va;
      map.SizeInPages = bytes / 4096;
      map.Protection.Value = protection;
      check(D3DKMTMapGpuVirtualAddress(&map), "MapGpuVirtualAddress");
      address_ = map.VirtualAddress;
      device_.after_paging(map.PagingFenceValue);
      if (requested_va && address_ != requested_va) throw std::runtime_error("Exact GPU VA mapping was not honored");
      D3DDDI_MAKERESIDENT resident{};
      resident.hPagingQueue = device_.queue();
      resident.NumAllocations = 1; resident.AllocationList = &allocation_;
      check(D3DKMTMakeResident(&resident), "MakeResident");
      device_.after_paging(resident.PagingFenceValue);
      std::lock_guard<std::mutex> lock(device_.buffers_mutex_);
      device_.buffers_.push_back(this);
   } catch (...) { close(); throw; }
}
Buffer::Buffer(Device &device, WindowSurfaceTag, uint32_t width, uint32_t height)
   : device_(device), cpu_tracking_(0), owns_va_(true)
{
   if (device.lost() || !width || !height || width > 16384 || height > 16384) throw std::runtime_error("Invalid window surface size");
   try {
      auto metadata = window_surface_metadata(width, height), runtime = window_surface_runtime_data(width, height);
      D3DDDI_ALLOCATIONINFO2 info{}; info.pPrivateDriverData = metadata.data(); info.PrivateDriverDataSize = static_cast<UINT>(metadata.size());
      D3DKMT_CREATEALLOCATION create{};
      create.hDevice = device_.handle(); create.NumAllocations = 1; create.pAllocationInfo2 = &info;
      create.pPrivateRuntimeData = runtime.data(); create.PrivateRuntimeDataSize = static_cast<UINT>(runtime.size());
      create.Flags.CreateResource = 1; create.Flags.CreateShared = 1;
      check(D3DKMTCreateAllocation2(&create), "CreateAllocation2 (window surface)");
      resource_ = create.hResource; share_ = create.hGlobalShare;
      /* Open it again, as a consumer would: the kernel driver's completed
       * description (pitch at 0xc0, size at 0xd0) comes back with it. */
      D3DKMT_QUERYRESOURCEINFO query{}; query.hDevice = device_.handle(); query.hGlobalShare = share_;
      check(D3DKMTQueryResourceInfo(&query), "QueryResourceInfo (window surface)");
      std::vector<uint8_t> opened_runtime(query.PrivateRuntimeDataSize), opened(query.TotalPrivateDriverDataSize);
      if (opened.size() < 0xd8) throw std::runtime_error("Unexpected window surface description");
      D3DDDI_OPENALLOCATIONINFO2 open_info{};
      D3DKMT_OPENRESOURCE open{}; open.hDevice = device_.handle(); open.hGlobalShare = share_; open.NumAllocations = 1;
      open.pOpenAllocationInfo2 = &open_info;
      open.pPrivateRuntimeData = opened_runtime.data(); open.PrivateRuntimeDataSize = static_cast<UINT>(opened_runtime.size());
      open.pTotalPrivateDriverDataBuffer = opened.data(); open.TotalPrivateDriverDataBufferSize = static_cast<UINT>(opened.size());
      check(D3DKMTOpenResource2(&open), "OpenResource2 (window surface)");
      opened_resource_ = open.hResource; allocation_ = open_info.hAllocation;
      memcpy(&pitch_, opened.data() + 0xc0, 8); memcpy(&bytes_, opened.data() + 0xd0, 8);
      if (pitch_ != ((width * 4ull + 127) & ~127ull) || bytes_ != pitch_ * ((height + 31) & ~31u))
         throw std::runtime_error("Window surface layout is not the expected Y-tiled one");
      D3DDDI_MAPGPUVIRTUALADDRESS map{};
      map.hPagingQueue = device_.queue(); map.hAllocation = allocation_;
      map.MinimumAddress = gpu_min; map.MaximumAddress = gpu_max; map.SizeInPages = bytes_ / 4096; map.Protection.Value = 1;
      check(D3DKMTMapGpuVirtualAddress(&map), "MapGpuVirtualAddress (window surface)");
      address_ = map.VirtualAddress;
      device_.wait_paging(map.PagingFenceValue);
      D3DDDI_MAKERESIDENT resident{};
      resident.hPagingQueue = device_.queue(); resident.NumAllocations = 1; resident.AllocationList = &allocation_;
      check(D3DKMTMakeResident(&resident), "MakeResident (window surface)");
      device_.wait_paging(resident.PagingFenceValue);
      std::lock_guard<std::mutex> lock(device_.buffers_mutex_);
      device_.buffers_.push_back(this);
   } catch (...) { close(); throw; }
}
void Buffer::close() noexcept
{
   {
      std::lock_guard<std::mutex> lock(device_.buffers_mutex_);
      auto &buffers = device_.buffers_;
      buffers.erase(std::remove(buffers.begin(), buffers.end(), this), buffers.end());
   }
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
   if (opened_resource_ || resource_) {   // window surface: the opened and the created resource
      for (D3DKMT_HANDLE *resource : {&opened_resource_, &resource_}) {
         if (!*resource) continue;
         D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.hDevice = device_.handle(); destroy.hResource = *resource;
         device_.note_cleanup(D3DKMTDestroyAllocation2(&destroy), "DestroyAllocation2 (window surface)"); *resource = 0;
      }
      allocation_ = 0;
   }
   if (allocation_) {
      D3DKMT_DESTROYALLOCATION2 destroy{};
      destroy.hDevice = device_.handle(); destroy.phAllocationList = &allocation_; destroy.AllocationCount = 1;
      device_.note_cleanup(D3DKMTDestroyAllocation2(&destroy), "DestroyAllocation2"); allocation_ = 0;
   }
}
Buffer::~Buffer() { close(); }
} // namespace hasvk::wddm
