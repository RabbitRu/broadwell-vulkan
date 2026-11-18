/* Poor man's sampling profiler for a running process.
 *
 * Every ~1 ms each thread of the process is suspended, its instruction
 * pointer recorded and the thread resumed. Per thread: CPU time (user and
 * kernel, from GetThreadTimes) over the sampling period and its hottest
 * symbols, resolved with dbghelp (exports for system DLLs, PDBs where found).
 * A thread inside a system call shows the syscall stub (e.g. NtWaitFor... when
 * blocked, a D3DKMT/NtGdiDdDDI call while the kernel works for it).
 * Usage: rip_sampler.exe <pid> <seconds> [symbol-path] */
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct Thread { HANDLE handle; DWORD id; ULONGLONG user0, kernel0; std::map<DWORD64, uint64_t> ips; uint64_t samples = 0; };
static ULONGLONG ticks(FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: %s <pid> <seconds> [symbol-path]\n", argv[0]); return 2; }
   const DWORD pid = strtoul(argv[1], nullptr, 10);
   const double seconds = atof(argv[2]);
   HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
   if (!process) { fprintf(stderr, "OpenProcess failed\n"); return 1; }
   timeBeginPeriod(1);
   std::map<DWORD, Thread> threads;
   auto refresh = [&] {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te{sizeof(te)};
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
         if (te.th32OwnerProcessID != pid || threads.count(te.th32ThreadID)) continue;
         HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
         if (!h) continue;
         FILETIME c, x, k, u; GetThreadTimes(h, &c, &x, &k, &u);
         threads[te.th32ThreadID] = Thread{h, te.th32ThreadID, ticks(u), ticks(k)};
      }
      CloseHandle(snap);
   };
   refresh();
   const ULONGLONG start = GetTickCount64(), end = start + (ULONGLONG)(seconds * 1000);
   unsigned round = 0;
   while (GetTickCount64() < end) {
      if (++round % 500 == 0) refresh();
      for (auto &[id, t] : threads) {
         if (SuspendThread(t.handle) == (DWORD)-1) continue;
         CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
         if (GetThreadContext(t.handle, &ctx)) { ++t.ips[ctx.Rip]; ++t.samples; }
         ResumeThread(t.handle);
      }
      Sleep(1);
   }
   const double wall = (GetTickCount64() - start) / 1000.0;
   timeEndPeriod(1);
   SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS);
   SymInitialize(process, argc > 3 ? argv[3] : nullptr, TRUE);
   struct Row { Thread *t; double user, kernel; };
   std::vector<Row> rows;
   for (auto &[id, t] : threads) {
      FILETIME c, x, k, u; GetThreadTimes(t.handle, &c, &x, &k, &u);
      rows.push_back({&t, (ticks(u) - t.user0) / 1e7, (ticks(k) - t.kernel0) / 1e7});
   }
   std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.user + a.kernel > b.user + b.kernel; });
   printf("wall %.1f s, %zu threads\n", wall, rows.size());
   for (size_t r = 0; r < rows.size() && r < 6; ++r) {
      Row &row = rows[r];
      printf("== thread %lu: user %.2f s, kernel %.2f s (%.0f%% of one core), %llu samples\n", row.t->id, row.user, row.kernel,
             100.0 * (row.user + row.kernel) / wall, (unsigned long long)row.t->samples);
      std::map<std::string, uint64_t> by_symbol;
      for (auto &[ip, n] : row.t->ips) {
         char buffer[sizeof(SYMBOL_INFO) + 256] = {}; auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
         sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
         DWORD64 displacement = 0;
         IMAGEHLP_MODULE64 mod{}; mod.SizeOfStruct = sizeof(mod);
         std::string name = SymGetModuleInfo64(process, ip, &mod) ? std::string(mod.ModuleName) : std::string("?");
         if (SymFromAddr(process, ip, &displacement, sym)) name += "!" + std::string(sym->Name);
         by_symbol[name] += n;
      }
      std::vector<std::pair<std::string, uint64_t>> v(by_symbol.begin(), by_symbol.end());
      std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
      for (size_t i = 0; i < v.size() && i < 14; ++i) printf("   %5.1f%% %s\n", 100.0 * v[i].second / std::max<uint64_t>(row.t->samples, 1), v[i].first.c_str());
   }
   return 0;
}
