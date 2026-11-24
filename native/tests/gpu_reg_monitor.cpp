/* GPU register monitor: every <interval> ms submits a tiny batch that stores
 * the given registers with MI_STORE_REGISTER_MEM and prints every change with
 * its time. Only reads registers; changes no GPU state. Made to look for a
 * change of the GT's power state next to an intermittent hang.
 *
 * Useful Broadwell registers:
 *   138064  GEN8_GT_SLICE_INFO: slices powered up, bits 2:0
 *   138060  GEN6_GT_CORE_STATUS: render C-state, bits 2:0 (0 = RC0, 3 = RC6)
 *   A01C    RPSTAT1: actual frequency, bits 13:7 in 50 MHz
 *   20C8    R_PWR_CLK_STATE of this monitor's own context
 *
 * Usage: gpu_reg_monitor.exe <duration ms> <interval ms> <hex offset> [...] */
#define NOMINMAX
#include "hasvk_wddm.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

using namespace hasvk::wddm;

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: gpu_reg_monitor.exe <duration ms> <interval ms> <hex offset> [...]\n"); return 2; }
   const int duration = atoi(argv[1]), interval = atoi(argv[2]);
   std::vector<uint32_t> offsets;
   for (int i = 3; i < argc && offsets.size() < 16; ++i) offsets.push_back(strtoul(argv[i], nullptr, 16));
   try {
      Device device;
      Buffer batch(device, linear_metadata(8192), 8192), results(device, linear_metadata(4096), 4096);
      const uint64_t va = results.address() & 0xffffffffffffull;
      std::vector<uint32_t> cmds;
      for (size_t i = 0; i < offsets.size(); ++i) {
         const uint64_t target = va + i * 4;
         cmds.insert(cmds.end(), {0x12000002 /* MI_STORE_REGISTER_MEM */, offsets[i], (uint32_t)target, (uint32_t)(target >> 32) & 0xffff});
      }
      cmds.push_back(0x05000000 /* MI_BATCH_BUFFER_END */);
      if (cmds.size() & 1) cmds.push_back(0);
      memcpy(batch.cpu(), cmds.data(), cmds.size() * 4);
      volatile uint32_t *r = static_cast<uint32_t *>(results.cpu());
      std::vector<uint32_t> last(offsets.size(), 0xfffffffeu);
      std::vector<std::map<uint32_t, unsigned>> histogram(offsets.size());
      unsigned samples = 0;
      const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
      while (std::chrono::steady_clock::now() < end) {
         for (size_t i = 0; i < offsets.size(); ++i) r[i] = 0xffffffffu;
         device.submit_and_wait(batch.address(), (uint32_t)(cmds.size() * 4), command_header_8192());
         SYSTEMTIME now; GetLocalTime(&now);
         for (size_t i = 0; i < offsets.size(); ++i) {
            const uint32_t v = r[i];
            ++histogram[i][v];
            if (v != last[i]) {
               printf("%02u:%02u:%02u.%03u  %06x: %08x -> %08x\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, offsets[i], last[i], v);
               last[i] = v;
            }
         }
         ++samples;
         if (interval) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      }
      printf("samples %u\n", samples);
      for (size_t i = 0; i < offsets.size(); ++i) {
         printf("  %06x:", offsets[i]);
         unsigned shown = 0;
         for (const auto &[value, n] : histogram[i]) { if (shown++ < 12) printf(" %08x x%u", value, n); }
         printf("%s\n", histogram[i].size() > 12 ? " ..." : "");
      }
      fflush(stdout);
   } catch (const std::exception &e) {
      fprintf(stderr, "error: %s\n", e.what());
      return 1;
   }
   return 0;
}
