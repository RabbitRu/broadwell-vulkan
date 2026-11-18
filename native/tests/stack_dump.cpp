/* Prints the call stack of every thread of a running process (x64), for
 * finding where a hung test or game waits. Symbols from dbghelp: exports for
 * system DLLs, PDBs next to the modules or on [symbol-path].
 * Usage: stack_dump.exe <pid> [symbol-path] */
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s <pid> [symbol-path]\n", argv[0]); return 2; }
   const DWORD pid = strtoul(argv[1], nullptr, 10);
   HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
   if (!process) { fprintf(stderr, "OpenProcess failed\n"); return 1; }
   SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
   if (!SymInitialize(process, argc > 2 ? argv[2] : nullptr, TRUE)) { fprintf(stderr, "SymInitialize failed\n"); return 1; }
   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
   THREADENTRY32 te{sizeof(te)};
   for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
      if (!thread) continue;
      SuspendThread(thread);
      CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &ctx)) {
         {  /* the current instruction, even when the walk below fails */
            char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto *symbol = (SYMBOL_INFO *)buffer; symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            printf("thread %lu: rip %s+0x%llx\n", te.th32ThreadID, SymFromAddr(process, ctx.Rip, &displacement, symbol) ? symbol->Name : "?",
                   (unsigned long long)displacement);
         }
         STACKFRAME64 frame{};
         frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
         frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
         frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
         for (int depth = 0; depth < 40; ++depth) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
               break;
            char buffer[sizeof(SYMBOL_INFO) + 512]{};
            auto *symbol = (SYMBOL_INFO *)buffer; symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULE64 module{sizeof(module)};
            const char *mod = SymGetModuleInfo64(process, frame.AddrPC.Offset, &module) ? module.ModuleName : "?";
            IMAGEHLP_LINE64 line{sizeof(line)}; DWORD line_displacement = 0;
            const bool has_line = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line);
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
               printf("  %s!%s+0x%llx", mod, symbol->Name, (unsigned long long)displacement);
            else
               printf("  %s!0x%llx", mod, (unsigned long long)frame.AddrPC.Offset);
            if (has_line) printf("  (%s:%lu)", line.FileName, line.LineNumber);
            printf("\n");
         }
      }
      ResumeThread(thread);
      CloseHandle(thread);
   }
   CloseHandle(snap);
   SymCleanup(process);
   return 0;
}
