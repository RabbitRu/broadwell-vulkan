/* depth_fill_probe's load on Intel's own D3D11 driver: D16 1536x1536,
 * <layers> full-screen triangles back to front. Pixel shader: none, empty,
 * or an alpha test (one sample + discard).
 * [order] 1 = front to back (only the first layer passes); PS invocations
 * are reported (pipeline statistics).
 * Usage: d3d11_depth_fill_probe.exe none|empty|discard [reps] [layers] [order] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char kVs[] = R"(
struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
cbuffer Order : register(b0) { int front_to_back; int3 pad; };
Out main(uint v : SV_VertexID, uint i : SV_InstanceID)
{
   float2 p = float2((v << 1) & 2, v & 2);
   Out o; o.uv = p;
   o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, front_to_back ? 0.1 + i * 0.01 : 0.9 - i * 0.01, 1);
   return o;
}
)";
static const char kEmpty[] = "void main() {}";
static const char kDiscard[] = R"(
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
void main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
   if (tex.Sample(smp, uv * 4).a < 0.5) discard;
}
)";

#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("%s = 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

static ID3DBlob *compile(const char *src, const char *target)
{
   ID3DBlob *blob = nullptr, *errors = nullptr;
   if (FAILED(D3DCompile(src, strlen(src), "probe", nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
      printf("%s\n", errors ? (const char *)errors->GetBufferPointer() : "compile failed");
   return blob;
}

int main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "none";
   const int reps = argc > 2 ? atoi(argv[2]) : 200;
   const UINT layers = argc > 3 ? atoi(argv[3]) : 8, size = 1536;
   const int front_to_back = argc > 4 ? atoi(argv[4]) : 0;
   ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
   ID3DBlob *vsb = compile(kVs, "vs_5_0"); if (!vsb) return 1;
   ID3D11VertexShader *vs; CHECK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs));
   ID3D11PixelShader *pixel = nullptr;
   if (strcmp(mode, "none")) {
      ID3DBlob *psb = compile(strcmp(mode, "empty") ? kDiscard : kEmpty, "ps_5_0"); if (!psb) return 1;
      CHECK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &pixel));
   }
   D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = size; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_TYPELESS;
   td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
   ID3D11Texture2D *depth; CHECK(dev->CreateTexture2D(&td, nullptr, &depth));
   D3D11_DEPTH_STENCIL_VIEW_DESC dvd{}; dvd.Format = DXGI_FORMAT_D16_UNORM; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
   ID3D11DepthStencilView *dsv; CHECK(dev->CreateDepthStencilView(depth, &dvd, &dsv));
   std::vector<uint32_t> texels(512 * 512, 0xff000080u);
   D3D11_TEXTURE2D_DESC cd{}; cd.Width = cd.Height = 512; cd.MipLevels = cd.ArraySize = 1; cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   cd.SampleDesc.Count = 1; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   D3D11_SUBRESOURCE_DATA init{texels.data(), 2048, 0};
   ID3D11Texture2D *texture; CHECK(dev->CreateTexture2D(&cd, &init, &texture));
   ID3D11ShaderResourceView *srv; CHECK(dev->CreateShaderResourceView(texture, nullptr, &srv));
   D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX;
   ID3D11SamplerState *sampler; CHECK(dev->CreateSamplerState(&sd, &sampler));
   D3D11_DEPTH_STENCIL_DESC dsd{}; dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
   ID3D11DepthStencilState *dss; CHECK(dev->CreateDepthStencilState(&dsd, &dss));
   D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
   ID3D11RasterizerState *rs; CHECK(dev->CreateRasterizerState(&rd, &rs));
   D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT}; ID3D11Query *disjoint; CHECK(dev->CreateQuery(&qd, &disjoint));
   qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0, *t1; CHECK(dev->CreateQuery(&qd, &t0)); CHECK(dev->CreateQuery(&qd, &t1));
   const int order[4] = {front_to_back, 0, 0, 0};
   D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA cinit{order, 0, 0}; ID3D11Buffer *cb; CHECK(dev->CreateBuffer(&cbd, &cinit, &cb));
   qd.Query = D3D11_QUERY_PIPELINE_STATISTICS; ID3D11Query *stats; CHECK(dev->CreateQuery(&qd, &stats));
   D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
   D3D11_VIEWPORT vp{0, 0, (float)size, (float)size, 0, 1};
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(pixel, nullptr, 0); ctx->VSSetConstantBuffers(0, 1, &cb);
   ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &sampler);
   ctx->OMSetRenderTargets(0, nullptr, dsv); ctx->OMSetDepthStencilState(dss, 0);
   ctx->RSSetState(rs); ctx->RSSetViewports(1, &vp);
   double best = 1e9;
   for (int rep = 0; rep < reps; ++rep) {
      ctx->Begin(disjoint); ctx->End(t0);
      ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
      ctx->Begin(stats);
      ctx->DrawInstanced(3, layers, 0, 0);
      ctx->End(stats);
      ctx->End(t1); ctx->End(disjoint);
      while (ctx->GetData(stats, &ps, sizeof(ps), 0) != S_OK) {}
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
      while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) {}
      while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) {}
      if (dj.Disjoint) continue;
      const double ms = (b - a) * 1000.0 / dj.Frequency;
      if (ms < best) best = ms;
   }
   printf("d3d11 %s %s: best %.3f ms for %u layers -> %.2f Gpixels/s, %.2fM PS invocations\n", mode, front_to_back ? "front-to-back" : "back-to-front",
          best, layers, (double)size * size * layers / (best / 1000) / 1e9, ps.PSInvocations / 1e6);
   return 0;
}
