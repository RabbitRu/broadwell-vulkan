/* vs_fetch.vert's load on Intel's own D3D11 driver: 3*2^20 clipped vertices,
 * a VS with <count> iterations of 3 Buffer<float4>.Load (or ALU only).
 * Usage: d3d11_vs_fetch_probe.exe [iterations] [mode 0=loads 1=ALU] [draws] [vertices per draw] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "kernel_dump.h"
#include <cstring>
#include <vector>

static const char kVs[] = R"(
Buffer<float4> rows : register(t0);
cbuffer Push : register(b0) { int count; int mode; int2 pad; };
float4 main(uint v : SV_VertexID) : SV_Position
{
   float4 p = float4((v & 1023) * 1e-3, (v >> 10) * 1e-3, 0.5, 1.0);
   float4 acc = 0;
   const uint base = (v * 7) & 1023;
   [loop] for (int i = 0; i < count; ++i) {
      if (mode == 0) {
         float4 r0 = rows.Load((base + i * 3 + 0) & 4095);
         float4 r1 = rows.Load((base + i * 3 + 1) & 4095);
         float4 r2 = rows.Load((base + i * 3 + 2) & 4095);
         acc += float4(dot(r0, p), dot(r1, p), dot(r2, p), 0);
      } else {
         acc += float4(p.x * i, p.y + i, p.z * 0.5, 0);
      }
   }
   return float4(3.0 + acc.x * 1e-6, 3.0 + acc.y * 1e-6, acc.z * 1e-6, 1.0);
}
)";

#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("%s = 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

int main(int argc, char **argv)
{
   const int count = argc > 1 ? atoi(argv[1]) : 4, mode = argc > 2 ? atoi(argv[2]) : 0;
   const UINT draws = argc > 3 ? atoi(argv[3]) : 1, per_draw = argc > 4 ? atoi(argv[4]) : 3u << 20;
   ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
   ID3DBlob *blob = nullptr, *errors = nullptr;
   if (FAILED(D3DCompile(kVs, sizeof(kVs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
      printf("%s\n", errors ? (const char *)errors->GetBufferPointer() : "compile failed"); return 1;
   }
   ID3D11VertexShader *vs; CHECK(dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs));
   std::vector<float> texels(4096 * 4); for (size_t i = 0; i < texels.size(); ++i) texels[i] = (float)(i % 97) * 0.01f;
   D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 4096 * 16; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   D3D11_SUBRESOURCE_DATA init{texels.data(), 0, 0};
   ID3D11Buffer *buffer; CHECK(dev->CreateBuffer(&bd, &init, &buffer));
   D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
   sd.Buffer.NumElements = 4096;
   ID3D11ShaderResourceView *srv; CHECK(dev->CreateShaderResourceView(buffer, &sd, &srv));
   const int32_t push[4] = {count, mode, 0, 0};
   D3D11_BUFFER_DESC cd{}; cd.ByteWidth = 16; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA cinit{push, 0, 0};
   ID3D11Buffer *cbuf; CHECK(dev->CreateBuffer(&cd, &cinit, &cbuf));
   D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT}; ID3D11Query *disjoint; CHECK(dev->CreateQuery(&qd, &disjoint));
   qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0, *t1; CHECK(dev->CreateQuery(&qd, &t0)); CHECK(dev->CreateQuery(&qd, &t1));
   D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ctx->VSSetShader(vs, nullptr, 0); ctx->VSSetShaderResources(0, 1, &srv); ctx->VSSetConstantBuffers(0, 1, &cbuf);
   ctx->PSSetShader(nullptr, nullptr, 0); ctx->RSSetViewports(1, &vp);
   const UINT vertices = draws * per_draw;
   double best = 1e9;
   for (int rep = 0; rep < 60; ++rep) {
      ctx->Begin(disjoint); ctx->End(t0);
      for (UINT i = 0; i < draws; ++i) ctx->Draw(per_draw, 0);
      ctx->End(t1); ctx->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
      while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) {}
      while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) {}
      if (dj.Disjoint) continue;
      const double ms = (b - a) * 1000.0 / dj.Frequency;
      if (ms < best) best = ms;
   }
   printf("d3d11 %s x%d: best %.3f ms for %u draws, %u vertices -> %.1f Mvertices/s, %.2f us per draw\n", mode ? "ALU" : "Load", count, best,
          draws, vertices, vertices / (best / 1000) / 1e6, best * 1000 / draws);
   const uint32_t constants[] = {0x358637bd /* 1e-6 */, 0x3a83126f /* 1e-3 */};
   DumpKernelCandidates(constants, 2, 2048);
   return 0;
}
