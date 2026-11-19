/* The captured Talos foliage draw (see replay_probe.cpp) on Intel's own D3D11
 * driver, with the trivial vertex shaders of replay_trivial.vert /
 * replay_t4: the same index and vertex data, instance rows, view-projection
 * constants, D16 1536x1536 target, GREATER_EQUAL depth test with writes and
 * no culling. Gives the fixed-function floor of the same geometry on the
 * same GPU for comparison with our Vulkan driver.
 * Usage: d3d11_replay_probe.exe <capture dir> [repeats, default 20]
 *        [elements 4|8, default 4] [options: s = 1x1 scissor, c = cull back] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static const char kVs4[] = R"(
cbuffer U : register(b0) { float4 vp[4]; };
struct In { float3 a0 : POSITION; float4 r0 : ROW0; float4 r1 : ROW1; float4 r2 : ROW2; };
float4 main(In i, out float4 v0 : TEXCOORD0) : SV_Position
{
   float3 p = i.a0; float w4 = 1.0;
   float3 w = float3(dot(i.r0.xyz, p) + i.r0.w * w4, dot(i.r1.xyz, p) + i.r1.w * w4, dot(i.r2.xyz, p) + i.r2.w * w4);
   v0 = 0;
   return float4(dot(vp[0].xyz, w) + vp[0].w * w4, dot(vp[1].xyz, w) + vp[1].w * w4,
                 dot(vp[2].xyz, w) + vp[2].w * w4, dot(vp[3].xyz, w) + vp[3].w * w4);
}
)";
static const char kVs8[] = R"(
cbuffer U : register(b0) { float4 vp[4]; };
struct In { float3 a0 : POSITION; float2 a1 : TEXCOORD0; float a2 : TEXCOORD1; float4 a3 : COLOR;
            float4 r0 : ROW0; float4 r1 : ROW1; float4 r2 : ROW2; float4 a10 : TEXCOORD2; };
float4 main(In i, out float4 v0 : TEXCOORD0) : SV_Position
{
   float3 p = i.a0 + (i.a3.xyz + float3(i.a2, 0, 0)) * 1e-9; float w4 = 1.0;
   float3 w = float3(dot(i.r0.xyz, p) + i.r0.w * w4, dot(i.r1.xyz, p) + i.r1.w * w4, dot(i.r2.xyz, p) + i.r2.w * w4);
   v0 = float4(i.a1, i.a10.w, 0);
   return float4(dot(vp[0].xyz, w) + vp[0].w * w4, dot(vp[1].xyz, w) + vp[1].w * w4,
                 dot(vp[2].xyz, w) + vp[2].w * w4, dot(vp[3].xyz, w) + vp[3].w * w4);
}
)";

#define CHECK(x) do { HRESULT h_ = (x); if (FAILED(h_)) { printf("%s = 0x%08lx\n", #x, (unsigned long)h_); return 1; } } while (0)

static std::vector<char> load(const std::string &path)
{
   std::ifstream in(path, std::ios::binary);
   return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv)
{
   if (argc < 2) { printf("usage: d3d11_replay_probe <capture dir> [repeats] [4|8] [s][c]\n"); return 2; }
   const std::string dir = argv[1];
   const int repeats = argc > 2 ? atoi(argv[2]) : 20;
   const int elements = argc > 3 ? atoi(argv[3]) : 4;
   const char *opts = argc > 4 ? argv[4] : "";
   std::ifstream txt(dir + "\\capture.txt");
   if (!txt) { printf("no capture.txt\n"); return 2; }
   uint32_t count = 0, instances = 1, first = 0, first_instance = 0, indexed = 0; int32_t vertex_offset = 0;
   std::map<uint32_t, std::string> vbufs; std::map<uint32_t, uint32_t> strides; std::string ubo;
   for (std::string line; std::getline(txt, line);) {
      std::istringstream s(line); std::string key; s >> key;
      if (key == "draw") s >> indexed >> count >> instances >> first >> vertex_offset >> first_instance;
      else if (key == "vbuf") { uint32_t b; std::string f; s >> b >> f; vbufs[b] = f; }
      else if (key == "vb_binding") { uint32_t b, stride; s >> b >> stride; strides[b] = stride; }
      else if (key == "desc") { uint32_t set, binding; std::string w, f; s >> set >> binding >> w >> w >> f; if (set == 0 && binding == 0) ubo = f; }
   }
   if (!indexed || !count) { printf("capture is not an indexed draw\n"); return 2; }

   ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx));
   ID3DBlob *blob = nullptr, *errors = nullptr;
   const char *src = elements == 8 ? kVs8 : kVs4;
   if (FAILED(D3DCompile(src, strlen(src), "vs", nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
      printf("%s\n", errors ? (const char *)errors->GetBufferPointer() : "compile failed"); return 1;
   }
   ID3D11VertexShader *vs; CHECK(dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs));
   std::vector<D3D11_INPUT_ELEMENT_DESC> layout = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"ROW", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"ROW", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
      {"ROW", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
   };
   if (elements == 8) {
      layout.push_back({"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
      layout.push_back({"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
      layout.push_back({"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
      layout.push_back({"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1});
   }
   ID3D11InputLayout *il; CHECK(dev->CreateInputLayout(layout.data(), (UINT)layout.size(), blob->GetBufferPointer(), blob->GetBufferSize(), &il));

   ID3D11Buffer *vb[5] = {}; UINT vstrides[5] = {}, voffsets[5] = {};
   for (uint32_t b = 0; b < 5; ++b) {
      std::vector<char> data = load(dir + "\\" + vbufs[b]);
      D3D11_BUFFER_DESC bd{}; bd.ByteWidth = (UINT)((data.size() + 15) & ~size_t(15)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      data.resize(bd.ByteWidth);
      D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
      CHECK(dev->CreateBuffer(&bd, &init, &vb[b])); vstrides[b] = strides[b];
   }
   std::vector<char> indices = load(dir + "\\index.bin");
   D3D11_BUFFER_DESC ibd{}; ibd.ByteWidth = (UINT)((indices.size() + 15) & ~size_t(15)); ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
   indices.resize(ibd.ByteWidth);
   D3D11_SUBRESOURCE_DATA iinit{indices.data(), 0, 0};
   ID3D11Buffer *ib; CHECK(dev->CreateBuffer(&ibd, &iinit, &ib));
   std::vector<char> constants = load(dir + "\\" + ubo); constants.resize(std::max<size_t>(64, (constants.size() + 15) & ~size_t(15)));
   D3D11_BUFFER_DESC cd{}; cd.ByteWidth = (UINT)constants.size(); cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA cinit{constants.data(), 0, 0};
   ID3D11Buffer *cbuf; CHECK(dev->CreateBuffer(&cd, &cinit, &cbuf));

   const UINT size = 1536;
   D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = size; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_TYPELESS;
   td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
   ID3D11Texture2D *depth; CHECK(dev->CreateTexture2D(&td, nullptr, &depth));
   D3D11_DEPTH_STENCIL_VIEW_DESC dvd{}; dvd.Format = DXGI_FORMAT_D16_UNORM; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
   ID3D11DepthStencilView *dsv; CHECK(dev->CreateDepthStencilView(depth, &dvd, &dsv));
   D3D11_DEPTH_STENCIL_DESC dsd{}; dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
   ID3D11DepthStencilState *dss; CHECK(dev->CreateDepthStencilState(&dsd, &dss));
   D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = strchr(opts, 'c') ? D3D11_CULL_BACK : D3D11_CULL_NONE;
   rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
   ID3D11RasterizerState *rs; CHECK(dev->CreateRasterizerState(&rd, &rs));

   D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT}; ID3D11Query *disjoint; CHECK(dev->CreateQuery(&qd, &disjoint));
   qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0, *t1; CHECK(dev->CreateQuery(&qd, &t0)); CHECK(dev->CreateQuery(&qd, &t1));
   qd.Query = D3D11_QUERY_PIPELINE_STATISTICS; ID3D11Query *stats; CHECK(dev->CreateQuery(&qd, &stats));
   D3D11_VIEWPORT vp{0, 0, (float)size, (float)size, 0, 1};
   D3D11_RECT scissor{0, 0, (LONG)size, (LONG)size}; if (strchr(opts, 's')) scissor.right = scissor.bottom = 1;
   ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->IASetInputLayout(il);
   ctx->IASetVertexBuffers(0, 5, vb, vstrides, voffsets); ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
   ctx->VSSetShader(vs, nullptr, 0); ctx->VSSetConstantBuffers(0, 1, &cbuf); ctx->PSSetShader(nullptr, nullptr, 0);
   ctx->RSSetState(rs); ctx->RSSetViewports(1, &vp); ctx->RSSetScissorRects(1, &scissor);
   ctx->OMSetRenderTargets(0, nullptr, dsv); ctx->OMSetDepthStencilState(dss, 0);
   double best = 1e9; D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
   for (int rep = -10; rep < 30; ++rep) {
      ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
      ctx->Begin(disjoint); ctx->End(t0); ctx->Begin(stats);
      for (int i = 0; i < repeats; ++i) ctx->DrawIndexedInstanced(count, instances, first, vertex_offset, first_instance);
      ctx->End(stats); ctx->End(t1); ctx->End(disjoint);
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
      while (ctx->GetData(t0, &a, sizeof(a), 0) != S_OK) {}
      while (ctx->GetData(t1, &b, sizeof(b), 0) != S_OK) {}
      while (ctx->GetData(stats, &ps, sizeof(ps), 0) != S_OK) {}
      if (dj.Disjoint || rep < 0) continue;
      const double ms = (b - a) * 1000.0 / dj.Frequency;
      if (ms < best) best = ms;
   }
   printf("%.4f ms per draw; %llu IA vertices, %llu VS, %llu clip out, %llu PS\n", best / repeats, ps.IAVertices / repeats, ps.VSInvocations / repeats,
          ps.CPrimitives / repeats, ps.PSInvocations / repeats);
   return 0;
}
