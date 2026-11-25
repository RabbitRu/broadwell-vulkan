/* Research tool: how the Intel OpenGL ICD (ig8icd64.dll) of the same driver
 * package presents frames without the D3D runtime.
 *
 * Every win32u!NtGdiDdDDI* syscall stub of this process is redirected to a
 * logger (the same technique as gpu-probe/src/KernelTrace.cs), then a small
 * OpenGL window clears and swaps a few frames and closes itself. Calls that
 * matter for presentation are decoded with their pointed-to buffers; all other
 * calls are logged by name and status. Only this process is affected.
 *
 * Usage: GlPresentTrace.exe <log file> [frames] */
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <GL/gl.h>
#include <psapi.h>
#include <dwmapi.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
FILE *out;
std::mutex gate;
std::atomic<const char *> phase{"startup"};
struct Hook { char name[96]; void *original; };
Hook hooks[1024];
unsigned hook_count;
typedef NTSTATUS(NTAPI *Syscall)(void *);

bool safe_copy(void *dst, const void *src, size_t bytes)
{
   __try { memcpy(dst, src, bytes); return true; }
   __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
/* Hex dump; runs of all-zero 16-byte lines are collapsed. Caller holds gate. */
void dump(const char *label, const void *data, size_t bytes, size_t cap = 8192)
{
   fprintf(out, "    %s @%p, %zu bytes\n", label, data, bytes);
   if (!data || !bytes) return;
   std::vector<uint8_t> copy(bytes < cap ? bytes : cap);
   if (!safe_copy(copy.data(), data, copy.size())) { fprintf(out, "      <unreadable>\n"); return; }
   bool skipping = false;
   for (size_t line = 0; line < copy.size(); line += 16) {
      const size_t n = copy.size() - line < 16 ? copy.size() - line : 16;
      bool zero = true; for (size_t i = 0; i < n; ++i) zero &= copy[line + i] == 0;
      if (zero && line && line + 16 < copy.size()) { if (!skipping) fprintf(out, "      *\n"); skipping = true; continue; }
      skipping = false;
      fprintf(out, "      %04zx:", line);
      for (size_t i = 0; i < n; ++i) fprintf(out, " %02x", copy[line + i]);
      fprintf(out, "\n");
   }
   if (copy.size() < bytes) fprintf(out, "      ... truncated at %zu\n", copy.size());
}
template <class T> bool read(T &value, const void *p) { return safe_copy(&value, p, sizeof(T)); }
/* GPU VA -> CPU pointer of the ICD's own allocations, to dump submitted batches. */
struct Mapping { D3DKMT_HANDLE allocation; uint64_t va, bytes; uint8_t *cpu; };
std::vector<Mapping> mappings;
const uint8_t *cpu_of(uint64_t va, uint64_t bytes)
{
   for (const Mapping &m : mappings)
      if (m.cpu && va >= m.va && va + bytes <= m.va + m.bytes) return m.cpu + (va - m.va);
   return nullptr;
}

void decode_pre(const char *name, void *data)
{
   if (!strcmp(name, "Present")) {
      D3DKMT_PRESENT p; if (!read(p, data)) return;
      fprintf(out, "    hContext=%#x hWindow=%p VidPnSource=%u hSource=%#x hDestination=%#x Color=%#x\n",
         p.hContext, (void *)p.hWindow, p.VidPnSourceId, p.hSource, p.hDestination, p.Color);
      fprintf(out, "    Dst=(%ld,%ld)-(%ld,%ld) Src=(%ld,%ld)-(%ld,%ld) SubRects=%u PresentCount=%u FlipInterval=%d Flags=%#x\n",
         p.DstRect.left, p.DstRect.top, p.DstRect.right, p.DstRect.bottom, p.SrcRect.left, p.SrcRect.top, p.SrcRect.right,
         p.SrcRect.bottom, p.SubRectCnt, p.PresentCount, (int)p.FlipInterval, p.Flags.Value);
      fprintf(out, "    Broadcast=%lu LimitSemaphore=%p Token.Model=%d Token.Size=%u CompositionBindingId=%#llx hAdapter=%#x Duration=%u PrivateDriverDataSize=%u\n",
         p.BroadcastContextCount, p.PresentLimitSemaphore, (int)p.PresentHistoryToken.Model, p.PresentHistoryToken.TokenSize,
         (unsigned long long)p.PresentHistoryToken.CompositionBindingId, p.hAdapter, p.Duration, p.PrivateDriverDataSize);
      for (ULONG i = 0; i < p.BroadcastContextCount && i < D3DDDI_MAX_BROADCAST_CONTEXT; ++i) fprintf(out, "    BroadcastContext[%lu]=%#x\n", i, p.BroadcastContext[i]);
      if (p.SubRectCnt) dump("SrcSubRects", p.pSrcSubRects, p.SubRectCnt * sizeof(RECT));
      if (p.PresentHistoryToken.Model) dump("PresentHistoryToken", (char *)data + offsetof(D3DKMT_PRESENT, PresentHistoryToken),
         sizeof(UINT) * 2 + sizeof(UINT64) + (p.PresentHistoryToken.TokenSize ? p.PresentHistoryToken.TokenSize : 64));
      if (p.pPresentRegions) dump("PresentRegions", p.pPresentRegions, sizeof(D3DKMT_PRESENT_RGNS));
      dump("PrivateDriverData", p.pPrivateDriverData, p.PrivateDriverDataSize);
      dump("D3DKMT_PRESENT", data, sizeof(D3DKMT_PRESENT), 512);
   } else if (!strcmp(name, "CreateAllocation")) {
      D3DKMT_CREATEALLOCATION a; if (!read(a, data)) return;
      uint32_t flags; memcpy(&flags, &a.Flags, sizeof(flags));
      fprintf(out, "    hDevice=%#x hResource=%#x Flags=%#x NumAllocations=%u RuntimeData=%u DriverData=%u\n",
         a.hDevice, a.hResource, flags, a.NumAllocations, a.PrivateRuntimeDataSize, a.PrivateDriverDataSize);
      dump("PrivateRuntimeData", a.pPrivateRuntimeData, a.PrivateRuntimeDataSize);
      if (a.Flags.StandardAllocation) dump("StandardAllocation", a.pStandardAllocation, sizeof(D3DKMT_CREATESTANDARDALLOCATION));
      else dump("ResourcePrivateDriverData", a.pPrivateDriverData, a.PrivateDriverDataSize);
      for (UINT i = 0; i < a.NumAllocations && i < 16; ++i) {
         D3DDDI_ALLOCATIONINFO2 info; if (!read(info, a.pAllocationInfo2 + i)) break;
         fprintf(out, "    allocation[%u]: SystemMem=%p VidPnSource=%u Flags=%#x Priority=%u DriverData=%u\n", i,
            info.pSystemMem, info.VidPnSourceId, info.Flags.Value, info.Priority, info.PrivateDriverDataSize);
         dump("AllocationPrivateDriverData", info.pPrivateDriverData, info.PrivateDriverDataSize);
      }
   } else if (!strcmp(name, "Escape")) {
      D3DKMT_ESCAPE e; if (!read(e, data)) return;
      fprintf(out, "    hAdapter=%#x hDevice=%#x Type=%d Flags=%#x hContext=%#x\n", e.hAdapter, e.hDevice, (int)e.Type, e.Flags.Value, e.hContext);
      dump("EscapeData(in)", e.pPrivateDriverData, e.PrivateDriverDataSize, 512);
   } else if (!strcmp(name, "GetSharedPrimaryHandle")) {
      D3DKMT_GETSHAREDPRIMARYHANDLE g; if (!read(g, data)) return;
      fprintf(out, "    hAdapter=%#x VidPnSource=%u\n", g.hAdapter, g.VidPnSourceId);
   } else if (!strcmp(name, "QueryResourceInfo") || !strcmp(name, "OpenResource")) {
      dump("args(in)", data, 96, 96);
   } else if (!strcmp(name, "SubmitCommand")) {
      D3DKMT_SUBMITCOMMAND s; if (!read(s, data)) return;
      struct { UINT Value; } flags; memcpy(&flags.Value, &s.Flags, 4);
      fprintf(out, "    Commands=%#llx Length=%#x Flags=%#x Token=%#llx Broadcast=%u ctx0=%#x PrivateDriverDataSize=%u WrittenPrimaries=%u\n",
         (unsigned long long)s.Commands, s.CommandLength, flags.Value, (unsigned long long)s.PresentHistoryToken, s.BroadcastContextCount,
         s.BroadcastContext[0], s.PrivateDriverDataSize, s.NumPrimaries);
      dump("SubmitPrivateDriverData", s.pPrivateDriverData, s.PrivateDriverDataSize, 1024);
      if (s.Flags.PresentRedirected) {
         const uint8_t *batch = cpu_of(s.Commands, s.CommandLength);
         if (!batch) fprintf(out, "    batch: no CPU mapping known for %#llx\n", (unsigned long long)s.Commands);
         else {
            fprintf(out, "    batch dwords:\n");
            std::vector<uint32_t> dw(s.CommandLength / 4);
            if (safe_copy(dw.data(), batch, dw.size() * 4))
               for (size_t i = 0; i < dw.size(); i += 8) {
                  fprintf(out, "      %04zx:", i * 4);
                  for (size_t j = i; j < i + 8 && j < dw.size(); ++j) fprintf(out, " %08x", dw[j]);
                  fprintf(out, "\n");
               }
         }
      }
      if (s.NumPrimaries) dump("WrittenPrimaries", s.WrittenPrimaries, s.NumPrimaries * sizeof(D3DKMT_HANDLE));
   } else if (!strcmp(name, "CreateContextVirtual")) {
      D3DKMT_CREATECONTEXTVIRTUAL c; if (!read(c, data)) return;
      fprintf(out, "    hDevice=%#x Node=%u Affinity=%#x Flags=%#x ClientHint=%d\n", c.hDevice, c.NodeOrdinal, c.EngineAffinity, c.Flags.Value, (int)c.ClientHint);
      dump("ContextPrivateDriverData", c.pPrivateDriverData, c.PrivateDriverDataSize, 512);
   } else if (!strcmp(name, "SetVidPnSourceOwner")) {
      D3DKMT_SETVIDPNSOURCEOWNER o; if (!read(o, data)) return;
      fprintf(out, "    hDevice=%#x VidPnSourceCount=%u\n", o.hDevice, o.VidPnSourceCount);
      for (UINT i = 0; i < o.VidPnSourceCount && i < 4; ++i) {
         D3DKMT_VIDPNSOURCEOWNER_TYPE type{}; D3DDDI_VIDEO_PRESENT_SOURCE_ID id = ~0u;
         read(type, o.pType + i); read(id, o.pVidPnSourceId + i);
         fprintf(out, "    owner[%u]: type=%d (0 none, 1 shared, 2 exclusive, 3 exclusive GDI, 4 emulated) VidPnSource=%u\n", i, (int)type, id);
      }
   } else if (!strcmp(name, "SetDisplayMode")) {
      D3DKMT_SETDISPLAYMODE m; if (!read(m, data)) return;
      uint32_t mode_flags; memcpy(&mode_flags, &m.Flags, sizeof(mode_flags));
      fprintf(out, "    hDevice=%#x hPrimaryAllocation=%#x ScanLineOrdering=%d DisplayOrientation=%d PrivateDriverFormatAttribute=%#x Flags=%#x\n",
         m.hDevice, m.hPrimaryAllocation, (int)m.ScanLineOrdering, (int)m.DisplayOrientation, m.PrivateDriverFormatAttribute, mode_flags);
   } else if (!strcmp(name, "GetDisplayModeList")) {
      D3DKMT_GETDISPLAYMODELIST g; if (!read(g, data)) return;
      fprintf(out, "    hAdapter=%#x VidPnSource=%u pModeList=%p ModeCount=%u\n", g.hAdapter, g.VidPnSourceId, (void *)g.pModeList, g.ModeCount);
   } else if (!strcmp(name, "CheckExclusiveOwnership") || !strcmp(name, "WaitForVerticalBlankEvent") || !strcmp(name, "GetPresentHistory") ||
              !strcmp(name, "CheckVidPnExclusiveOwnership") || !strcmp(name, "GetScanLine") || !strcmp(name, "DestroyAllocation2")) {
      dump("args(in)", data, 48, 48);
   } else if (!strcmp(name, "CreateContext") ||
              !strcmp(name, "Lock2") || !strcmp(name, "SignalSynchronizationObject2") || !strcmp(name, "WaitForSynchronizationObject2") ||
              !strcmp(name, "CreateSynchronizationObject2") || !strcmp(name, "OpenSynchronizationObject")) {
      dump("args(in)", data, 96, 96);
   }
}
void decode_post(const char *name, void *data)
{
   if (!strcmp(name, "MapGpuVirtualAddress")) {
      D3DDDI_MAPGPUVIRTUALADDRESS m; if (!read(m, data)) return;
      bool found = false;
      for (Mapping &x : mappings) if (x.allocation == m.hAllocation) { x.va = m.VirtualAddress; x.bytes = m.SizeInPages * 4096; found = true; }
      if (!found) mappings.push_back({m.hAllocation, m.VirtualAddress, m.SizeInPages * 4096, nullptr});
      fprintf(out, "    -> hAllocation=%#x VA=%#llx pages=%llu\n", m.hAllocation, (unsigned long long)m.VirtualAddress, (unsigned long long)m.SizeInPages);
      return;
   }
   if (!strcmp(name, "Lock2")) {
      D3DKMT_LOCK2 l; if (!read(l, data)) return;
      bool found = false;
      for (Mapping &x : mappings) if (x.allocation == l.hAllocation) { x.cpu = static_cast<uint8_t *>(l.pData); found = true; }
      if (!found) mappings.push_back({l.hAllocation, 0, 0, static_cast<uint8_t *>(l.pData)});
   }
   if (!strcmp(name, "CreateAllocation")) {
      D3DKMT_CREATEALLOCATION a; if (!read(a, data)) return;
      fprintf(out, "    -> hResource=%#x hGlobalShare=%#x", a.hResource, a.hGlobalShare);
      for (UINT i = 0; i < a.NumAllocations && i < 16; ++i) {
         D3DDDI_ALLOCATIONINFO2 info; if (!read(info, a.pAllocationInfo2 + i)) break;
         fprintf(out, " hAllocation[%u]=%#x", i, info.hAllocation);
      }
      fprintf(out, "\n");
      /* The kernel driver completes the description in place (pitch, size, tiling). */
      for (UINT i = 0; i < a.NumAllocations && i < 16; ++i) {
         D3DDDI_ALLOCATIONINFO2 info; if (!read(info, a.pAllocationInfo2 + i)) break;
         if (info.Flags.Primary) dump("AllocationPrivateDriverData(out)", info.pPrivateDriverData, info.PrivateDriverDataSize);
      }
   } else if (!strcmp(name, "GetSharedPrimaryHandle")) {
      D3DKMT_GETSHAREDPRIMARYHANDLE g; if (read(g, data)) fprintf(out, "    -> hSharedPrimary=%#x\n", g.hSharedPrimary);
   } else if (!strcmp(name, "QueryResourceInfo")) {
      D3DKMT_QUERYRESOURCEINFO q; if (!read(q, data)) return;
      fprintf(out, "    -> hGlobalShare=%#x Runtime=%u TotalDriver=%u ResourceDriver=%u NumAllocations=%u\n", q.hGlobalShare,
         q.PrivateRuntimeDataSize, q.TotalPrivateDriverDataSize, q.ResourcePrivateDriverDataSize, q.NumAllocations);
      dump("RuntimeData", q.pPrivateRuntimeData, q.pPrivateRuntimeData ? q.PrivateRuntimeDataSize : 0);
   } else if (!strcmp(name, "OpenResource")) {
      D3DKMT_OPENRESOURCE o; if (!read(o, data)) return;
      fprintf(out, "    -> hGlobalShare=%#x hResource=%#x NumAllocations=%u\n", o.hGlobalShare, o.hResource, o.NumAllocations);
      dump("RuntimeData", o.pPrivateRuntimeData, o.PrivateRuntimeDataSize);
      dump("ResourcePrivateDriverData", o.pResourcePrivateDriverData, o.ResourcePrivateDriverDataSize);
      dump("TotalPrivateDriverData", o.pTotalPrivateDriverDataBuffer, o.TotalPrivateDriverDataBufferSize);
      for (UINT i = 0; i < o.NumAllocations && i < 16; ++i) {
         D3DDDI_OPENALLOCATIONINFO2 info; if (!read(info, o.pOpenAllocationInfo2 + i)) break;
         fprintf(out, "    allocation[%u]: hAllocation=%#x GpuVA=%#llx DriverData=%u\n", i, info.hAllocation,
            (unsigned long long)info.GpuVirtualAddress, info.PrivateDriverDataSize);
         dump("AllocationPrivateDriverData", info.pPrivateDriverData, info.PrivateDriverDataSize);
      }
   } else if (!strcmp(name, "Escape")) {
      D3DKMT_ESCAPE e; if (read(e, data)) dump("EscapeData(out)", e.pPrivateDriverData, e.PrivateDriverDataSize, 512);
   } else if (!strcmp(name, "GetDisplayModeList")) {
      D3DKMT_GETDISPLAYMODELIST g; if (!read(g, data)) return;
      fprintf(out, "    -> ModeCount=%u\n", g.ModeCount);
      for (UINT i = 0; g.pModeList && i < g.ModeCount; ++i) {
         D3DKMT_DISPLAYMODE m; if (!read(m, g.pModeList + i)) break;
         uint32_t mode_flags; memcpy(&mode_flags, &m.Flags, sizeof(mode_flags));
         if (m.Width == 2560 || i < 2)
            fprintf(out, "    mode[%u]: %ux%u format %d refresh %u/%u scanline %d orientation %d fixed output %u flags %#x\n", i, m.Width, m.Height, (int)m.Format,
               m.RefreshRate.Numerator, m.RefreshRate.Denominator, (int)m.ScanLineOrdering, (int)m.DisplayOrientation, m.DisplayFixedOutput, mode_flags);
      }
   } else if (!strcmp(name, "GetDeviceState")) {
      D3DKMT_GETDEVICESTATE s; if (read(s, data)) dump("device state", data, 40, 40);
   } else if (!strcmp(name, "Present")) {
      D3DKMT_PRESENT p; if (read(p, data)) fprintf(out, "    -> bOptimizeForComposition=%u\n", p.bOptimizeForComposition);
   } else if (!strcmp(name, "CreateContextVirtual") || !strcmp(name, "CreateContext") || !strcmp(name, "CreateSynchronizationObject2") ||
              !strcmp(name, "Lock2") || !strcmp(name, "OpenSynchronizationObject")) {
      dump("args(out)", data, 96, 96);
   }
}
/* Frequent calls that carry nothing about presentation. */
bool quiet(const char *name)
{
   static const char *names[] = {"QueryAdapterInfo", "GetDeviceState", "WaitForSynchronizationObjectFromCpu", "MakeResident",
                                 "Evict", "SetAllocationPriority", "QueryAllocationResidency", "UpdateGpuVirtualAddress",
                                 "QueryStatistics", "CheckMultiPlaneOverlaySupport"};
   for (const char *n : names) if (!strcmp(name, n)) return true;
   return false;
}
/* Caller chain as module!offset, skipping this tool and the gdi32/win32u wrappers. */
void log_callers()
{
   void *frames[48]; const USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
   fprintf(out, "    callers:");
   unsigned shown = 0;
   for (USHORT i = 0; i < n && shown < 24; ++i) {
      HMODULE m{}; char path[MAX_PATH] = "?";
      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m)) continue;
      if (m == GetModuleHandleW(nullptr)) continue;
      GetModuleFileNameA(m, path, MAX_PATH); const char *file = strrchr(path, '\\'); file = file ? file + 1 : path;
      if (!_stricmp(file, "win32u.dll") || !_stricmp(file, "gdi32.dll") || !_stricmp(file, "gdi32full.dll")) continue;
      fprintf(out, " %s+%#llx", file, (unsigned long long)((uint8_t *)frames[i] - (uint8_t *)m)); ++shown;
   }
   fprintf(out, "\n");
}
bool show_callers(const char *name)
{
   static const char *names[] = {"CreateDevice", "CreateAllocation", "OpenResource", "QueryResourceInfo", "SubmitCommand", "Escape", "CreateContextVirtual"};
   for (const char *n : names) if (!strcmp(name, n)) return true;
   return false;
}
extern "C" NTSTATUS handler(void *data, uint64_t index)
{
   const Hook &hook = hooks[index];
   const bool verbose = !quiet(hook.name);
   if (verbose) {
      std::lock_guard<std::mutex> lock(gate);
      fprintf(out, "[%s] tid=%lu %s(%p)\n", phase.load(), GetCurrentThreadId(), hook.name, data);
      if (show_callers(hook.name)) log_callers();
      decode_pre(hook.name, data); fflush(out);
   }
   const NTSTATUS status = reinterpret_cast<Syscall>(hook.original)(data);
   std::lock_guard<std::mutex> lock(gate);
   if (verbose) { fprintf(out, "    status=%#lx\n", (unsigned long)status); decode_post(hook.name, data); }
   else fprintf(out, "[%s] %s status=%#lx\n", phase.load(), hook.name, (unsigned long)status);
   fflush(out);
   return status;
}
/* Other win32u syscalls (any argument count): name and first three arguments only. */
struct LightHook { char name[96]; };
LightHook light[4096];
unsigned light_count;
std::atomic<bool> light_enabled{false};
extern "C" void light_logger(uint64_t index, uint64_t a1, uint64_t a2, uint64_t a3)
{
   if (!light_enabled.load()) return;
   const char *name = light[index].name;
   if (!strncmp(name, "NtUserPeekMessage", 17) || !strncmp(name, "NtUserGetThreadState", 20) || !strncmp(name, "NtUserCallOneParam", 18)) return;
   std::lock_guard<std::mutex> lock(gate);
   fprintf(out, "[%s] tid=%lu %s(%#llx, %#llx, %#llx)\n", phase.load(), GetCurrentThreadId(), name,
      (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3);
   fflush(out);
}
extern "C" void return_logger(uint64_t index, uint64_t result)
{
   if (!light_enabled.load()) return;
   std::lock_guard<std::mutex> lock(gate);
   fprintf(out, "    %s -> %#llx\n", light[index].name, (unsigned long long)result);
   fflush(out);
}
/* Syscalls with at most four arguments whose return value is logged too. */
bool log_return(const char *name)
{
   static const char *names[] = {"NtUserGetProp2", "NtUserGetProp", "NtGdiGetDCDword", "NtDxgkGetProperties", "NtUserWindowFromDC"};
   for (const char *n : names) if (!strcmp(name, n)) return true;
   return false;
}
void *exec_alloc(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
/* For log_return syscalls: call the light stub as a function, then log rax. */
uint8_t *emit_return_stub(uint8_t *p, uint64_t index, const void *inner)
{
   *p++ = 0x48; *p++ = 0x83; *p++ = 0xec; *p++ = 0x38;                                       /* sub rsp, 38h */
   const uint64_t t = reinterpret_cast<uint64_t>(inner);
   *p++ = 0x48; *p++ = 0xb8; memcpy(p, &t, 8); p += 8; *p++ = 0xff; *p++ = 0xd0;            /* mov rax, inner; call rax */
   const uint8_t save[] = {0x48, 0x89, 0x44, 0x24, 0x30}; memcpy(p, save, 5); p += 5;      /* mov [rsp+30h], rax */
   *p++ = 0x48; *p++ = 0xb9; memcpy(p, &index, 8); p += 8;                                   /* mov rcx, index */
   *p++ = 0x48; *p++ = 0x89; *p++ = 0xc2;                                                    /* mov rdx, rax */
   const uint64_t logger = reinterpret_cast<uint64_t>(&return_logger);
   *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8; *p++ = 0xff; *p++ = 0xd0;       /* mov rax, logger; call rax */
   const uint8_t restore[] = {0x48, 0x8b, 0x44, 0x24, 0x30, 0x48, 0x83, 0xc4, 0x38, 0xc3};   /* mov rax,[rsp+30h]; add rsp,38h; ret */
   memcpy(p, restore, sizeof(restore)); p += sizeof(restore);
   return p;
}
/* Stub that calls light_logger(index, rcx, rdx, r8) and then the original
 * syscall with all argument registers and stack arguments untouched. */
uint8_t *emit_light_stub(uint8_t *p, uint64_t index, const void *trampoline)
{
   const uint8_t prologue[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x28,   /* push rcx,rdx,r8,r9; sub rsp,28h */
                               0x4d, 0x89, 0xc1, 0x49, 0x89, 0xd0, 0x48, 0x89, 0xca};        /* mov r9,r8; mov r8,rdx; mov rdx,rcx */
   memcpy(p, prologue, sizeof(prologue)); p += sizeof(prologue);
   *p++ = 0x48; *p++ = 0xb9; memcpy(p, &index, 8); p += 8;                                   /* mov rcx, index */
   const uint64_t logger = reinterpret_cast<uint64_t>(&light_logger);
   *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8; *p++ = 0xff; *p++ = 0xd0;       /* mov rax, logger; call rax */
   const uint8_t epilogue[] = {0x48, 0x83, 0xc4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59};  /* add rsp,28h; pop r9,r8,rdx,rcx */
   memcpy(p, epilogue, sizeof(epilogue)); p += sizeof(epilogue);
   const uint64_t t = reinterpret_cast<uint64_t>(trampoline);
   *p++ = 0x48; *p++ = 0xb8; memcpy(p, &t, 8); p += 8; *p++ = 0xff; *p++ = 0xe0;            /* mov rax, trampoline; jmp rax */
   return p;
}
/* Redirect every NtGdiDdDDI* stub of win32u that has the known syscall layout. */
unsigned install_hooks()
{
   HMODULE module = LoadLibraryW(L"win32u.dll");
   auto base = reinterpret_cast<uint8_t *>(module);
   auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
   auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
   auto exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
   auto names = reinterpret_cast<uint32_t *>(base + exports->AddressOfNames);
   auto ordinals = reinterpret_cast<uint16_t *>(base + exports->AddressOfNameOrdinals);
   auto functions = reinterpret_cast<uint32_t *>(base + exports->AddressOfFunctions);
   static const uint8_t expected[24] = {0x4c,0x8b,0xd1,0xb8,0,0,0,0,0xf6,0x04,0x25,0x08,0x03,0xfe,0x7f,0x01,0x75,0x03,0x0f,0x05,0xc3,0xcd,0x2e,0xc3};
   uint8_t *code = static_cast<uint8_t *>(exec_alloc(1 << 20)), *next = code;
   unsigned skipped = 0;
   for (DWORD i = 0; i < exports->NumberOfNames && hook_count < 1024 && light_count < 4096; ++i) {
      const char *name = reinterpret_cast<const char *>(base + names[i]);
      if (strncmp(name, "Nt", 2)) continue;
      uint8_t *target = base + functions[ordinals[i]];
      bool match = true; for (int b = 0; b < 24; ++b) if ((b < 4 || b > 7) && target[b] != expected[b]) match = false;
      if (!match) { ++skipped; fprintf(out, "unknown syscall layout: %s\n", name); continue; }
      if (strncmp(name, "NtGdiDdDDI", 10)) {
         strncpy_s(light[light_count].name, name, _TRUNCATE);
         memcpy(next, target, 24); const void *trampoline = next; next += 32;
         uint8_t *stub = next; next = emit_light_stub(next, light_count, trampoline); next += 16 - (uintptr_t)next % 16;
         if (log_return(name)) { uint8_t *outer = next; next = emit_return_stub(next, light_count, stub); next += 16 - (uintptr_t)next % 16; stub = outer; }
         uint8_t jump[12] = {0x48, 0xb8}; const uint64_t s = reinterpret_cast<uint64_t>(stub); memcpy(jump + 2, &s, 8); jump[10] = 0xff; jump[11] = 0xe0;
         DWORD old; VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &old);
         memcpy(target, jump, 12); VirtualProtect(target, 12, old, &old);
         ++light_count;
         continue;
      }
      Hook &hook = hooks[hook_count];
      strncpy_s(hook.name, name + 10, _TRUNCATE);
      memcpy(next, target, 24); hook.original = next; next += 32;              /* trampoline: the original stub */
      uint8_t *stub = next; next += 32;                                        /* mov rdx, index; mov rax, handler; jmp rax */
      stub[0] = 0x48; stub[1] = 0xba; const uint64_t index = hook_count; memcpy(stub + 2, &index, 8);
      stub[10] = 0x48; stub[11] = 0xb8; const uint64_t h = reinterpret_cast<uint64_t>(&handler); memcpy(stub + 12, &h, 8);
      stub[20] = 0xff; stub[21] = 0xe0;
      uint8_t jump[12] = {0x48, 0xb8}; const uint64_t s = reinterpret_cast<uint64_t>(stub); memcpy(jump + 2, &s, 8); jump[10] = 0xff; jump[11] = 0xe0;
      DWORD old; VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &old);
      memcpy(target, jump, 12); VirtualProtect(target, 12, old, &old);
      ++hook_count;
   }
   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
   fprintf(out, "hooked %u NtGdiDdDDI entry points (decoded) and %u other win32u syscalls (names only), %u with an unknown layout skipped\n",
           hook_count, light_count, skipped);
   return hook_count;
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
   if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
   return DefWindowProcW(window, message, w, l);
}
void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
} // namespace

/* d3d11_present_trace.cpp reuses the hooks above with its own main. */
#ifndef PRESENT_TRACE_NO_MAIN
int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: GlPresentTrace <log> [frames]\n"); return 2; }
   const int frames = argc > 2 ? atoi(argv[2]) : 4;
   if (fopen_s(&out, argv[1], "w") || !out) return 3;
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // physical screen coordinates
   SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);            // a sleeping display makes every present "occluded"
   Sleep(1500);
   if (!install_hooks()) return 4;
   HINSTANCE instance = GetModuleHandleW(nullptr);
   WNDCLASSW wc{}; wc.style = CS_OWNDC; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"GlPresentTrace";
   RegisterClassW(&wc);
   phase = "window"; light_enabled = true;
   HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"GL present trace", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240,
                                 nullptr, nullptr, instance, nullptr);
   if (!window) return 5;
   pump();
   HDC dc = GetDC(window);
   PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
   pfd.cDepthBits = 24; pfd.iLayerType = PFD_MAIN_PLANE;
   phase = "pixel format";
   const int format = ChoosePixelFormat(dc, &pfd);
   if (!format || !SetPixelFormat(dc, format, &pfd)) return 6;
   phase = "create context";
   HGLRC context = wglCreateContext(dc);
   if (!context) return 7;
   phase = "make current";
   if (!wglMakeCurrent(dc, context)) return 8;
   { std::lock_guard<std::mutex> lock(gate);
     fprintf(out, "GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s\n", (const char *)glGetString(GL_VENDOR),
             (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION)); fflush(out); }
   static const char *frame_names[] = {"frame 0", "frame 1", "frame 2", "frame 3", "frame 4", "frame 5", "frame 6", "frame 7"};
   for (int f = 0; f < frames && f < 8; ++f) {
      phase = frame_names[f];
      glClearColor(f & 1 ? 1.f : 0.f, f & 2 ? 1.f : 0.f, 0.5f, 1.f); glClear(GL_COLOR_BUFFER_BIT);
      SwapBuffers(dc); pump(); DwmFlush(); Sleep(150); DwmFlush(); pump();
      {  /* Control for present_redirect_probe: the composed pixel at the client centre. */
         RECT r; GetClientRect(window, &r); POINT c{(r.right) / 2, (r.bottom) / 2}; ClientToScreen(window, &c);
         HDC desktop = GetDC(nullptr); const COLORREF s = GetPixel(desktop, c.x, c.y); ReleaseDC(nullptr, desktop);
         RECT w; GetWindowRect(window, &w);
         HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, w.right - w.left, w.bottom - w.top);
         HGDIOBJ old = SelectObject(memory, bitmap); PrintWindow(window, memory, 2 /* PW_RENDERFULLCONTENT */);
         const COLORREF p = GetPixel(memory, c.x - w.left, c.y - w.top);
         SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
         std::lock_guard<std::mutex> lock(gate);
         fprintf(out, "SCREEN after frame %d: screen=%02x%02x%02x printwindow=%02x%02x%02x (cleared to %02x%02x80)\n", f,
                 GetRValue(s), GetGValue(s), GetBValue(s), GetRValue(p), GetGValue(p), GetBValue(p), f & 1 ? 0xff : 0, f & 2 ? 0xff : 0); fflush(out);
      }
   }
   light_enabled = false;
   phase = "finish"; glFinish();
   phase = "teardown";
   wglMakeCurrent(nullptr, nullptr); wglDeleteContext(context); ReleaseDC(window, dc); DestroyWindow(window); pump();
   phase = "exit";
   { std::lock_guard<std::mutex> lock(gate);
     HMODULE modules[512]; DWORD bytes = 0;
     if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
        fprintf(out, "loaded modules:");
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) { char path[MAX_PATH]; GetModuleFileNameA(modules[i], path, MAX_PATH);
           const char *file = strrchr(path, '\\'); fprintf(out, " %s", file ? file + 1 : path); }
        fprintf(out, "\n");
     }
     fprintf(out, "done\n"); fflush(out); }
   return 0;
}
#endif
