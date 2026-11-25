/* What is on the screen, read through the Desktop Duplication API (it sees
 * what the display shows also while another device owns the output
 * exclusively, where GDI's screen DC shows the stale desktop).
 * Saves a BMP and prints the pixels at the given points.
 *
 * Usage: screen_capture.exe <out.bmp> [timeout ms] [x,y ...] */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: screen_capture <out.bmp> [timeout ms] [x,y ...]\n"); return 2; }
   const UINT timeout = argc > 2 ? (UINT)atoi(argv[2]) : 2000;
   SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
   HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
   if (FAILED(hr)) { printf("D3D11CreateDevice %#lx\n", (unsigned long)hr); return 3; }
   IDXGIDevice *dxgi_device = nullptr; device->QueryInterface(&dxgi_device);
   IDXGIAdapter *adapter = nullptr; dxgi_device->GetAdapter(&adapter);
   IDXGIOutput *output = nullptr; hr = adapter->EnumOutputs(0, &output);
   if (FAILED(hr)) { printf("EnumOutputs %#lx\n", (unsigned long)hr); return 4; }
   IDXGIOutput1 *output1 = nullptr; output->QueryInterface(&output1);
   IDXGIOutputDuplication *duplication = nullptr;
   hr = output1->DuplicateOutput(device, &duplication);
   if (FAILED(hr)) { printf("DuplicateOutput %#lx\n", (unsigned long)hr); return 5; }
   /* A frame with a picture has a present time; frames without one carry
    * only pointer updates. Wait for the first picture (the screen must change). */
   DXGI_OUTDUPL_FRAME_INFO info{}; IDXGIResource *resource = nullptr;
   const ULONGLONG deadline = GetTickCount64() + timeout;
   for (;;) {
      hr = duplication->AcquireNextFrame(100, &info, &resource);
      if (SUCCEEDED(hr) && info.LastPresentTime.QuadPart) break;
      if (SUCCEEDED(hr)) { resource->Release(); resource = nullptr; duplication->ReleaseFrame(); }
      else if (hr != DXGI_ERROR_WAIT_TIMEOUT) { printf("AcquireNextFrame %#lx\n", (unsigned long)hr); return 6; }
      if (GetTickCount64() > deadline) { printf("no picture within %u ms (last result %#lx)\n", timeout, (unsigned long)hr); return 6; }
   }
   ID3D11Texture2D *frame = nullptr; resource->QueryInterface(&frame);
   D3D11_TEXTURE2D_DESC desc; frame->GetDesc(&desc);
   D3D11_TEXTURE2D_DESC staging_desc = desc; staging_desc.Usage = D3D11_USAGE_STAGING; staging_desc.BindFlags = 0;
   staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; staging_desc.MiscFlags = 0;
   ID3D11Texture2D *staging = nullptr; device->CreateTexture2D(&staging_desc, nullptr, &staging);
   context->CopyResource(staging, frame);
   D3D11_MAPPED_SUBRESOURCE mapped{};
   hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
   if (FAILED(hr)) { printf("Map %#lx\n", (unsigned long)hr); return 7; }
   printf("frame %ux%u format %d, accumulated frames %u, last present time %lld\n", desc.Width, desc.Height, (int)desc.Format, info.AccumulatedFrames,
          (long long)info.LastPresentTime.QuadPart);
   /* The mouse pointer is not part of the picture; the system reports it apart. */
   if (info.LastMouseUpdateTime.QuadPart)
      printf("pointer %s at (%ld,%ld)\n", info.PointerPosition.Visible ? "visible" : "hidden", info.PointerPosition.Position.x, info.PointerPosition.Position.y);
   else {
      CURSORINFO cursor{}; cursor.cbSize = sizeof(cursor);
      if (GetCursorInfo(&cursor)) printf("pointer (no update in this frame): %s at (%ld,%ld)\n", cursor.flags & CURSOR_SHOWING ? "showing" : "hidden",
                                         cursor.ptScreenPos.x, cursor.ptScreenPos.y);
   }
   auto pixel = [&](UINT x, UINT y) { uint32_t v; memcpy(&v, (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch + x * 4, 4); return v & 0xffffff; };
   for (int a = 3; a < argc; ++a) {
      UINT x = 0, y = 0;
      if (sscanf_s(argv[a], "%u,%u", &x, &y) == 2 && x < desc.Width && y < desc.Height) printf("pixel (%u,%u) = %06x\n", x, y, pixel(x, y));
   }
   BITMAPFILEHEADER file{}; BITMAPINFOHEADER bmp{};
   const DWORD bytes = desc.Width * desc.Height * 4;
   file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(bmp); file.bfSize = file.bfOffBits + bytes;
   bmp.biSize = sizeof(bmp); bmp.biWidth = (LONG)desc.Width; bmp.biHeight = -(LONG)desc.Height; bmp.biPlanes = 1; bmp.biBitCount = 32; bmp.biSizeImage = bytes;
   FILE *out = nullptr;
   if (!fopen_s(&out, argv[1], "wb") && out) {
      fwrite(&file, sizeof(file), 1, out); fwrite(&bmp, sizeof(bmp), 1, out);
      for (UINT y = 0; y < desc.Height; ++y) fwrite((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, 4, desc.Width, out);
      fclose(out);
   }
   context->Unmap(staging, 0);
   duplication->ReleaseFrame();
   return 0;
}
