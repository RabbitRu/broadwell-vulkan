/* GPU frequency monitor (stage 7, F4/A5): while another process renders,
 * submits a tiny batch every <interval> ms that stores RPSTAT1 (0xA01C, the
 * actual GT frequency, CAGF in bits 13:7, 50 MHz units on Broadwell) with
 * MI_STORE_REGISTER_MEM. Only reads a register; changes no GPU state.
 *
 * Prints a histogram of the frequencies seen and the mean over samples taken
 * while the GPU was above its idle frequency.
 *
 * Usage: GpuFreqMonitor.exe <duration ms> [interval ms = 2] */
#define NOMINMAX
#include "hasvk_wddm.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

using namespace hasvk::wddm;

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: GpuFreqMonitor.exe <duration ms> [interval ms]\n"); return 2; }
   const int duration = atoi(argv[1]), interval = argc > 2 ? atoi(argv[2]) : 2;
   try {
      Device device;
      Buffer batch(device, linear_metadata(8192), 8192), results(device, linear_metadata(4096), 4096);
      const uint64_t va = results.address() & 0xffffffffffffull;
      const uint32_t cmds[] = {0x12000002 /* MI_STORE_REGISTER_MEM */, 0xA01C, (uint32_t)va, (uint32_t)(va >> 32) & 0xffff,
                               0x05000000 /* MI_BATCH_BUFFER_END */, 0};
      memcpy(batch.cpu(), cmds, sizeof(cmds));
      volatile uint32_t *r = static_cast<uint32_t *>(results.cpu());
      std::map<unsigned, unsigned> histogram;
      unsigned samples = 0, busy = 0;
      double busy_sum = 0;
      const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
      while (std::chrono::steady_clock::now() < end) {
         r[0] = 0xffffffffu;
         device.submit_and_wait(batch.address(), sizeof(cmds), command_header_8192());
         const uint32_t v = r[0];
         if (v == 0xffffffffu) { fprintf(stderr, "register store did not land\n"); return 1; }
         const unsigned mhz = ((v >> 7) & 0x7f) * 50;
         ++histogram[mhz]; ++samples;
         if (mhz > 300) { ++busy; busy_sum += mhz; }
         std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      }
      printf("samples %u; above 300 MHz: %u, mean %.0f MHz\n", samples, busy, busy ? busy_sum / busy : 0.0);
      for (const auto &[mhz, n] : histogram) printf("  %4u MHz: %u\n", mhz, n);
   } catch (const std::exception &e) {
      fprintf(stderr, "error: %s\n", e.what());
      return 1;
   }
   return 0;
}
