/* Poor man's sampling profiler for a running process.
 *
 * Every ~1 ms each thread of the process is suspended, its instruction
 * pointer recorded and the thread resumed. Per thread: CPU time over the
 * sampling period (QueryThreadCycleTime: exact, unlike GetThreadTimes, which
 * counts timer ticks), its hottest symbols resolved with dbghelp (exports for
 * system DLLs, PDBs where found) and the share of samples by module.
 * A thread inside a system call shows the syscall stub (e.g. NtWaitFor... when
 * blocked, a D3DKMT/NtGdiDdDDI call while the kernel works for it); such
 * samples are walked up the stack and listed by who called: the first three
 * frames outside ntdll/kernelbase/kernel32/win32u/gdi32 (module!function with
 * a PDB, module+rva without one), so a wait can be told apart by its call site.
 * With a module name as the fourth argument, every sample is also walked up
 * the stack (StackWalk64): the share of samples with that module anywhere on
 * the stack (its inclusive time, system calls it makes included) and, for
 * those, the outermost function of the module (the entry point it was called
 * through) and the innermost one.
 * Threads that use under 1 % of a CPU are sampled only for the first half
 * second: walking two dozen idle threads every millisecond slows the process.
 * Usage: rip_sampler.exe <pid> <seconds> [symbol-path] [module] */
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

struct Thread { HANDLE handle; DWORD id; ULONGLONG user0, kernel0; std::map<DWORD64, uint64_t> ips; uint64_t samples = 0;
                uint64_t inside = 0; std::map<DWORD64, uint64_t> entry, inner; std::map<std::vector<DWORD64>, uint64_t> chains;
                ULONG64 cycles0 = 0, cycles_mark = 0, tsc0 = 0; bool active = true;
                std::map<int, uint64_t> leaf_modules; std::map<std::array<DWORD64, 4>, uint64_t> syscalls; };
struct Module { DWORD64 base, end; std::string name; bool wrapper; };
static std::vector<Module> modules;
static ULONGLONG ticks(FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

static void load_modules(DWORD pid)
{
   static const char *wrappers[] = {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll", "gdi32.dll", "gdi32full.dll"};
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
   if (snap == INVALID_HANDLE_VALUE) return;
   modules.clear();
   MODULEENTRY32 me{sizeof(me)};
   for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
      Module m{(DWORD64)me.modBaseAddr, (DWORD64)me.modBaseAddr + me.modBaseSize, me.szModule, false};
      for (const char *w : wrappers) if (!_stricmp(me.szModule, w)) m.wrapper = true;
      modules.push_back(m);
   }
   CloseHandle(snap);
}

static int module_of(DWORD64 pc)
{
   for (size_t i = 0; i < modules.size(); ++i)
      if (pc >= modules[i].base && pc < modules[i].end) return (int)i;
   return -1;
}

int main(int argc, char **argv)
{
   if (argc < 3) { fprintf(stderr, "usage: %s <pid> <seconds> [symbol-path] [module]\n", argv[0]); return 2; }
   const DWORD pid = strtoul(argv[1], nullptr, 10);
   const double seconds = atof(argv[2]);
   HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
   if (!process) { fprintf(stderr, "OpenProcess failed\n"); return 1; }
   const char *module = argc > 4 ? argv[4] : nullptr;
   SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
   SymInitialize(process, argc > 3 ? argv[3] : nullptr, TRUE);
   load_modules(pid);
   DWORD64 module_base = 0, module_end = 0;
   if (module) {
      for (const Module &m : modules)
         if (!_stricmp(m.name.c_str(), module)) { module_base = m.base; module_end = m.end; }
      if (!module_base) fprintf(stderr, "module %s not loaded in the process\n", module);
   }
   /* RIP_NOWALK=1: instruction pointers only, no stack walks (about 1 kHz and
    * little slowdown; with walks a large process is sampled at 30-80 Hz).
    * RIP_RVAS=<n>: per thread, the n hottest 4 KiB code pages and the n hottest
    * addresses as module+rva: comparable between runs of the same binary, also
    * where there are no symbols. */
   const bool nowalk = getenv("RIP_NOWALK") != nullptr;
   const int rvas = getenv("RIP_RVAS") ? atoi(getenv("RIP_RVAS")) : 0;
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
         Thread t{h, te.th32ThreadID, ticks(u), ticks(k)};
         QueryThreadCycleTime(h, &t.cycles0); t.cycles_mark = t.cycles0; t.tsc0 = __rdtsc();
         threads[te.th32ThreadID] = t;
      }
      CloseHandle(snap);
   };
   refresh();
   const ULONGLONG start = GetTickCount64(), end = start + (ULONGLONG)(seconds * 1000);
   ULONG64 tsc_mark = __rdtsc();
   const ULONG64 tsc_start = tsc_mark;
   unsigned round = 0;
   while (GetTickCount64() < end) {
      if (++round % 500 == 0) {
         /* Keep sampling only the threads that used at least 1 % of a CPU since the last mark. */
         const ULONG64 now = __rdtsc();
         for (auto &[id, t] : threads) {
            ULONG64 cycles = 0; QueryThreadCycleTime(t.handle, &cycles);
            t.active = (double)(cycles - t.cycles_mark) >= 0.01 * (double)(now - tsc_mark);
            t.cycles_mark = cycles;
         }
         tsc_mark = now;
         refresh();
      }
      for (auto &[id, t] : threads) {
         if (!t.active) continue;
         if (SuspendThread(t.handle) == (DWORD)-1) continue;
         CONTEXT ctx{}; ctx.ContextFlags = nowalk ? CONTEXT_CONTROL : CONTEXT_FULL;
         std::array<DWORD64, 4> callers{}; int leaf_module = -1; bool in_wrapper = false;
         const bool got = GetThreadContext(t.handle, &ctx);
         if (got) {
            ++t.ips[ctx.Rip]; ++t.samples;
            leaf_module = module_of(ctx.Rip);
            in_wrapper = leaf_module >= 0 && modules[leaf_module].wrapper;
            if (nowalk) in_wrapper = false;
            if (!nowalk && (module_base || in_wrapper)) {
               STACKFRAME64 frame{}; frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
               frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
               frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
               CONTEXT walk = ctx; DWORD64 outer = 0, inner = 0; std::vector<DWORD64> chain;
               int found = 1; callers[0] = ctx.Rip;
               for (int depth = 0; depth < 48; ++depth) {
                  if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, t.handle, &frame, &walk, nullptr, SymFunctionTableAccess64,
                                   SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                     break;
                  const DWORD64 pc = frame.AddrPC.Offset;
                  if (module_base && pc >= module_base && pc < module_end) { outer = pc; if (!inner) inner = pc; if (chain.size() < 7) chain.push_back(pc); }
                  if (in_wrapper && depth > 0 && found < 4) {
                     const int m = module_of(pc);
                     if (m < 0 || !modules[m].wrapper) callers[found++] = pc;
                  }
                  if (!module_base && found == 4) break;
               }
               if (outer) { ++t.inside; ++t.entry[outer]; ++t.inner[inner]; ++t.chains[chain]; }
            }
         }
         ResumeThread(t.handle);
         if (got) {
            ++t.leaf_modules[leaf_module];
            if (in_wrapper) ++t.syscalls[callers];
         }
      }
      Sleep(1);
   }
   const double wall = (GetTickCount64() - start) / 1000.0;
   const ULONG64 tsc_end = __rdtsc();
   const double tsc_hz = (double)(tsc_end - tsc_start) / wall;
   timeEndPeriod(1);
   load_modules(pid);   /* names for modules loaded meanwhile */

   struct Row { Thread *t; double user, kernel, cpu; };
   std::vector<Row> rows;
   for (auto &[id, t] : threads) {
      FILETIME c, x, k, u; GetThreadTimes(t.handle, &c, &x, &k, &u);
      ULONG64 cycles = 0; QueryThreadCycleTime(t.handle, &cycles);
      rows.push_back({&t, (ticks(u) - t.user0) / 1e7, (ticks(k) - t.kernel0) / 1e7,
                      100.0 * (double)(cycles - t.cycles0) / std::max<double>((double)(tsc_end - t.tsc0), 1.0)});
   }
   std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.cpu > b.cpu; });
   double all = 0; for (const Row &r : rows) all += r.cpu;
   printf("wall %.1f s, %zu threads, TSC %.3f GHz, all threads %.1f%% of one logical CPU\n", wall, rows.size(), tsc_hz / 1e9, all);
   auto symbol_of = [&](DWORD64 ip) {
      char buffer[sizeof(SYMBOL_INFO) + 256] = {}; auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255; DWORD64 d = 0;
      return SymFromAddr(process, ip, &d, sym) ? std::string(sym->Name) : std::string("?");
   };
   auto caller_name = [&](DWORD64 pc) -> std::string {
      if (!pc) return "";
      const int m = module_of(pc);
      if (m < 0) return "?";
      const std::string symbol = symbol_of(pc);   /* loads the module's symbols */
      IMAGEHLP_MODULE64 info{}; info.SizeOfStruct = sizeof(info);
      if (symbol != "?" && SymGetModuleInfo64(process, pc, &info) && info.SymType == SymPdb) return modules[m].name + "!" + symbol;
      char rva[32]; snprintf(rva, sizeof rva, "+0x%llx", (unsigned long long)(pc - modules[m].base));
      return modules[m].name + rva;
   };
   for (size_t r = 0; r < rows.size() && r < 8; ++r) {
      Row &row = rows[r];
      const double samples = (double)std::max<uint64_t>(row.t->samples, 1);
      printf("== thread %lu: %.1f%% of one logical CPU (user %.2f s, kernel %.2f s by thread times), %llu samples\n", row.t->id, row.cpu,
             row.user, row.kernel, (unsigned long long)row.t->samples);
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
      for (size_t i = 0; i < v.size() && i < 14; ++i) printf("   %5.1f%% %s\n", 100.0 * v[i].second / samples, v[i].first.c_str());

      std::vector<std::pair<std::string, uint64_t>> mods;
      for (auto &[m, n] : row.t->leaf_modules) mods.push_back({m >= 0 ? modules[m].name : std::string("?"), n});
      std::sort(mods.begin(), mods.end(), [](auto &a, auto &b) { return a.second > b.second; });
      printf("   -- samples by module:");
      for (size_t i = 0; i < mods.size() && i < 10; ++i)
         if (100.0 * mods[i].second / samples >= 0.3) printf(" %s %.1f%%,", mods[i].first.c_str(), 100.0 * mods[i].second / samples);
      printf("\n");

      if (rvas > 0) {
         std::map<std::string, uint64_t> pages, exact;
         for (auto &[ip, n] : row.t->ips) {
            const int m = module_of(ip);
            if (m < 0) continue;
            char text[320];
            snprintf(text, sizeof text, "%s+0x%llx", modules[m].name.c_str(), (unsigned long long)((ip - modules[m].base) & ~0xfffull));
            pages[text] += n;
            snprintf(text, sizeof text, "%s+0x%llx", modules[m].name.c_str(), (unsigned long long)(ip - modules[m].base));
            exact[text] += n;
         }
         for (auto *which : {&pages, &exact}) {
            std::vector<std::pair<std::string, uint64_t>> w(which->begin(), which->end());
            std::sort(w.begin(), w.end(), [](auto &a, auto &b) { return a.second > b.second; });
            printf("   -- hottest %s:\n", which == &pages ? "code pages (4 KiB)" : "addresses");
            for (size_t i = 0; i < w.size() && i < (size_t)rvas; ++i) printf("      %5.2f%% %s\n", 100.0 * w[i].second / samples, w[i].first.c_str());
         }
      }

      if (!row.t->syscalls.empty()) {
         std::map<std::string, uint64_t> named;
         for (auto &[key, n] : row.t->syscalls) {
            std::string text = symbol_of(key[0]);
            for (int i = 1; i < 4; ++i) { const std::string c = caller_name(key[i]); if (!c.empty()) text += " < " + c; }
            named[text] += n;
         }
         std::vector<std::pair<std::string, uint64_t>> w(named.begin(), named.end());
         std::sort(w.begin(), w.end(), [](auto &a, auto &b) { return a.second > b.second; });
         printf("   -- inside system DLLs, by caller (innermost first):\n");
         for (size_t i = 0; i < w.size() && i < 16; ++i) printf("      %5.1f%% %s\n", 100.0 * w[i].second / samples, w[i].first.c_str());
      }

      if (module_base && row.t->inside) {
         auto name_of = symbol_of;
         printf("   -- %s on the stack in %.1f%% of samples\n", module, 100.0 * row.t->inside / samples);
         for (auto *which : {&row.t->entry, &row.t->inner}) {
            std::map<std::string, uint64_t> named;
            for (auto &[ip, n] : *which) named[name_of(ip)] += n;
            std::vector<std::pair<std::string, uint64_t>> w(named.begin(), named.end());
            std::sort(w.begin(), w.end(), [](auto &a, auto &b) { return a.second > b.second; });
            printf("   -- %s:\n", which == &row.t->entry ? "entered through" : "innermost in the module");
            for (size_t i = 0; i < w.size() && i < 14; ++i) printf("      %5.1f%% %s\n", 100.0 * w[i].second / samples, w[i].first.c_str());
         }
         std::map<std::string, uint64_t> named_chains;
         for (auto &[chain, n] : row.t->chains) {
            std::string text; std::string last;
            for (DWORD64 ip : chain) { std::string s = name_of(ip); if (s != last) { if (!text.empty()) text += " < "; text += s; last = s; } }
            named_chains[text] += n;
         }
         std::vector<std::pair<std::string, uint64_t>> w(named_chains.begin(), named_chains.end());
         std::sort(w.begin(), w.end(), [](auto &a, auto &b) { return a.second > b.second; });
         printf("   -- call chains in the module (innermost first):\n");
         for (size_t i = 0; i < w.size() && i < 16; ++i) printf("      %5.1f%% %s\n", 100.0 * w[i].second / samples, w[i].first.c_str());
         /* RIP_LINES=<n>: the n hottest source lines of the module (PDB line
          * info; inlined code is attributed to the line it came from). */
         if (const char *lines_env = getenv("RIP_LINES")) {
            std::map<std::string, uint64_t> by_line;
            for (auto &[ip, n] : row.t->ips) {
               if (ip < module_base || ip >= module_end) continue;
               IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD d = 0;
               std::string key = name_of(ip);
               /* Innermost inlined frame first: otherwise inlined code is
                * attributed to the line of the call it was inlined into. */
               DWORD inline_context = 0, frame_index = 0; bool found = false;
               if (SymAddrIncludeInlineTrace(process, ip) &&
                   SymQueryInlineTrace(process, ip, 0, ip, ip, &inline_context, &frame_index)) {
                  found = SymGetLineFromInlineContext(process, ip, inline_context, 0, &d, &line);
                  char buffer[sizeof(SYMBOL_INFO) + 256] = {}; auto *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
                  sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255; DWORD64 d64 = 0;
                  if (found && SymFromInlineContext(process, ip, inline_context, &d64, sym)) key = sym->Name;
               }
               if (found || SymGetLineFromAddr64(process, ip, &d, &line)) {
                  const char *file = strrchr(line.FileName, '\\'); file = file ? file + 1 : line.FileName;
                  key = std::string(file) + ":" + std::to_string(line.LineNumber) + " (" + key + ")";
               }
               by_line[key] += n;
            }
            std::vector<std::pair<std::string, uint64_t>> l(by_line.begin(), by_line.end());
            std::sort(l.begin(), l.end(), [](auto &a, auto &b) { return a.second > b.second; });
            printf("   -- hottest source lines in the module:\n");
            for (size_t i = 0; i < l.size() && i < (size_t)atoi(lines_env); ++i)
               printf("      %5.1f%% %s\n", 100.0 * l[i].second / samples, l[i].first.c_str());
         }
      }
   }
   return 0;
}
