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
