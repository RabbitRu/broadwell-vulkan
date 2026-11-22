/* Timing proxy d3d11.dll: the DX11 counterpart of HASVK_WDDM_PROFILE_PASSES (branch debug).
 *
 * Put next to a game's executable only for a measurement run (tools/
 * run-talos.ps1 -D3D11Proxy copies it there and removes it afterwards). It
 * loads the system d3d11.dll by full path, forwards every export (stubs.c),
 * and after D3D11CreateDevice(AndSwapChain) patches the immediate context's
 * vtable (render target changes, draws, copies, command lists). Frames end
 * when the game leaves the swap chain's back buffer (DXGI_USAGE_BACK_BUFFER)
 * for another render target: hooking Present would collide with the Steam
 * overlay's own Present hook. Every render target change starts a segment with a
 * D3D11 timestamp query; a disjoint query spans each frame. Results are read
 * three frames later and every 5 s the most expensive segments per frame
 * (render target and depth size/format, draw count) are written to the file
 * named by D3D11_PROFILE_LOG.
 * Assumes, as D3D11 itself does, that the immediate context is used from one
 * thread at a time. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void proxy_resolve_stubs(HMODULE real);
static BOOL split_enabled(void);

typedef HRESULT(WINAPI *PFN_CREATE)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CREATE_SWAP)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                         const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                         ID3D11DeviceContext **);
static PFN_CREATE real_create;
static PFN_CREATE_SWAP real_create_swap;
static FILE *logfile;
static CRITICAL_SECTION lock;

static void logf_(const char *fmt, ...)
{
   char text[1024];
   va_list args; va_start(args, fmt); vsnprintf(text, sizeof(text), fmt, args); va_end(args);
   if (logfile) { fputs(text, logfile); fflush(logfile); } else OutputDebugStringA(text);
}

/* ---- profiling state ---- */
#define QUERIES 8192u
#define FRAMES 4u
#define SEGMENTS 1024u
#define BUCKETS 256u
struct segment { UINT query; UINT draws; char key[96]; };
#define STATS 4096u   /* pipeline statistics queries, one per segment */
struct frame { ID3D11Query *disjoint; UINT count; UINT end_query; BOOL open, overflow; struct segment seg[SEGMENTS]; };
struct bucket { char key[96]; double ms, max_ms; unsigned long long count, draws, stats_count; double ia_vertices, vs, c_prims, ps, max_ps, samples, c_inv;
                unsigned long long heavy;   /* segments with more than 100k PS invocations */ };
static struct {
   ID3D11Device *device;
   ID3D11DeviceContext *context;
   ID3D11Query *queries[QUERIES];
   ID3D11Query *stats[STATS];   /* index = timestamp query % STATS of the segment start */
   ID3D11Query *occlusion[STATS];   /* samples passed, same index (D3D11_SPLIT_SHADERS only) */
   BOOL stats_open; UINT stats_open_index;
   UINT next_query;
   struct frame frames[FRAMES];
   UINT frame;                 /* frames presented */
   char last_key[96];
   BOOL remark;                /* a copy/compute segment interrupted the render target's */
   BOOL on_backbuffer;         /* the last render target was the swap chain's */
   void **vtable;              /* the immediate context's vtable when it was patched */
   struct bucket buckets[BUCKETS];
   UINT bucket_count;
   unsigned long long window_frames, dropped;
   DWORD window_start;
   BOOL ready;
} p;

/* Original methods. */
static void (STDMETHODCALLTYPE *o_End)(ID3D11DeviceContext *, ID3D11Asynchronous *);
static void (STDMETHODCALLTYPE *o_Begin)(ID3D11DeviceContext *, ID3D11Asynchronous *);
static HRESULT (STDMETHODCALLTYPE *o_GetData)(ID3D11DeviceContext *, ID3D11Asynchronous *, void *, UINT, UINT);
static void (STDMETHODCALLTYPE *o_OMSetRenderTargets)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void (STDMETHODCALLTYPE *o_OMSetRTUAV)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT, UINT,
                                              ID3D11UnorderedAccessView *const *, const UINT *);
static void (STDMETHODCALLTYPE *o_Draw)(ID3D11DeviceContext *, UINT, UINT);
static void (STDMETHODCALLTYPE *o_DrawIndexed)(ID3D11DeviceContext *, UINT, UINT, INT);
static void (STDMETHODCALLTYPE *o_DrawInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
static void (STDMETHODCALLTYPE *o_DrawIndexedInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static void (STDMETHODCALLTYPE *o_DrawIndexedInstancedIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
static void (STDMETHODCALLTYPE *o_DrawInstancedIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
static void (STDMETHODCALLTYPE *o_Dispatch)(ID3D11DeviceContext *, UINT, UINT, UINT);
static void (STDMETHODCALLTYPE *o_CopyResource)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
static void (STDMETHODCALLTYPE *o_CopySubresourceRegion)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT,
                                                         const D3D11_BOX *);
static void (STDMETHODCALLTYPE *o_ResolveSubresource)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT);
static void (STDMETHODCALLTYPE *o_ExecuteCommandList)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
static HRESULT (STDMETHODCALLTYPE *o_Present)(IDXGISwapChain *, UINT, UINT);
static void (STDMETHODCALLTYPE *o_VSSetShader)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
static void (STDMETHODCALLTYPE *o_PSSetShader)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
static void (STDMETHODCALLTYPE *o_OMSetDepthStencilState)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
static void (STDMETHODCALLTYPE *o_RSSetState)(ID3D11DeviceContext *, ID3D11RasterizerState *);
static void (STDMETHODCALLTYPE *o_RSSetViewports)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
static void (STDMETHODCALLTYPE *o_ClearDepthStencilView)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
static void (STDMETHODCALLTYPE *o_RSSetScissorRects)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
static HRESULT (STDMETHODCALLTYPE *o_CreatePixelShader)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static HRESULT (STDMETHODCALLTYPE *o_CreateVertexShader)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
static HRESULT (STDMETHODCALLTYPE *o_Present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

static BOOL describe_view(ID3D11View *view, char *out, size_t size, const char *what)
{
   ID3D11Resource *resource = NULL; ID3D11Texture2D *texture = NULL; IDXGIResource *dxgi = NULL; BOOL back = FALSE;
   ID3D11View_GetResource(view, &resource);
   if (resource && SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_IDXGIResource, (void **)&dxgi))) {
      DXGI_USAGE usage = 0;
      if (SUCCEEDED(IDXGIResource_GetUsage(dxgi, &usage))) back = (usage & DXGI_USAGE_BACK_BUFFER) != 0;
      IDXGIResource_Release(dxgi);
   }
   if (resource && SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture))) {
      D3D11_TEXTURE2D_DESC desc; ID3D11Texture2D_GetDesc(texture, &desc);
      snprintf(out, size, "%s %ux%u f%d%s%s", what, desc.Width, desc.Height, (int)desc.Format, desc.SampleDesc.Count > 1 ? "xMS" : "",
               back ? " backbuffer" : "");
      ID3D11Texture2D_Release(texture);
   } else snprintf(out, size, "%s ?", what);
   if (resource) ID3D11Resource_Release(resource);
   return back;
}

static void bucket_add(const char *key, double ms, UINT draws, const D3D11_QUERY_DATA_PIPELINE_STATISTICS *ps, UINT64 samples)
{
   UINT i = 0;
   while (i < p.bucket_count && strcmp(p.buckets[i].key, key)) ++i;
   if (i == p.bucket_count) { if (i == BUCKETS) return; ++p.bucket_count; memset(&p.buckets[i], 0, sizeof(p.buckets[i])); strcpy(p.buckets[i].key, key); }
   p.buckets[i].ms += ms; ++p.buckets[i].count; p.buckets[i].draws += draws;
   if (ms > p.buckets[i].max_ms) p.buckets[i].max_ms = ms;
   if (ps && (double)ps->PSInvocations > p.buckets[i].max_ps) p.buckets[i].max_ps = (double)ps->PSInvocations;
   if (ps && ps->PSInvocations > 100000) ++p.buckets[i].heavy;
   if (ps) { ++p.buckets[i].stats_count; p.buckets[i].ia_vertices += (double)ps->IAVertices; p.buckets[i].vs += (double)ps->VSInvocations;
             p.buckets[i].c_prims += (double)ps->CPrimitives; p.buckets[i].ps += (double)ps->PSInvocations; p.buckets[i].c_inv += (double)ps->CInvocations; }
   if (samples != UINT64_MAX) p.buckets[i].samples += (double)samples;
}

/* Starts a new segment (and the frame, if none is open). */
static void mark(ID3D11DeviceContext *ctx, const char *key)
{
   if (!p.ready || ctx != p.context) return;
   struct frame *f = &p.frames[p.frame % FRAMES];
   if (!f->open) { o_Begin(ctx, (ID3D11Asynchronous *)f->disjoint); f->open = TRUE; f->count = 0; f->overflow = FALSE; }
   if (f->count == SEGMENTS) { f->overflow = TRUE; return; }
   struct segment *s = &f->seg[f->count++];
   s->query = p.next_query++ % QUERIES; s->draws = 0;
   strncpy(s->key, key, sizeof(s->key) - 1); s->key[sizeof(s->key) - 1] = 0;
   o_End(ctx, (ID3D11Asynchronous *)p.queries[s->query]);
   if (p.stats_open) { o_End(ctx, (ID3D11Asynchronous *)p.stats[p.stats_open_index]);
                       if (p.occlusion[p.stats_open_index]) o_End(ctx, (ID3D11Asynchronous *)p.occlusion[p.stats_open_index]); }
   p.stats_open_index = s->query % STATS; o_Begin(ctx, (ID3D11Asynchronous *)p.stats[p.stats_open_index]); p.stats_open = TRUE;
   if (p.occlusion[p.stats_open_index]) o_Begin(ctx, (ID3D11Asynchronous *)p.occlusion[p.stats_open_index]);
}

static volatile LONG n_draw_immediate2, n_draw_other2;
static void count_draw(ID3D11DeviceContext *ctx)
{
   InterlockedIncrement(ctx == p.context ? &n_draw_immediate2 : &n_draw_other2);
   if (!p.ready || ctx != p.context) return;
   if (p.remark) { p.remark = FALSE; mark(ctx, p.last_key); }
   struct frame *f = &p.frames[p.frame % FRAMES];
   if (f->open && f->count) ++f->seg[f->count - 1].draws;
   else mark(ctx, p.last_key[0] ? p.last_key : "draws before any target");
}

static void collect(struct frame *f)
{
   if (!f->disjoint || f->open || !f->count) return;
   D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
   if (o_GetData(p.context, (ID3D11Asynchronous *)f->disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint || f->overflow) {
      ++p.dropped; f->count = 0; return;
   }
   UINT64 previous = 0;
   for (UINT i = 0; i <= f->count; ++i) {
      UINT64 t;
      const UINT q = i < f->count ? f->seg[i].query : f->end_query;
      if (o_GetData(p.context, (ID3D11Asynchronous *)p.queries[q], &t, sizeof(t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ++p.dropped; f->count = 0; return; }
      if (i) {
         D3D11_QUERY_DATA_PIPELINE_STATISTICS ps;
         const BOOL have = o_GetData(p.context, (ID3D11Asynchronous *)p.stats[f->seg[i - 1].query % STATS], &ps, sizeof(ps),
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
         UINT64 samples = UINT64_MAX; ID3D11Query *oq = p.occlusion[f->seg[i - 1].query % STATS];
         if (oq && o_GetData(p.context, (ID3D11Asynchronous *)oq, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) samples = UINT64_MAX;
         bucket_add(f->seg[i - 1].key, (double)(t - previous) * 1000.0 / (double)dj.Frequency, f->seg[i - 1].draws, have ? &ps : NULL, samples);
      }
      previous = t;
   }
   f->count = 0;
   ++p.window_frames;
}

static int compare(const void *a, const void *b)
{
   const struct bucket *x = a, *y = b;
   return x->ms < y->ms ? 1 : x->ms > y->ms ? -1 : 0;
}

static volatile LONG n_frames_ended;
static void end_frame(void)
{
   if (!p.ready) return;
   InterlockedIncrement(&n_frames_ended);
   EnterCriticalSection(&lock);
   struct frame *f = &p.frames[p.frame % FRAMES];
   if (f->open) {
      f->end_query = p.next_query++ % QUERIES;
      o_End(p.context, (ID3D11Asynchronous *)p.queries[f->end_query]);
      if (p.stats_open) { o_End(p.context, (ID3D11Asynchronous *)p.stats[p.stats_open_index]);
                          if (p.occlusion[p.stats_open_index]) o_End(p.context, (ID3D11Asynchronous *)p.occlusion[p.stats_open_index]); p.stats_open = FALSE; }
      o_End(p.context, (ID3D11Asynchronous *)f->disjoint);
      f->open = FALSE;
   }
   ++p.frame;
   collect(&p.frames[(p.frame + 1) % FRAMES]);   /* the frame recorded three presents ago */
   const DWORD now = GetTickCount();
   if (!p.window_start) p.window_start = now;
   if (now - p.window_start >= 5000 && p.window_frames) {
      qsort(p.buckets, p.bucket_count, sizeof(p.buckets[0]), compare);
      double total = 0;
      for (UINT i = 0; i < p.bucket_count; ++i) total += p.buckets[i].ms;
      const double n = (double)p.window_frames;
      logf_("D3D11 profile: %llu frames in %.1f s, GPU time %.2f ms/frame (%llu frames dropped)\n", p.window_frames,
            (now - p.window_start) / 1000.0, total / n, p.dropped);
      for (UINT i = 0; i < p.bucket_count && i < (split_enabled() ? 45u : 25u); ++i)
         logf_("  %6.2f ms/frame %5.1f/frame avg %6.3f ms draws %6.1f | %s\n", p.buckets[i].ms / n, p.buckets[i].count / n,
               p.buckets[i].ms / p.buckets[i].count, (double)p.buckets[i].draws / p.buckets[i].count, p.buckets[i].key);
      for (UINT i = 0; i < p.bucket_count && i < (split_enabled() ? 40u : 8u); ++i)
         if (p.buckets[i].stats_count)
            logf_("    per segment: max %.2f ms, max %.0fk PS, %llu of %llu segments over 100k PS | %s\n", p.buckets[i].max_ms, p.buckets[i].max_ps / 1e3,
                  p.buckets[i].heavy, p.buckets[i].stats_count, p.buckets[i].key),
            logf_("    per segment: %.0fk IA vertices, %.0fk VS, %.1fk clip in, %.1fk clip out, %.1fk PS, %.1fk samples passed | %s\n",
                  p.buckets[i].ia_vertices / 1e3 / p.buckets[i].stats_count,
                  p.buckets[i].vs / 1e3 / p.buckets[i].stats_count, p.buckets[i].c_inv / 1e3 / p.buckets[i].stats_count,
                  p.buckets[i].c_prims / 1e3 / p.buckets[i].stats_count,
                  p.buckets[i].ps / 1e3 / p.buckets[i].stats_count, p.buckets[i].samples / 1e3 / p.buckets[i].stats_count, p.buckets[i].key);
      p.bucket_count = 0; p.window_frames = 0; p.dropped = 0; p.window_start = now;
   }
   LeaveCriticalSection(&lock);
}

/* ---- shadow map dump (D3D11_DUMP_DIR, D3D11_DUMP_AT seconds after the first
 * shadow pass): every 1536x1536 depth buffer of one frame, when the game
 * switches away from it, as raw 16-bit rows (<dir>\d3d11-shadow-<n>.r16). ---- */
static ID3D11Resource *shadow_res;
static UINT shadow_slice;   /* array slice the depth view renders into */
static DWORD shadow_first_ms;
static UINT dump_frame = UINT_MAX, dump_count;
static void dump_shadow_as(ID3D11DeviceContext *ctx, const char *suffix);
static void dump_shadow(ID3D11DeviceContext *ctx) { dump_shadow_as(ctx, ""); }
static void dump_shadow_as(ID3D11DeviceContext *ctx, const char *suffix)
{
   char dir[MAX_PATH], at[32];
   if (!shadow_res || !GetEnvironmentVariableA("D3D11_DUMP_DIR", dir, sizeof(dir))) return;
   const DWORD now = GetTickCount();
   if (!shadow_first_ms) shadow_first_ms = now;
   const DWORD wait = GetEnvironmentVariableA("D3D11_DUMP_AT", at, sizeof(at)) ? (DWORD)atoi(at) * 1000u : 20000u;
   if (dump_frame == UINT_MAX && now - shadow_first_ms >= wait) dump_frame = p.frame;
   if (p.frame != dump_frame || dump_count >= 8) return;
   ID3D11Texture2D *src = NULL, *staging = NULL;
   if (FAILED(ID3D11Resource_QueryInterface(shadow_res, &IID_ID3D11Texture2D, (void **)&src))) return;
   D3D11_TEXTURE2D_DESC desc; ID3D11Texture2D_GetDesc(src, &desc);
   desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
   if (SUCCEEDED(ID3D11Device_CreateTexture2D(p.device, &desc, NULL, &staging))) {
      o_CopyResource(ctx, (ID3D11Resource *)staging, shadow_res);
      D3D11_MAPPED_SUBRESOURCE map;
      const UINT sub = shadow_slice * desc.MipLevels;   /* D3D11CalcSubresource(0, slice, mips) */
      if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, sub, D3D11_MAP_READ, 0, &map))) {
         char path[MAX_PATH + 40]; snprintf(path, sizeof(path), "%s\\d3d11-shadow-%u%s.r16", dir, dump_count, suffix);
         FILE *out = fopen(path, "wb");
         if (out) { for (UINT y = 0; y < desc.Height; ++y) fwrite((const char *)map.pData + (size_t)y * map.RowPitch, 2, desc.Width, out); fclose(out); }
         ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, sub);
         logf_("D3D11 proxy: dumped shadow map %u%s (%ux%u format %d, slice %u of %u, frame %u, %.1f s after the first) to %s\n", dump_count, suffix,
               desc.Width, desc.Height, (int)desc.Format, shadow_slice, desc.ArraySize, p.frame, (now - shadow_first_ms) / 1000.0, path);
         if (!*suffix) ++dump_count;
      }
      ID3D11Texture2D_Release(staging);
   }
   ID3D11Texture2D_Release(src);
}
static void track_shadow(ID3D11DeviceContext *ctx, ID3D11DepthStencilView *dsv)
{
   dump_shadow(ctx);   /* the previous target is complete */
   if (shadow_res) { ID3D11Resource_Release(shadow_res); shadow_res = NULL; }
   if (!dsv) return;
   ID3D11Resource *res = NULL; ID3D11Texture2D *tex = NULL;
   ID3D11View_GetResource((ID3D11View *)dsv, &res);
   if (res && SUCCEEDED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
      D3D11_TEXTURE2D_DESC desc; ID3D11Texture2D_GetDesc(tex, &desc);
      if (desc.Width == 1536 && desc.Height == 1536) {
         shadow_res = res; res = NULL;
         D3D11_DEPTH_STENCIL_VIEW_DESC vd; ID3D11DepthStencilView_GetDesc(dsv, &vd);
         shadow_slice = vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? vd.Texture2DArray.FirstArraySlice : 0;
         dump_shadow_as(ctx, "-before");   /* the content the frame starts from */
      }
      ID3D11Texture2D_Release(tex);
   }
   if (res) ID3D11Resource_Release(res);
}

/* ---- shader split (D3D11_SPLIT_SHADERS=1): inside 1536x1536 depth targets
 * (shadow maps) every vertex/pixel shader change starts a segment keyed by the
 * shader pair, numbered in order of first use ("none" = no pixel shader). ---- */
static char shader_target[96];
static BOOL split_shaders;
static const void *vs_seen[512], *ps_seen[512];
static UINT vs_seen_count, ps_seen_count;
static int cur_vs = -1, cur_ps = -1;
static char cur_depth[24] = "z?", cur_raster[24] = "r?", cur_view[40] = "";
static volatile LONG n_vsset, n_psset;
/* Bytecode of every created shader (for D3D11_SPLIT_SHADERS with
 * D3D11_DUMP_DIR: <dir>\vs<n>.dxbc / ps<n>.dxbc as each gets its number). */
struct code { const void *shader; void *data; SIZE_T size; };
static struct code *codes; static UINT code_count, code_capacity;
static CRITICAL_SECTION code_lock;
static void keep_code(const void *shader, const void *data, SIZE_T size)
{
   if (!split_enabled() || !shader) return;
   EnterCriticalSection(&code_lock);
   if (code_count == code_capacity) { code_capacity = code_capacity ? code_capacity * 2 : 1024; codes = realloc(codes, code_capacity * sizeof(*codes)); }
   codes[code_count].shader = shader; codes[code_count].data = malloc(size); codes[code_count].size = size;
   if (codes[code_count].data) { memcpy(codes[code_count].data, data, size); ++code_count; }
   LeaveCriticalSection(&code_lock);
}
static void dump_code(const void *shader, const char *kind, int index)
{
   char dir[MAX_PATH]; if (!GetEnvironmentVariableA("D3D11_DUMP_DIR", dir, sizeof(dir))) return;
   EnterCriticalSection(&code_lock);
   for (UINT i = code_count; i-- > 0;)
      if (codes[i].shader == shader) {
         char path[MAX_PATH + 32]; snprintf(path, sizeof(path), "%s\\%s%d.dxbc", dir, kind, index);
         FILE *out = fopen(path, "wb"); if (out) { fwrite(codes[i].data, 1, codes[i].size, out); fclose(out); }
         break;
      }
   LeaveCriticalSection(&code_lock);
}
static int shader_index(const void *shader, const void **seen, UINT *count)
{
   if (!shader) return -1;
   for (UINT i = 0; i < *count; ++i) if (seen[i] == shader) return (int)i;
   if (*count == 512) return 511;
   seen[*count] = shader;
   dump_code(shader, seen == vs_seen ? "vs" : "ps", (int)*count);
   return (int)(*count)++;
}
static HRESULT STDMETHODCALLTYPE h_CreatePixelShader(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l, ID3D11PixelShader **s)
{ const HRESULT r = o_CreatePixelShader(d, b, n, l, s); if (SUCCEEDED(r) && s) keep_code(*s, b, n); return r; }
static HRESULT STDMETHODCALLTYPE h_CreateVertexShader(ID3D11Device *d, const void *b, SIZE_T n, ID3D11ClassLinkage *l, ID3D11VertexShader **s)
{ const HRESULT r = o_CreateVertexShader(d, b, n, l, s); if (SUCCEEDED(r) && s) keep_code(*s, b, n); return r; }
static BOOL split_enabled(void)
{
   static int on = -1;
   if (on < 0) { char v[8]; on = GetEnvironmentVariableA("D3D11_SPLIT_SHADERS", v, sizeof(v)) > 0; }
   return on;
}
static void shader_mark(ID3D11DeviceContext *c)
{
   if (!p.ready || c != p.context || !split_shaders || !split_enabled()) return;
   char ps[16]; if (cur_ps < 0) strcpy(ps, "none"); else snprintf(ps, sizeof(ps), "#%d", cur_ps);
   char key[96]; snprintf(key, sizeof(key), "%.30s|VS%d PS %s %s %s%s", shader_target + 9, cur_vs, ps, cur_depth, cur_raster, cur_view);
   if (!strcmp(key, p.last_key)) return;
   strcpy(p.last_key, key); p.remark = FALSE; mark(c, key);
}

/* ---- hooks ---- */
static volatile LONG n_omset_immediate, n_omset_other, n_draw_immediate, n_draw_other, n_ecl, n_end_frame;
static void target_key(UINT count, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv)
{
   char rt[64] = "no RT", ds[48] = "no DS";
   BOOL back = FALSE;
   if (count && rtvs && rtvs[0]) back = describe_view((ID3D11View *)rtvs[0], rt, sizeof(rt), "RT");
   if (dsv) describe_view((ID3D11View *)dsv, ds, sizeof(ds), "DS");
   {  /* each distinct target once, for finding the frame structure */
      static char seen[256][96]; static UINT seen_count;
      char key[96]; snprintf(key, sizeof(key), "%u: %s + %s", count, rt, ds);
      UINT i = 0; while (i < seen_count && strcmp(seen[i], key)) ++i;
      if (i == seen_count && seen_count < 256) { strcpy(seen[seen_count++], key); logf_("target %u: %s\n", seen_count, key); }
   }
   if (p.on_backbuffer && !back && (count || dsv)) end_frame();   /* the next frame starts */
   if (count || dsv) p.on_backbuffer = back;
   snprintf(p.last_key, sizeof(p.last_key), "%u: %s + %s", count, rt, ds);
   snprintf(shader_target, sizeof(shader_target), "%s", p.last_key);
   split_shaders = strstr(ds, "1536x1536") != NULL;
}
static void STDMETHODCALLTYPE h_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *r, ID3D11DepthStencilView *d)
{
   InterlockedIncrement(c == p.context ? &n_omset_immediate : &n_omset_other);
   /* Segments start here, not at the first draw: the runtime rewrites the draw
    * slots of the vtable, so draws are not seen. */
   if (p.ready && c == p.context) { track_shadow(c, d); target_key(n, r, d); p.remark = FALSE; mark(c, p.last_key); }
   o_OMSetRenderTargets(c, n, r, d);
}
static void STDMETHODCALLTYPE h_OMSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *r, ID3D11DepthStencilView *d,
                                           UINT s, UINT u, ID3D11UnorderedAccessView *const *v, const UINT *i)
{
   if (p.ready && c == p.context && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { track_shadow(c, d); target_key(n, r, d); p.remark = FALSE; mark(c, p.last_key); }
   o_OMSetRTUAV(c, n, r, d, s, u, v, i);
}
static void STDMETHODCALLTYPE h_Draw(ID3D11DeviceContext *c, UINT a, UINT b) { count_draw(c); o_Draw(c, a, b); }
static void STDMETHODCALLTYPE h_DrawIndexed(ID3D11DeviceContext *c, UINT a, UINT b, INT d) { count_draw(c); o_DrawIndexed(c, a, b, d); }
static void STDMETHODCALLTYPE h_DrawInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e) { count_draw(c); o_DrawInstanced(c, a, b, d, e); }
static void STDMETHODCALLTYPE h_DrawIndexedInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f)
{ count_draw(c); o_DrawIndexedInstanced(c, a, b, d, e, f); }
static void STDMETHODCALLTYPE h_DrawIndexedInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT o) { count_draw(c); o_DrawIndexedInstancedIndirect(c, b, o); }
static void STDMETHODCALLTYPE h_DrawInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT o) { count_draw(c); o_DrawInstancedIndirect(c, b, o); }
static void STDMETHODCALLTYPE h_Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z) { mark(c, "compute"); o_Dispatch(c, x, y, z); mark(c, p.last_key); }
static void STDMETHODCALLTYPE h_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *d, ID3D11Resource *s) { mark(c, "copy"); o_CopyResource(c, d, s); mark(c, p.last_key); }
static void STDMETHODCALLTYPE h_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource *s,
                                                      UINT ss, const D3D11_BOX *b)
{ mark(c, "copy"); o_CopySubresourceRegion(c, d, ds, x, y, z, s, ss, b); mark(c, p.last_key); }
static void STDMETHODCALLTYPE h_ResolveSubresource(ID3D11DeviceContext *c, ID3D11Resource *d, UINT ds, ID3D11Resource *s, UINT ss, DXGI_FORMAT f)
{ mark(c, "resolve"); o_ResolveSubresource(c, d, ds, s, ss, f); mark(c, p.last_key); }
static void STDMETHODCALLTYPE h_ExecuteCommandList(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL r)
{ InterlockedIncrement(&n_ecl); mark(c, "command list"); p.remark = TRUE; o_ExecuteCommandList(c, l, r); }
static void STDMETHODCALLTYPE h_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s, ID3D11ClassInstance *const *ci, UINT n)
{ InterlockedIncrement(&n_vsset); if (c == p.context) { cur_vs = shader_index(s, vs_seen, &vs_seen_count); shader_mark(c); } o_VSSetShader(c, s, ci, n); }
static void STDMETHODCALLTYPE h_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s, ID3D11ClassInstance *const *ci, UINT n)
{ InterlockedIncrement(&n_psset); if (c == p.context) { cur_ps = shader_index(s, ps_seen, &ps_seen_count); shader_mark(c); } o_PSSetShader(c, s, ci, n); }
static void STDMETHODCALLTYPE h_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref)
{
   if (c == p.context) {
      if (!s) strcpy(cur_depth, "z-default");
      else { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState_GetDesc(s, &d);
             snprintf(cur_depth, sizeof(cur_depth), "z%d f%d w%d", d.DepthEnable, (int)d.DepthFunc, (int)d.DepthWriteMask); }
      shader_mark(c);
   }
   o_OMSetDepthStencilState(c, s, ref);
}
static D3D11_VIEWPORT last_vp; static D3D11_RECT last_sc; static BOOL scissor_on;
static void view_key(ID3D11DeviceContext *c)
{
   char sc[24] = ""; if (scissor_on) snprintf(sc, sizeof(sc), " sc%ldx%ld", last_sc.right - last_sc.left, last_sc.bottom - last_sc.top);
   snprintf(cur_view, sizeof(cur_view), " vp%.0f,%.0f %.0fx%.0f%s", last_vp.TopLeftX, last_vp.TopLeftY, last_vp.Width, last_vp.Height, sc);
   shader_mark(c);
}
static void STDMETHODCALLTYPE h_RSSetViewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v)
{ if (c == p.context && n && v) { last_vp = v[0]; view_key(c); } o_RSSetViewports(c, n, v); }
static void STDMETHODCALLTYPE h_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{ if (c == p.context && n && r) { last_sc = r[0]; view_key(c); } o_RSSetScissorRects(c, n, r); }
/* Depth clears of 1536x1536 targets: counted by (slice, flags, value), logged with the profile. */
static struct { UINT slice, flags; float depth; unsigned long long count; } clears[32]; static UINT clear_kinds;
static void STDMETHODCALLTYPE h_ClearDepthStencilView(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil)
{
   if (c == p.context && v && split_enabled()) {
      ID3D11Resource *res = NULL; ID3D11Texture2D *tex = NULL; ID3D11View_GetResource((ID3D11View *)v, &res);
      if (res && SUCCEEDED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
         D3D11_TEXTURE2D_DESC desc; ID3D11Texture2D_GetDesc(tex, &desc);
         if (desc.Width == 1536) {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd; ID3D11DepthStencilView_GetDesc(v, &vd);
            const UINT slice = vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? vd.Texture2DArray.FirstArraySlice : 0;
            UINT i = 0; while (i < clear_kinds && !(clears[i].slice == slice && clears[i].flags == flags && clears[i].depth == depth)) ++i;
            if (i == clear_kinds && i < 32) { clears[i].slice = slice; clears[i].flags = flags; clears[i].depth = depth; clears[i].count = 0; ++clear_kinds; }
            if (i < 32) ++clears[i].count;
         }
         ID3D11Texture2D_Release(tex);
      }
      if (res) ID3D11Resource_Release(res);
   }
   o_ClearDepthStencilView(c, v, flags, depth, stencil);
}
static void STDMETHODCALLTYPE h_RSSetState(ID3D11DeviceContext *c, ID3D11RasterizerState *s)
{
   if (c == p.context) {
      if (!s) strcpy(cur_raster, "r-default");
      else { D3D11_RASTERIZER_DESC d; ID3D11RasterizerState_GetDesc(s, &d);
             snprintf(cur_raster, sizeof(cur_raster), "cull%d s%.1f", (int)d.CullMode, d.SlopeScaledDepthBias); scissor_on = d.ScissorEnable; view_key(c); }
      shader_mark(c);
   }
   o_RSSetState(c, s);
}
static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain *s, UINT i, UINT f) { end_frame(); return o_Present(s, i, f); }
static HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1 *s, UINT i, UINT f, const DXGI_PRESENT_PARAMETERS *pp) { end_frame(); return o_Present1(s, i, f, pp); }

static void patch(void **vtable, size_t offset, void *hook, void **original)
{
   void **slot = vtable + offset / sizeof(void *);
   if (*slot == hook) return;
   DWORD old;
   VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
   *original = *slot; *slot = hook;
   VirtualProtect(slot, sizeof(void *), old, &old);
}
#define PATCH_CTX(field, hook, original) patch((void **)ctx->lpVtbl, offsetof(ID3D11DeviceContextVtbl, field), (void *)hook, (void **)&original)

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcA(w, m, a, b); }

/* IDXGISwapChain's vtable is shared by every swap chain from dxgi.dll: one
 * temporary swap chain on a hidden window gives it. */
static void hook_present(ID3D11Device *device)
{
   IDXGIDevice *dxgi = NULL; IDXGIAdapter *adapter = NULL; IDXGIFactory *factory = NULL; IDXGISwapChain *swap = NULL;
   WNDCLASSA wc = {0}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d11_proxy_probe";
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
   if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi)) && SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) &&
       SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory))) {
      DXGI_SWAP_CHAIN_DESC desc = {0};
      desc.BufferDesc.Width = desc.BufferDesc.Height = 64; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 1; desc.OutputWindow = hwnd; desc.Windowed = TRUE;
      if (SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, &swap))) {
         patch((void **)swap->lpVtbl, offsetof(IDXGISwapChainVtbl, Present), (void *)h_Present, (void **)&o_Present);
         IDXGISwapChain1 *swap1 = NULL;
         if (SUCCEEDED(IDXGISwapChain_QueryInterface(swap, &IID_IDXGISwapChain1, (void **)&swap1))) {
            patch((void **)swap1->lpVtbl, offsetof(IDXGISwapChain1Vtbl, Present1), (void *)h_Present1, (void **)&o_Present1);
            IDXGISwapChain1_Release(swap1);
         }
         IDXGISwapChain_Release(swap);
         logf_("D3D11 proxy: Present hooked\n");
      } else logf_("D3D11 proxy: temporary swap chain failed; no frame boundaries\n");
   }
   if (factory) IDXGIFactory_Release(factory);
   if (adapter) IDXGIAdapter_Release(adapter);
   if (dxgi) IDXGIDevice_Release(dxgi);
   DestroyWindow(hwnd);
}

static void install(ID3D11Device *device, ID3D11DeviceContext *ctx)
{
   EnterCriticalSection(&lock);
   if (!device || !ctx || ctx == p.context) { LeaveCriticalSection(&lock); return; }
   /* A later device (games often create a temporary one first) becomes the
    * profiled one; the vtable hooks are shared by all contexts of the class. */
   p.ready = FALSE;
   for (UINT i = 0; i < QUERIES; ++i) if (p.queries[i]) { ID3D11Query_Release(p.queries[i]); p.queries[i] = NULL; }
   for (UINT i = 0; i < FRAMES; ++i) {
      if (p.frames[i].disjoint) ID3D11Query_Release(p.frames[i].disjoint);
      memset(&p.frames[i], 0, sizeof(p.frames[i]));
   }
   p.bucket_count = 0; p.window_frames = 0; p.dropped = 0; p.window_start = 0; p.last_key[0] = 0; p.remark = FALSE;
   const BOOL hooked = o_End != NULL;
   D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0};
   for (UINT i = 0; i < QUERIES; ++i)
      if (FAILED(ID3D11Device_CreateQuery(device, &qd, &p.queries[i]))) { LeaveCriticalSection(&lock); logf_("D3D11 proxy: CreateQuery failed\n"); return; }
   qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
   for (UINT i = 0; i < STATS; ++i) if (!p.stats[i]) ID3D11Device_CreateQuery(device, &qd, &p.stats[i]);
   if (split_enabled()) { qd.Query = D3D11_QUERY_OCCLUSION; for (UINT i = 0; i < STATS; ++i) if (!p.occlusion[i]) ID3D11Device_CreateQuery(device, &qd, &p.occlusion[i]); }
   p.stats_open = FALSE;
   qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
   for (UINT i = 0; i < FRAMES; ++i) ID3D11Device_CreateQuery(device, &qd, &p.frames[i].disjoint);
   p.device = device; p.context = ctx;
   p.vtable = (void **)ctx->lpVtbl;
   if (hooked) { p.ready = TRUE; LeaveCriticalSection(&lock); logf_("D3D11 proxy: profiling a newer device\n"); return; }
   o_End = ctx->lpVtbl->End; o_Begin = ctx->lpVtbl->Begin; o_GetData = ctx->lpVtbl->GetData;
   PATCH_CTX(OMSetRenderTargets, h_OMSetRenderTargets, o_OMSetRenderTargets);
   PATCH_CTX(VSSetShader, h_VSSetShader, o_VSSetShader);
   PATCH_CTX(PSSetShader, h_PSSetShader, o_PSSetShader);
   PATCH_CTX(OMSetDepthStencilState, h_OMSetDepthStencilState, o_OMSetDepthStencilState);
   if (split_enabled()) {
      InitializeCriticalSection(&code_lock);
      patch((void **)device->lpVtbl, offsetof(ID3D11DeviceVtbl, CreatePixelShader), (void *)h_CreatePixelShader, (void **)&o_CreatePixelShader);
      patch((void **)device->lpVtbl, offsetof(ID3D11DeviceVtbl, CreateVertexShader), (void *)h_CreateVertexShader, (void **)&o_CreateVertexShader);
   }
   PATCH_CTX(RSSetState, h_RSSetState, o_RSSetState);
   PATCH_CTX(RSSetViewports, h_RSSetViewports, o_RSSetViewports);
   PATCH_CTX(ClearDepthStencilView, h_ClearDepthStencilView, o_ClearDepthStencilView);
   PATCH_CTX(RSSetScissorRects, h_RSSetScissorRects, o_RSSetScissorRects);
   PATCH_CTX(OMSetRenderTargetsAndUnorderedAccessViews, h_OMSetRTUAV, o_OMSetRTUAV);
   PATCH_CTX(Draw, h_Draw, o_Draw);
   PATCH_CTX(DrawIndexed, h_DrawIndexed, o_DrawIndexed);
   PATCH_CTX(DrawInstanced, h_DrawInstanced, o_DrawInstanced);
   PATCH_CTX(DrawIndexedInstanced, h_DrawIndexedInstanced, o_DrawIndexedInstanced);
   PATCH_CTX(DrawIndexedInstancedIndirect, h_DrawIndexedInstancedIndirect, o_DrawIndexedInstancedIndirect);
   PATCH_CTX(DrawInstancedIndirect, h_DrawInstancedIndirect, o_DrawInstancedIndirect);
   PATCH_CTX(Dispatch, h_Dispatch, o_Dispatch);
   PATCH_CTX(CopyResource, h_CopyResource, o_CopyResource);
   PATCH_CTX(CopySubresourceRegion, h_CopySubresourceRegion, o_CopySubresourceRegion);
   PATCH_CTX(ResolveSubresource, h_ResolveSubresource, o_ResolveSubresource);
   PATCH_CTX(ExecuteCommandList, h_ExecuteCommandList, o_ExecuteCommandList);
   p.ready = TRUE;
   LeaveCriticalSection(&lock);
   logf_("D3D11 proxy: profiling the immediate context\n");
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                                       const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk, ID3D11Device **device,
                                                       D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
   ID3D11Device *d = NULL; ID3D11DeviceContext *c = NULL;
   HRESULT hr = real_create(adapter, type, software, flags, levels, level_count, sdk, &d, level, &c);
   if (SUCCEEDED(hr) && d && c) install(d, c);
   if (device) *device = d; else if (d) ID3D11Device_Release(d);
   if (context) *context = c; else if (c) ID3D11DeviceContext_Release(c);
   return hr;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                                                   const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
                                                                   const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap, ID3D11Device **device,
                                                                   D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
   ID3D11Device *d = NULL; ID3D11DeviceContext *c = NULL;
   HRESULT hr = real_create_swap(adapter, type, software, flags, levels, level_count, sdk, desc, swap, &d, level, &c);
   if (SUCCEEDED(hr) && d && c) install(d, c);
   if (device) *device = d; else if (d) ID3D11Device_Release(d);
   if (context) *context = c; else if (c) ID3D11DeviceContext_Release(c);
   return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
   (void)instance; (void)reserved;
   if (reason == DLL_PROCESS_ATTACH) {
      InitializeCriticalSection(&lock);
      char path[MAX_PATH];
      if (GetEnvironmentVariableA("D3D11_PROFILE_LOG", path, sizeof(path))) logfile = fopen(path, "a");
      char system[MAX_PATH];
      GetSystemDirectoryA(system, sizeof(system));
      strcat_s(system, sizeof(system), "\\d3d11.dll");
      HMODULE real = LoadLibraryA(system);
      if (!real) { logf_("D3D11 proxy: cannot load %s\n", system); return FALSE; }
      real_create = (PFN_CREATE)GetProcAddress(real, "D3D11CreateDevice");
      real_create_swap = (PFN_CREATE_SWAP)GetProcAddress(real, "D3D11CreateDeviceAndSwapChain");
      proxy_resolve_stubs(real);
      logf_("D3D11 proxy: loaded, forwarding to %s\n", system);
   } else if (reason == DLL_PROCESS_DETACH) {
      logf_("D3D11 proxy: calls  OMSetRenderTargets immediate %ld other %ld, draws immediate %ld other %ld, ExecuteCommandList %ld, frames ended %ld\n",
            n_omset_immediate, n_omset_other, n_draw_immediate2, n_draw_other2, n_ecl, n_frames_ended);
      logf_("D3D11 proxy: VSSetShader %ld, PSSetShader %ld calls; %u vertex and %u pixel shaders seen\n", n_vsset, n_psset, vs_seen_count, ps_seen_count);
      for (UINT i = 0; i < clear_kinds; ++i)
         logf_("D3D11 proxy: 1536x1536 depth clear slice %u flags %u value %g: %llu times\n", clears[i].slice, clears[i].flags, clears[i].depth, clears[i].count);
      if (p.context) {
         void **now = (void **)p.context->lpVtbl;
         const size_t draw = offsetof(ID3D11DeviceContextVtbl, DrawIndexed) / sizeof(void *);
         const size_t om = offsetof(ID3D11DeviceContextVtbl, OMSetRenderTargets) / sizeof(void *);
         logf_("D3D11 proxy: vtable patched %p now %p; DrawIndexed slot %s, OMSetRenderTargets slot %s\n", (void *)p.vtable, (void *)now,
               now[draw] == (void *)h_DrawIndexed ? "hooked" : "NOT hooked", now[om] == (void *)h_OMSetRenderTargets ? "hooked" : "NOT hooked");
      }
   }
   return TRUE;
}
