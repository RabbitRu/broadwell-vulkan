/* Finds the machine code that Intel's driver compiled for a shader of this
 * process, to compare it with ELK's. The driver keeps its kernels in memory
 * mapped into the process; a kernel is recognized by immediates of the
 * shader: every place where all the given 32-bit values occur within `window`
 * bytes is written with its surroundings to <prefix>-<n>.bin (other copies of
 * the constants match too, for example the DXBC; the disassembly tells them
 * apart). Disassemble with
 *    IrisElkCompile.exe --disasm <file> [offset [length]]
 * The D3D11 probes call this when D3D11_PROBE_DUMP=<prefix> is set.
 * The driver's command buffers stay mapped as well: a search for a command
 * header (0x7105000d, GPGPU_WALKER) with a tag finds them
 * (<prefix><tag>-<n>.bin, raw dwords around the match); 0x7b000005 (3DPRIMITIVE)
 * finds the ones with draws, of Intel's driver and of this ICD alike. */
#pragma once
/* D3D11_PROBE_FIND=<hex dword>: also dumps every place where that dword occurs
 * (<prefix>-find-<n>.bin), for example 11000001, MI_LOAD_REGISTER_IMM. */
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static inline void DumpKernelCandidates(const uint32_t *values, size_t count, size_t window = 256, const char *tag = "", size_t span = 2048, unsigned limit = 24)
{
   char prefix[MAX_PATH];
   if (!GetEnvironmentVariableA("D3D11_PROBE_DUMP", prefix, sizeof(prefix))) return;
   const size_t before = span, after = span, chunk_size = 1 << 20;
   std::vector<uint8_t> chunk(chunk_size);
   unsigned found = 0;
   MEMORY_BASIC_INFORMATION info;
   for (uint8_t *address = nullptr; VirtualQuery(address, &info, sizeof(info)) && found < limit;
        address = (uint8_t *)info.BaseAddress + info.RegionSize) {
      if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || info.Type == MEM_IMAGE) continue;
      if ((uint8_t *)info.BaseAddress <= chunk.data() && chunk.data() < (uint8_t *)info.BaseAddress + info.RegionSize) continue;
      for (size_t offset = 0; offset < info.RegionSize && found < limit; offset += chunk_size) {
         const size_t size = info.RegionSize - offset < chunk_size ? info.RegionSize - offset : chunk_size;
         SIZE_T read = 0;
         if (!ReadProcessMemory(GetCurrentProcess(), (uint8_t *)info.BaseAddress + offset, chunk.data(), size, &read) || read < 8) continue;
         for (size_t i = 0; i + 4 <= read; i += 4) {
            uint32_t word; memcpy(&word, &chunk[i], 4);
            if (word != values[0]) continue;
            const size_t low = i > window ? i - window : 0, high = i + window < read ? i + window : read;
            bool all = true;
            for (size_t v = 1; v < count && all; ++v) {
               all = false;
               for (size_t j = low; j + 4 <= high && !all; j += 4) { memcpy(&word, &chunk[j], 4); all = word == values[v]; }
            }
            if (!all) continue;
            const size_t start = (i > before ? i - before : 0) & ~(size_t)63, end = i + after < read ? i + after : read;
            const std::string name = std::string(prefix) + tag + "-" + std::to_string(found++) + ".bin";
            if (FILE *file = fopen(name.c_str(), "wb")) { fwrite(&chunk[start], 1, end - start, file); fclose(file); }
            printf("kernel candidate %s: address %p (match at +%zu), protect 0x%lx, type 0x%lx\n", name.c_str(),
                   (void *)((uint8_t *)info.BaseAddress + offset + start), i - start, info.Protect, info.Type);
            i = end;
         }
      }
   }
   if (!found) printf("no kernel candidates found\n");
}

static inline void DumpFoundWords()
{
   char text[32];
   if (!GetEnvironmentVariableA("D3D11_PROBE_FIND", text, sizeof(text))) return;
   const uint32_t word[] = {(uint32_t)strtoul(text, nullptr, 16)};
   DumpKernelCandidates(word, 1, 256, "-find", 256, 400);
}
