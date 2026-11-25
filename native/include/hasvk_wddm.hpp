#pragma once
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
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
std::vector<uint8_t> window_surface_metadata(uint32_t width, uint32_t height);
std::vector<uint8_t> window_surface_runtime_data(uint32_t width, uint32_t height);
std::vector<uint8_t> primary_metadata(uint32_t width, uint32_t height);
struct WindowSurfaceTag {};
struct PrimaryTag {};

// Stage-2 transport. Private allocation/submit data is supplied explicitly by
// the caller; this does not claim to generate Intel's resource descriptions.
class Device {
public:
   Device();
   /* presentable_context: create the context with the private data the Intel
    * OpenGL ICD passes (pointer to a 0x2cdc-byte buffer, its size, 0x100).
    * Without it the kernel accepts PresentRedirected submissions but DWM never
    * shows them. Other arguments are for experiments. */
   Device(uint32_t client_hint, std::vector<uint8_t> context_private, bool presentable_context);
   ~Device();
   Device(const Device &) = delete;
   Device &operator=(const Device &) = delete;
   void wait_paging(uint64_t fence, DWORD timeout_ms = 15000);
   /* Paging operations (GPU VA mapping, residency) the next submission must
    * wait for: submit() makes the GPU context wait for the paging fence instead
    * of blocking the CPU at allocation time (as the WDDM 2 D3D runtimes do). */
   void after_paging(uint64_t fence);
   /* present_token != 0: the submission is a redirected present
    * (D3DKMT_SUBMITCOMMANDFLAGS::PresentRedirected) with this present history
    * token, (0x80000000 | HWND) << 32 | shared surface handle, as the Intel
    * OpenGL ICD submits it. */
   void submit_and_wait(uint64_t commands, uint32_t bytes,
                        const std::vector<uint8_t> &header, DWORD timeout_ms = 15000,
                        uint64_t present_token = 0);
   /* Asynchronous form: submit returns the submission's sequence number
    * (increasing from 1); wait(seq) waits for it and every earlier one, then
    * flushes FLUSH_AFTER buffers. pending_event/retire_oldest let a caller
    * wait without holding its own lock (the C API does that). */
   uint64_t submit(uint64_t commands, uint32_t bytes, const std::vector<uint8_t> &header, uint64_t present_token = 0);
   /* Presentation without the compositor: exclusive ownership of VidPn source
    * 0 and flips between primaries (Buffer with PrimaryTag), as DXGI's
    * full-screen state does it. acquire_output takes the output and sets the
    * mode to primary; flip shows primary (interval 0: at once, 1..4: at that
    * vertical blank) behind everything submitted so far. Both return the
    * kernel's status; a failed flip means the output is no longer ours.
    * written_primaries names the primaries the next submission renders into:
    * the kernel holds it back while one of them is still on the screen. */
   LONG acquire_output(D3DKMT_HANDLE primary);
   void release_output() noexcept;
   bool owns_output() const { return owns_output_; }
   LONG flip(D3DKMT_HANDLE primary, HWND window, uint32_t width, uint32_t height, uint32_t interval);
   void set_written_primaries(const D3DKMT_HANDLE *primaries, unsigned count);
   void wait(uint64_t seq, DWORD timeout_ms = 15000);
   /* Oldest pending submission's event and number if seq is not complete, else null. */
   HANDLE pending_event(uint64_t seq, uint64_t *oldest = nullptr) const;
   void retire_oldest();                       // after its event was set
   uint64_t completed_seq() const { return completed_seq_; }
   uint64_t submitted_seq() const { return next_seq_; }
   uint64_t reserve_va(uint64_t bytes, uint64_t requested_address = 0);
   void free_va(uint64_t address, uint64_t bytes);
   void shutdown();
   D3DKMT_HANDLE handle() const { return device_; }
   D3DKMT_HANDLE context() const { return context_; }
   D3DKMT_HANDLE queue() const { return paging_.hPagingQueue; }
   bool lost() const { return lost_; }
   void abort_context() noexcept;
   uint64_t driver_version() const { return driver_version_; }
   uint32_t execution_state();
   LUID adapter_luid() const { return luid_; }
   std::wstring adapter_name;
private:
   friend class Buffer;
   friend class VaRange;
   void note_cleanup(LONG status, const char *operation) noexcept;
   void close() noexcept;
   D3DKMT_HANDLE adapter_ = 0, device_ = 0, context_ = 0;
   D3DKMT_CREATEPAGINGQUEUE paging_{};
   HANDLE completed_ = nullptr;
   /* Highest paging fence value submissions must wait for, and the highest
    * one the context already waits for (see after_paging). */
   std::atomic<uint64_t> paging_pending_{0}, paging_gpu_waited_{0};
   uint64_t driver_version_ = 0;
   LUID luid_{};
   /* Buffers are created and destroyed concurrently with submissions (the C
    * API holds no lock around their kernel calls): buffers_ has its own lock. */
   std::mutex buffers_mutex_;
   std::vector<Buffer *> buffers_;
   std::vector<uint8_t> context_buffer_;   // referenced by the context's private data
   std::deque<std::pair<uint64_t, HANDLE>> pending_;   // submissions not yet retired, oldest first
   std::vector<HANDLE> free_events_;
   uint64_t next_seq_ = 0, completed_seq_ = 0;
   bool owns_output_ = false;
   uint32_t present_count_ = 0;
   D3DKMT_HANDLE written_primaries_[4]{};
   unsigned written_primary_count_ = 0;
   bool lost_ = false;
   bool cancellation_failed_ = false;
   std::atomic<unsigned> cleanup_errors_{0};
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
   /* cpu_map = false: no Lock2 mapping (GPU-only memory, cpu() is null, never flushed). */
   Buffer(Device &device, const std::vector<uint8_t> &metadata, uint64_t bytes,
          uint64_t protection = 1, uint64_t requested_va = 0, bool cpu_map = true);
   /* Shared window surface for redirected present (not CPU-mapped). share()
    * is its global handle, pitch() the row pitch the kernel driver chose. */
   Buffer(Device &device, WindowSurfaceTag, uint32_t width, uint32_t height);
   /* Primary of VidPn source 0 (not CPU-mapped, X-tiled): pitch() is the row
    * pitch the kernel driver chose, allocation() the handle flips name. */
   Buffer(Device &device, PrimaryTag, uint32_t width, uint32_t height);
   bool primary() const { return primary_; }
   D3DKMT_HANDLE allocation() const { return allocation_; }
   uint32_t share() const { return share_; }
   uint64_t pitch() const { return pitch_; }
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
   D3DKMT_HANDLE resource_ = 0, opened_resource_ = 0;   // window surfaces only
   uint32_t share_ = 0;
   uint64_t pitch_ = 0;
   bool primary_ = false;
   void *cpu_ = nullptr;
   uint64_t address_ = 0, bytes_ = 0;
   bool owns_va_ = true;
};
} // namespace hasvk::wddm

