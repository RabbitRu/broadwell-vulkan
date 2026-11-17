#include "hasvk_wddm.hpp"
#include <cstring>

namespace hasvk::wddm {
// Empirical ABI profile for 8086:162b + Intel 20.19.15.5171 only. The
// internal-linear allocation class has no image/GMM layout payload. These
// constants reproduce captured state allocations byte-for-byte; byte 40
// encodes their logical size. This is not a general Intel GMM implementation.
std::vector<uint8_t> linear_metadata(uint64_t bytes)
{
   if (!bytes || bytes > kMaxLinearAllocationBytes) throw std::runtime_error("Linear allocation size outside experimental profile");
   std::vector<uint8_t> metadata(1096, 0);
   const uint32_t fields[][2] = {
      {0, 3}, {8, 6}, {12, 0x155}, {16, 8}, {20, 0x20000000},
      {28, 0x4000a}, {48, 1}, {52, 1}, {60, 1}, {64, 0x21},
      {80, 1}, {940, 0x14330000},
   };
   for (const auto &field : fields) memcpy(metadata.data() + field[0], &field[1], 4);
   memcpy(metadata.data() + 40, &bytes, 8);
   return metadata;
}
/* Window surface for redirected present: 2D B8G8R8A8, render target, shared.
 * Captured from the surface dwmapi creates through D3D11 for the Intel OpenGL
 * ICD (tools/trace-gl-present.ps1); only width (0x28) and height (0x30) vary.
 * The kernel driver picks Y tiling, pitch align(width * 4, 128) and height
 * aligned to 32 rows. */
std::vector<uint8_t> window_surface_metadata(uint32_t width, uint32_t height)
{
   std::vector<uint8_t> metadata(1096, 0);
   const uint32_t fields[][2] = {
      {0x0, 3}, {0x8, 2}, {0xc, 0x17}, {0x10, 0x20}, {0x14, 0x400000}, {0x18, 2}, {0x1c, 0x42100008},
      {0x28, width}, {0x30, height}, {0x34, 1}, {0x3c, 1}, {0x40, 0x2e}, {0x4c, 1}, {0x50, 1}, {0x194, 0x15},
      {0x3ac, 0x14330000},
   };
   for (const auto &field : fields) memcpy(metadata.data() + field[0], &field[1], 4);
   return metadata;
}
/* D3D runtime data of that resource (DXGI_FORMAT_B8G8R8A8_UNORM = 0x57). */
std::vector<uint8_t> window_surface_runtime_data(uint32_t width, uint32_t height)
{
   std::vector<uint8_t> data(104, 0);
   const uint32_t fields[][2] = {
      {0x0, 0x68}, {0x4, 4}, {0x38, 3}, {0x3c, width}, {0x40, height}, {0x44, 1}, {0x48, 1}, {0x4c, 0x57},
      {0x50, 1}, {0x5c, 8}, {0x64, 2},
   };
   for (const auto &field : fields) memcpy(data.data() + field[0], &field[1], 4);
   return data;
}
std::vector<uint8_t> command_header_8192()
{
   // Header field 20 has only been validated for an 8192-byte command BO.
   // Do not extrapolate it to larger HasVK batches without differential tests.
   std::vector<uint8_t> header(80, 0);
   uint32_t signature = 0x00171433, client = 4, capacity = 8192;
   memcpy(header.data(), &signature, 4);
   memcpy(header.data() + 8, &client, 4);
   memcpy(header.data() + 20, &capacity, 4);
   return header;
}
} // namespace hasvk::wddm
