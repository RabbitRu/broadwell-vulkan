/* sample.comp's load on Intel's own D3D11 driver, for comparison with
 * sample_probe: 1024x1024 dispatch, 16 bilinear RGBA8 samples per thread.
 * Usage: d3d11_sample_probe.exe [reps] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static const char kShader[] = R"(
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
RWStructuredBuffer<float4> v : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
   float2 uv = (float2(id.xy) + 0.5) / 1024.0;
   float4 sum = 0;
   [loop] for (int i = 0; i < 16; ++i)
      sum += tex.SampleLevel(smp, uv + float2(i & 3, i >> 2) * (0.37 / 1024.0), 0.0);
   v[(id.y * 1024 + id.x) & 1023] = sum;
}
)";

#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("%s = 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

int main(int argc, char **argv)
{
   const int reps = argc > 1 ? atoi(argv[1]) : 200;
   ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
   ID3DBlob *blob, *errors = nullptr;
   if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "sample", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                         &blob, &errors))) {
      printf("%s\n", errors ? (const char *)errors->GetBufferPointer() : "compile failed");
      return 1;
   }
   ID3D11ComputeShader *cs; CHECK(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs));
   std::vector<uint32_t> texels(1024 * 1024, 0xffbf7f40u);
   D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 1024; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   D3D11_SUBRESOURCE_DATA init{texels.data(), 4096, 0};
   ID3D11Texture2D *texture; CHECK(dev->CreateTexture2D(&td, &init, &texture));
   ID3D11ShaderResourceView *srv; CHECK(dev->CreateShaderResourceView(texture, nullptr, &srv));
   D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX;
   ID3D11SamplerState *sampler; CHECK(dev->CreateSamplerState(&sd, &sampler));
   D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16384; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
   bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
   ID3D11Buffer *buffer; CHECK(dev->CreateBuffer(&bd, nullptr, &buffer));
   ID3D11UnorderedAccessView *uav; CHECK(dev->CreateUnorderedAccessView(buffer, nullptr, &uav));
   D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT}; ID3D11Query *disjoint; CHECK(dev->CreateQuery(&qd, &disjoint));
   qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0, *t1; CHECK(dev->CreateQuery(&qd, &t0)); CHECK(dev->CreateQuery(&qd, &t1));
   ctx->CSSetShader(cs, nullptr, 0); ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
   ctx->CSSetShaderResources(0, 1, &srv); ctx->CSSetSamplers(0, 1, &sampler);
   for (int rep = 0; rep < reps; ++rep) {
      ctx->Begin(disjoint); ctx->End(t0);
      ctx->Dispatch(128, 128, 1);
      ctx->End(t1); ctx->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
      while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) {}
      while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) {}
      const double ms = (b - a) * 1000.0 / dj.Frequency;
      if (reps <= 20 || rep % (reps / 20) == 0)
         printf("run %d: %.3f ms -> %.2f Gsamples/s%s\n", rep, ms, 1024.0 * 1024 * 16 / (ms / 1000) / 1e9, dj.Disjoint ? " (disjoint)" : "");
   }
   return 0;
}
