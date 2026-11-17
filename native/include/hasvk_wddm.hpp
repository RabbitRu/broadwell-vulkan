#pragma once
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace hasvk::wddm {
// Largest linear allocation of the experimental Intel profile. The size is an
// 8-byte field of the 1096-byte descriptor; verified on hardware up to this value.
constexpr uint64_t kMaxLinearAllocationBytes = 1ull << 30;
class Buffer;
constexpr uint64_t gpu_min = 0x800000000000ull;
constexpr uint64_t gpu_max = 0xfffffffff000ull;
constexpr uint64_t supported_driver = (20ull << 48) | (19ull << 32) | (15ull << 16) | 5171;

struct Error : std::runtime_error {
   LONG status;
   Error(const char *operation, LONG code);
};
void check(LONG status, const char *operation);
uint64_t canonical(uint64_t address);
std::vector<uint8_t> linear_metadata(uint64_t bytes);
std::vector<uint8_t> command_header_8192();

// Stage-2 transport. Private allocation/submit data is supplied explicitly by
// the caller; this does not claim to generate Intel's resource descriptions.
class Device {
public:
   Device();
   ~Device();
   Device(const Device &) = delete;
   Device &operator=(const Device &) = delete;
   void wait_paging(uint64_t fence, DWORD timeout_ms = 15000);
   void submit_and_wait(uint64_t commands, uint32_t bytes,
                        const std::vector<uint8_t> &header, DWORD timeout_ms = 15000);
   uint64_t reserve_va(uint64_t bytes, uint64_t requested_address = 0);
   void free_va(uint64_t address, uint64_t bytes);
   void shutdown();
   D3DKMT_HANDLE handle() const { return device_; }
   D3DKMT_HANDLE queue() const { return paging_.hPagingQueue; }
   bool lost() const { return lost_; }
   void abort_context() noexcept;
   uint64_t driver_version() const { return driver_version_; }
   uint32_t execution_state();
   LUID adapter_luid() const { return luid_; }
   std::wstring adapter_name;
   /* Profiling counters (QueryPerformanceCounter ticks, bytes). */
   std::atomic<uint64_t> flush_ticks{0}, kernel_ticks{0}, flushed_bytes{0};
private:
   friend class Buffer;
   friend class VaRange;
   void note_cleanup(LONG status, const char *operation) noexcept;
   void close() noexcept;
   D3DKMT_HANDLE adapter_ = 0, device_ = 0, context_ = 0;
   D3DKMT_CREATEPAGINGQUEUE paging_{};
   HANDLE completed_ = nullptr;
   uint64_t driver_version_ = 0;
   LUID luid_{};
   std::vector<Buffer *> buffers_;
   bool lost_ = false;
   bool cancellation_failed_ = false;
   unsigned cleanup_errors_ = 0;
};

class VaRange {
public:
   VaRange(Device &device, uint64_t bytes, uint64_t requested_address = 0);
   ~VaRange();
   VaRange(const VaRange &) = delete;
   VaRange &operator=(const VaRange &) = delete;
   uint64_t address() const { return address_; }
   uint64_t size() const { return bytes_; }
private:
   Device &device_;
   uint64_t address_, bytes_;
};

class Buffer {
public:
   Buffer(Device &device, const std::vector<uint8_t> &metadata, uint64_t bytes,
          uint64_t protection = 1, uint64_t requested_va = 0);
   ~Buffer();
   Buffer(const Buffer &) = delete;
   Buffer &operator=(const Buffer &) = delete;
   void *cpu() const { return cpu_; }
   uint64_t address() const { return address_; }
   uint64_t size() const { return bytes_; }
   /* Whether the CPU may have touched this buffer since its last flush. Only
    * such buffers are flushed around submissions (all of them by default). */
   /* Bit 0: flush before each submission (CPU writes the GPU must see).
    * Bit 1: flush after completion (GPU writes the CPU will read). */
   unsigned cpu_tracking() const { return cpu_tracking_; }
   void set_cpu_tracking(unsigned mode) { cpu_tracking_ = mode & 3; }
   void flush() const { flush(0, bytes_); }   // write back and invalidate the CPU cache lines
   void flush(uint64_t offset, uint64_t bytes) const;
private:
   void close() noexcept;
   Device &device_;
   unsigned cpu_tracking_ = 3;
   D3DKMT_HANDLE allocation_ = 0;
   void *cpu_ = nullptr;
   uint64_t address_ = 0, bytes_ = 0;
   bool owns_va_ = true;
};
} // namespace hasvk::wddm

