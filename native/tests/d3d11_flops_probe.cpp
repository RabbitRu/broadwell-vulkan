/* The flops.comp load through Intel's own D3D11 driver, for comparing GPU
 * clock behaviour with this ICD (flops_probe). Same work per dispatch, same
 * submit-and-wait pattern, timed by D3D11 timestamp queries.
 * Usage: d3d11_flops_probe.exe [reps] [dispatches per submit] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "kernel_dump.h"

static const char kShader[] = R"(
RWStructuredBuffer<float> v : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
   float a = float(id.x) * 1e-7, b = a + 0.1, c = a + 0.2, d = a + 0.3;
   float e = a + 0.4, f = a + 0.5, g = a + 0.6, h = a + 0.7;
   const float m = 0.9999, k = 0.0001;
   [loop] for (uint i = 0; i < 512; ++i) {
      a = mad(a, m, k); b = mad(b, m, k); c = mad(c, m, k); d = mad(d, m, k);
      e = mad(e, m, k); f = mad(f, m, k); g = mad(g, m, k); h = mad(h, m, k);
   }
   v[id.x & 1023] = a + b + c + d + e + f + g + h;
}
)";

#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("%s = 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

int main(int argc, char **argv)
{
   const int reps = argc > 1 ? atoi(argv[1]) : 6;
   const int per_submit = argc > 2 ? atoi(argv[2]) : 1;
   ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
   ID3DBlob *blob, *errors = nullptr;
   if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "flops", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         &blob, &errors))) {
      printf("%s\n", errors ? (const char *)errors->GetBufferPointer() : "compile failed");
      return 1;
   }
   ID3D11ComputeShader *cs; CHECK(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs));
   D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 4096; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
   bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
   ID3D11Buffer *buffer; CHECK(dev->CreateBuffer(&bd, nullptr, &buffer));
   ID3D11UnorderedAccessView *uav; CHECK(dev->CreateUnorderedAccessView(buffer, nullptr, &uav));
   D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT}; ID3D11Query *disjoint; CHECK(dev->CreateQuery(&qd, &disjoint));
   qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0, *t1; CHECK(dev->CreateQuery(&qd, &t0)); CHECK(dev->CreateQuery(&qd, &t1));
   ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
   const uint32_t groups = 4096;
   for (int rep = 0; rep < reps; ++rep) {
      ctx->Begin(disjoint); ctx->End(t0);
      for (int i = 0; i < per_submit; ++i) ctx->Dispatch(groups, 1, 1);
      ctx->End(t1); ctx->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
      while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) {}
      while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) {}
      const double ms = (b - a) * 1000.0 / dj.Frequency / per_submit;
      const double flop = groups * 64.0 * 512 * 8 * 2;
      if (reps <= 20 || rep % (reps / 20) == 0)
         printf("run %d: %.2f ms -> %.0f GFLOPS%s\n", rep, ms, flop / (ms / 1000) / 1e9, dj.Disjoint ? " (disjoint)" : "");
   }
   const uint32_t constants[] = {0x3f7ff972 /* 0.9999 */, 0x38d1b717 /* 0.0001 */};
   DumpKernelCandidates(constants, 2);
   return 0;
}
