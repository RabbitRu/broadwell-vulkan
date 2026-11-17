/* Risk check (roadmap stage 0): which MI commands and MMIO registers that
 * HasVK emits work in our WDDM context (an unprivileged PPGTT batch of the
 * Intel KMD 20.19.15.5171). MI_SEMAPHORE_WAIT is known not to complete and is
 * not run here.
 *
 * Register test: MI_LOAD_REGISTER_IMM two values and MI_LOAD_REGISTER_MEM a
 * third, MI_STORE_REGISTER_MEM after each; "writable" means all three read
 * back, "ignored" means the writes had no effect, "rejected" means the kernel
 * or GPU refused the batch (the context is lost; each test has its own
 * device). Commands are checked by their memory results.
 *
 * Usage: KernelLimitsProbe.exe <report.json> */
#define NOMINMAX
#include "hasvk_wddm.hpp"
#include <cstdio>
#include <functional>
#include <string>

using namespace hasvk::wddm;

namespace {
constexpr uint32_t LRI = 0x11000001, SRM = 0x12000002, LRM = 0x14800002, LRR = 0x15000001,
   SDI_QWORD = 0x10200003, COPY_MEM = 0x17000003, BBE = 0x05000000, NOOP = 0;
uint32_t lo(uint64_t a) { return (uint32_t)a; }
uint32_t hi(uint64_t a) { return (uint32_t)(a >> 32) & 0xffff; }

struct Context {
   Device device;
   Buffer batch, results, aux;
   std::vector<uint32_t> cmds;
   Context() : batch(device, linear_metadata(8192), 8192), results(device, linear_metadata(4096), 4096),
               aux(device, linear_metadata(65536), 65536) {
      auto *r = static_cast<uint32_t *>(results.cpu());
      for (unsigned i = 0; i < 1024; ++i) r[i] = 0xdeadbeef;
      memset(aux.cpu(), 0, 65536);
   }
   uint64_t result_va(unsigned dword) const { return (results.address() & 0xffffffffffffull) + 4ull * dword; }
   uint64_t aux_va(unsigned byte) const { return (aux.address() & 0xffffffffffffull) + byte; }
   uint32_t result(unsigned dword) const { return static_cast<const uint32_t *>(results.cpu())[dword]; }
   void lri(uint32_t reg, uint32_t value) { cmds.insert(cmds.end(), {LRI, reg, value}); }
   void srm(uint32_t reg, unsigned dword) { const uint64_t a = result_va(dword); cmds.insert(cmds.end(), {SRM, reg, lo(a), hi(a)}); }
   void lrm(uint32_t reg, uint64_t address) { cmds.insert(cmds.end(), {LRM, reg, lo(address), hi(address)}); }
   void stall() { cmds.insert(cmds.end(), {0x7a000004, 0x00100000 | 0x20 /* CS stall | DC flush */, 0, 0, 0, 0}); }
   void run() {
      cmds.push_back(BBE); if (cmds.size() & 1) cmds.push_back(NOOP);
      if (cmds.size() * 4 > 8176) throw std::runtime_error("test batch too large");
      memcpy(batch.cpu(), cmds.data(), cmds.size() * 4);
      device.submit_and_wait(batch.address(), (uint32_t)(cmds.size() * 4), command_header_8192());
   }
};

std::string hex(uint32_t v) { char s[16]; snprintf(s, sizeof(s), "0x%08x", v); return s; }
struct Outcome { std::string verdict, detail; };
FILE *report;
unsigned blocked_count;
void record(const char *kind, const char *name, const Outcome &o, bool first)
{
   printf("%-8s %-32s %-9s %s\n", kind, name, o.verdict.c_str(), o.detail.c_str());
   fprintf(report, "%s    {\"kind\": \"%s\", \"name\": \"%s\", \"verdict\": \"%s\", \"detail\": \"%s\"}", first ? "" : ",\n", kind, name,
           o.verdict.c_str(), o.detail.c_str());
   if (o.verdict != "works" && o.verdict != "writable" && o.verdict != "readable") ++blocked_count;
}
Outcome guarded(const std::function<Outcome(Context &)> &test)
{
   try { Context c; return test(c); }
   catch (const std::exception &e) { return {"rejected", e.what()}; }
}
/* LRI v1, SRM; LRI v2, SRM; LRM v3, SRM. mask: implemented bits. */
Outcome register_rw(uint32_t reg, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t mask = ~0u)
{
   return guarded([&](Context &c) {
      memcpy(static_cast<uint8_t *>(c.aux.cpu()) + 256, &v3, 4);
      c.srm(reg, 0); c.lri(reg, v1); c.srm(reg, 1); c.lri(reg, v2); c.srm(reg, 2); c.lrm(reg, c.aux_va(256)); c.srm(reg, 3);
      c.run();
      const uint32_t before = c.result(0), r1 = c.result(1), r2 = c.result(2), r3 = c.result(3);
      const std::string detail = "before " + hex(before) + ", read " + hex(r1) + " " + hex(r2) + " " + hex(r3);
      if (r1 == 0xdeadbeef) return Outcome{"unreadable", detail};
      if ((r1 & mask) == (v1 & mask) && (r2 & mask) == (v2 & mask) && (r3 & mask) == (v3 & mask)) return Outcome{"writable", detail};
      if (r1 == before && r2 == before && r3 == before) return Outcome{"ignored", detail};
      return Outcome{"partial", detail};
   });
}
Outcome register_read(uint32_t reg)
{
   return guarded([&](Context &c) {
      c.srm(reg, 0); c.srm(reg, 1); c.run();
      return c.result(0) == 0xdeadbeef ? Outcome{"unreadable", ""} : Outcome{"readable", hex(c.result(0)) + " " + hex(c.result(1))};
   });
}
} // namespace

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: KernelLimitsProbe <report.json>\n"); return 2; }
   if (fopen_s(&report, argv[1], "w") || !report) return 3;
   fprintf(report, "{\n  \"driver\": \"20.19.15.5171\",\n  \"results\": [\n");
   bool first = true;
   auto reg = [&](const char *name, Outcome o) { record("register", name, o, first); first = false; };
   auto cmd = [&](const char *name, Outcome o) { record("command", name, o, first); first = false; };

   /* Registers HasVK writes (mi_builder, draw/dispatch indirect, predicates, transform feedback). */
   for (unsigned g = 0; g < 16; ++g) {
      const std::string name = "CS_GPR" + std::to_string(g);
      reg(name.c_str(), register_rw(0x2600 + 8 * g, 0x11110000 + g, 0x22220000 + g, 0x33330000 + g));
   }
   reg("CS_GPR0 high dword", register_rw(0x2604, 0x44440000, 0x55550000, 0x66660000));
   reg("MI_PREDICATE_SRC0", register_rw(0x2400, 0x12345678, 0x9abcdef0, 0x0badf00d));
   reg("MI_PREDICATE_SRC0 high", register_rw(0x2404, 0x1, 0x2, 0x3));
   reg("MI_PREDICATE_SRC1", register_rw(0x2408, 0x12345678, 0x9abcdef0, 0x0badf00d));
   reg("MI_PREDICATE_SRC1 high", register_rw(0x240c, 0x1, 0x2, 0x3));
   reg("MI_PREDICATE_DATA", register_rw(0x2410, 0x1, 0x2, 0x3));
   reg("3DPRIM_END_OFFSET", register_rw(0x2420, 0x100, 0x200, 0x300));
   reg("3DPRIM_START_VERTEX", register_rw(0x2430, 0x10, 0x20, 0x30));
   reg("3DPRIM_VERTEX_COUNT", register_rw(0x2434, 0x10, 0x20, 0x30));
   reg("3DPRIM_INSTANCE_COUNT", register_rw(0x2438, 0x10, 0x20, 0x30));
   reg("3DPRIM_START_INSTANCE", register_rw(0x243c, 0x10, 0x20, 0x30));
   reg("3DPRIM_BASE_VERTEX", register_rw(0x2440, 0x10, 0x20, 0x30));
   reg("GPGPU_DISPATCHDIMX", register_rw(0x2500, 0x10, 0x20, 0x30));
   reg("GPGPU_DISPATCHDIMY", register_rw(0x2504, 0x10, 0x20, 0x30));
   reg("GPGPU_DISPATCHDIMZ", register_rw(0x2508, 0x10, 0x20, 0x30));
   for (unsigned s = 0; s < 4; ++s) {
      const std::string name = "SO_WRITE_OFFSET" + std::to_string(s);
      reg(name.c_str(), register_rw(0x5280 + 4 * s, 0x100, 0x200, 0x300, ~3u));
   }
   reg("SO_NUM_PRIMS_WRITTEN0", register_rw(0x5200, 0x10, 0x20, 0x30));
   reg("SO_PRIM_STORAGE_NEEDED0", register_rw(0x5240, 0x10, 0x20, 0x30));
   /* CACHE_MODE_1 is masked: the high 16 bits select which low bits are written. */
   reg("CACHE_MODE_1 (masked)", register_rw(0x7004, 0x00400040, 0x00400000, 0x00400040, 0x40));
   reg("INSTPM (masked)", register_rw(0x20c0, 0x00400040, 0x00400000, 0x00400000, 0x40));
   /* Read-only counters used by queries. */
   reg("TIMESTAMP", register_read(0x2358));
   reg("IA_VERTICES_COUNT", register_read(0x2310));
   reg("PS_INVOCATION_COUNT", register_read(0x2348));
   reg("CL_INVOCATION_COUNT", register_read(0x2338));
   reg("CS_INVOCATION_COUNT", register_read(0x2290));

   /* L3CNTLREG: switch between two validated Broadwell configurations and back. */
   reg("L3CNTLREG", guarded([](Context &c) {
      c.srm(0x7034, 0); c.run();
      const uint32_t original = c.result(0);
      const uint32_t other = original == 0x60000060 ? 0x80000040 : 0x60000060;   // {URB48,ALL48} / {URB32,ALL64}
      c.cmds.clear(); c.stall(); c.lri(0x7034, other); c.stall(); c.srm(0x7034, 1); c.lri(0x7034, original); c.stall(); c.srm(0x7034, 2);
      c.run();
      const std::string detail = "original " + hex(original) + ", after write " + hex(c.result(1)) + ", restored " + hex(c.result(2));
      return Outcome{c.result(1) == other && c.result(2) == original ? "writable" : c.result(1) == original ? "ignored" : "partial", detail};
   }));

   /* Commands. */
   cmd("MI_LOAD_REGISTER_REG", guarded([](Context &c) {
      c.lri(0x2600, 0xcafe0001); c.lri(0x2608, 0); c.cmds.insert(c.cmds.end(), {LRR, 0x2600, 0x2608}); c.srm(0x2608, 0); c.run();
      return c.result(0) == 0xcafe0001 ? Outcome{"works", ""} : Outcome{"broken", hex(c.result(0))};
   }));
   cmd("MI_MATH (ADD SUB AND OR XOR)", guarded([](Context &c) {
      for (uint32_t g : {0x2600u, 0x2604u, 0x2608u, 0x260cu}) c.lri(g, 0);
      c.lri(0x2600, 1000); c.lri(0x2608, 300);
      const uint32_t ops[5] = {0x100, 0x101, 0x102, 0x103, 0x104};
      for (unsigned i = 0; i < 5; ++i) {
         /* LOAD SRCA R0; LOAD SRCB R1; <op>; STORE R2 ACCU */
         c.cmds.insert(c.cmds.end(), {0x0d000003, (0x080u << 20) | (0x20 << 10) | 0, (0x080u << 20) | (0x21 << 10) | 1, ops[i] << 20,
                                      (0x180u << 20) | (2 << 10) | 0x31});
         c.srm(0x2610, i);
      }
      c.run();
      const uint32_t expected[5] = {1300, 700, 1000 & 300, 1000 | 300, 1000 ^ 300};
      std::string detail; bool ok = true;
      for (unsigned i = 0; i < 5; ++i) { detail += hex(c.result(i)) + " "; ok &= c.result(i) == expected[i]; }
      return Outcome{ok ? "works" : "broken", detail};
   }));
   cmd("MI_PREDICATE (SRCS_EQUAL)", guarded([](Context &c) {
      /* equal sources -> result 1, different -> 0; MI_PREDICATE_RESULT is 0x2418. */
      c.lri(0x2400, 7); c.lri(0x2404, 0); c.lri(0x2408, 7); c.lri(0x240c, 0);
      c.cmds.push_back(0x06000082); c.srm(0x2418, 0);
      c.lri(0x2408, 8); c.cmds.push_back(0x06000082); c.srm(0x2418, 1);
      c.run();
      return c.result(0) == 1 && c.result(1) == 0 ? Outcome{"works", ""} : Outcome{"broken", hex(c.result(0)) + " " + hex(c.result(1))};
   }));
   cmd("MI_COPY_MEM_MEM", guarded([](Context &c) {
      const uint32_t v = 0x600df00d; memcpy(static_cast<uint8_t *>(c.aux.cpu()) + 64, &v, 4);
      const uint64_t d = c.result_va(0), s = c.aux_va(64);
      c.cmds.insert(c.cmds.end(), {COPY_MEM, lo(d), hi(d), lo(s), hi(s)}); c.run();
      return c.result(0) == v ? Outcome{"works", ""} : Outcome{"broken", hex(c.result(0))};
   }));
   cmd("MI_STORE_DATA_IMM (qword)", guarded([](Context &c) {
      const uint64_t d = c.result_va(0); c.cmds.insert(c.cmds.end(), {SDI_QWORD, lo(d), hi(d), 0x11112222, 0x33334444}); c.run();
      return c.result(0) == 0x11112222 && c.result(1) == 0x33334444 ? Outcome{"works", ""} : Outcome{"broken", hex(c.result(0))};
   }));
   cmd("MI_REPORT_PERF_COUNT", guarded([](Context &c) {
      /* 256-byte OA report at a 64-byte aligned address; dword 0 is the report id. */
      const uint64_t d = c.aux_va(1024); c.cmds.insert(c.cmds.end(), {0x14000002, lo(d), hi(d), 0x5a5a0001}); c.stall(); c.run();
      const uint32_t *r = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(c.aux.cpu()) + 1024);
      unsigned nonzero = 0; for (unsigned i = 0; i < 64; ++i) nonzero += r[i] != 0;
      return Outcome{nonzero ? "works" : "ignored", "report dwords nonzero: " + std::to_string(nonzero) + ", id " + hex(r[0])};
   }));
   cmd("MI_BATCH_BUFFER_START 2nd level", guarded([](Context &c) {
      /* A second-level batch stores 1 and returns; the store after the call proves the return. */
      uint32_t *second = static_cast<uint32_t *>(c.aux.cpu()) + 4096 / 4;
      const uint64_t r0 = c.result_va(0), r1 = c.result_va(2), sva = c.aux_va(4096);
      const uint32_t sub[] = {SDI_QWORD, lo(r0), hi(r0), 1, 0, BBE};
      memcpy(second, sub, sizeof(sub));
      c.cmds.insert(c.cmds.end(), {0x18c00101, lo(sva), hi(sva), NOOP, SDI_QWORD, lo(r1), hi(r1), 2, 0});
      c.run();
      return c.result(0) == 1 && c.result(2) == 2 ? Outcome{"works", ""} : Outcome{"broken", hex(c.result(0)) + " " + hex(c.result(2))};
   }));
   cmd("self-modifying batch", guarded([](Context &c) {
      /* mi_builder's pattern: write a later dword of the running batch, then
       * CS stall and enough NOOPs to drain the command prefetch. */
      /* [0] MI_STORE_DATA_IMM (dword) patching the immediate of the store at [sdi] */
      c.cmds.insert(c.cmds.end(), {0x10000002, 0, 0, 0xfeed0001});
      c.stall();
      for (int i = 0; i < 128; ++i) c.cmds.push_back(NOOP);
      const size_t sdi = c.cmds.size();
      const uint64_t r = c.result_va(0);
      c.cmds.insert(c.cmds.end(), {0x10000002, lo(r), hi(r), 0xdead0000});
      const uint64_t target = (c.batch.address() & 0xffffffffffffull) + 4 * (sdi + 3);
      c.cmds[1] = lo(target); c.cmds[2] = hi(target);
      c.run();
      return c.result(0) == 0xfeed0001 ? Outcome{"works", ""} : Outcome{c.result(0) == 0xdead0000 ? "stale" : "broken", hex(c.result(0))};
   }));

   fprintf(report, "\n  ],\n  \"notWorking\": %u\n}\n", blocked_count);
   fclose(report);
   printf("%u checks not working\n", blocked_count);
   return 0;
}
