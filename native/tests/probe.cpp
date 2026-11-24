#include "hasvk_wddm.hpp"
#include "gen8_compute.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace hasvk::wddm;
using Bytes = std::vector<uint8_t>;
using Words = std::vector<uint32_t>;
constexpr uint64_t mask48 = 0xffffffffffffull;

static Bytes read(const fs::path &path)
{
   std::ifstream file(path, std::ios::binary | std::ios::ate);
   if (!file) throw std::runtime_error("Cannot read fixture: " + path.u8string());
   auto size = file.tellg();
   if (size < 0 || size > 64 * 1024 * 1024) throw std::runtime_error("Invalid fixture size");
   Bytes data(static_cast<size_t>(size));
   file.seekg(0);
   if (!file.read(reinterpret_cast<char *>(data.data()), size)) throw std::runtime_error("Short fixture read");
   return data;
}
static void write(const fs::path &path, const void *data, size_t bytes)
{
   std::ofstream file(path, std::ios::binary);
   if (!file.write(static_cast<const char *>(data), bytes)) throw std::runtime_error("Artifact write failed");
}
static json read_json(const fs::path &path)
{
   std::ifstream file(path);
   if (!file) throw std::runtime_error("Missing JSON fixture: " + path.u8string());
   return json::parse(file);
}
static std::string sha256(const Bytes &bytes)
{
   BCRYPT_ALG_HANDLE algorithm = nullptr;
   BCRYPT_HASH_HANDLE hash = nullptr;
   check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "Open SHA256");
   uint8_t digest[32]{};
   try {
      check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0), "Create SHA256");
      check(BCryptHashData(hash, const_cast<uint8_t *>(bytes.data()), static_cast<ULONG>(bytes.size()), 0), "HashData");
      check(BCryptFinishHash(hash, digest, sizeof(digest), 0), "FinishHash");
   } catch (...) { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
   BCryptDestroyHash(hash);
   BCryptCloseAlgorithmProvider(algorithm, 0);
   std::ostringstream text;
   for (uint8_t value : digest) text << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
   return text.str();
}
static Bytes checked_read(const fs::path &path, const std::string &hash)
{
   auto bytes = read(path);
   if (sha256(bytes) != hash) throw std::runtime_error("Fixture SHA256 mismatch: " + path.u8string());
   return bytes;
}
static uint32_t random_word()
{
   uint32_t word;
   check(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&word), sizeof(word), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Random seed");
   return word;
}
static uint32_t mix(uint32_t value)
{
   value ^= value >> 16; value *= 0x7feb352du;
   value ^= value >> 15; value *= 0x846ca68bu;
   return value ^ (value >> 16);
}
static uint64_t u64(const void *memory, size_t offset)
{
   uint64_t value; memcpy(&value, static_cast<const uint8_t *>(memory) + offset, 8); return value;
}
static void put64(void *memory, size_t offset, uint64_t value)
{
   memcpy(static_cast<uint8_t *>(memory) + offset, &value, 8);
}
static void put32(void *memory, size_t offset, uint32_t value)
{
   memcpy(static_cast<uint8_t *>(memory) + offset, &value, 4);
}
static uint64_t address(const json &value) { return std::stoull(value.get<std::string>(), nullptr, 16) & mask48; }
static void verify(Buffer &buffer, const Words &expected)
{
   const auto *actual = static_cast<const uint32_t *>(buffer.cpu());
   for (size_t i = 0; i < expected.size(); ++i)
      if (actual[i] != expected[i]) throw std::runtime_error("GPU mismatch at word " + std::to_string(i));
}
static Bytes submit_header(const fs::path &bootstrap)
{
   return checked_read(bootstrap / "submit-header.bin", "f0dee23679c6f9cb6d7de450ac10a94f0e71056fcaf7e4a690be2c0a23ce490b");
}
static Bytes command_metadata(const fs::path &bootstrap)
{
   return checked_read(bootstrap / "buffer-8192.bin", "c556da668a4af55e8c69ae99821604c7e7fc417512e83c1acb2b1073a1655ada");
}
static Bytes result_metadata(const fs::path &bootstrap)
{
   return checked_read(bootstrap / "buffer-4096.bin", "a0e961d199ce684b7f940bf5607d5f33bd7a21ae0fa007acda95ec4b14c50078");
}
static json store(Device &device, const fs::path &bootstrap, const fs::path &output, uint64_t exact_va = 0,
                  bool generated = false, uint64_t result_bytes = 4096)
{
   if (result_bytes < 4096 || result_bytes > 16 * 1024 * 1024 || (result_bytes & 4095))
      throw std::runtime_error("Invalid store test size");
   Buffer commands(device, generated ? linear_metadata(8192) : command_metadata(bootstrap), 8192);
   Buffer result(device, generated ? linear_metadata(result_bytes) : result_metadata(bootstrap), result_bytes, 1, exact_va);
   Words expected(static_cast<size_t>(result_bytes / 4), 0xcdcdcdcdu), batch;
   memcpy(result.cpu(), expected.data(), static_cast<size_t>(result_bytes));
   uint32_t seed = random_word();
   Words indices{0u, 7u, 255u, uint32_t(expected.size() - 1)};
   if (generated) {
      uint32_t pages = static_cast<uint32_t>(result_bytes / 4096);
      for (uint32_t page = 0; page < pages; page += std::max(1u, pages / 32)) {
         indices.push_back(page * 1024); indices.push_back(page * 1024 + 1023);
      }
      std::sort(indices.begin(), indices.end());
      indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
   }
   for (uint32_t index : indices) {
      uint64_t target = result.address() + 4 * index;
      uint32_t value = mix(seed ^ index);
      if (value == 0xcdcdcdcdu) value ^= 1;
      expected[index] = value;
      batch.insert(batch.end(), {0x10000002u, uint32_t(target), uint32_t(target >> 32), value});
   }
   batch.insert(batch.end(), {0x05000000u, 0u});
   memset(commands.cpu(), 0, 8192);
   memcpy(commands.cpu(), batch.data(), batch.size() * 4);
   device.submit_and_wait(commands.address(), static_cast<uint32_t>(batch.size() * 4), generated ? command_header_8192() : submit_header(bootstrap));
   verify(result, expected);
   write(output / "store-batch.bin", batch.data(), batch.size() * 4);
   write(output / "store-readback.bin", result.cpu(), static_cast<size_t>(result_bytes));
   return {{"passed", true}, {"seed", seed}, {"wordsVerifiedIncludingGuards", expected.size()},
           {"guardWordsVerified", expected.size() - indices.size()}, {"gpuWritesVerified", indices.size()},
           {"gpuVa", result.address()}, {"exactVa", exact_va},
           {"allocationBytes", result_bytes}, {"generatedAllocationMetadata", generated}, {"generatedSubmitHeader", generated}};
}

struct Packet { std::string name; uint64_t va; Words words; };
static std::vector<Packet> packets(const fs::path &directory)
{
   std::vector<Packet> result;
   for (const auto &item : read_json(directory / "decoded-packets.json")) {
      Packet packet{item.at("Name"), address(item.at("Address")), {}};
      for (const auto &word : item.at("Words")) packet.words.push_back(static_cast<uint32_t>(std::stoul(word.get<std::string>(), nullptr, 16)));
      result.push_back(std::move(packet));
   }
   return result;
}
static const Packet &pick(const std::vector<Packet> &packets, const std::string &name,
                          const std::function<bool(const Packet &)> &predicate = {}, bool last = false)
{
   const Packet *selected = nullptr;
   for (const auto &packet : packets) if (packet.name == name && (!predicate || predicate(packet))) {
      selected = &packet; if (!last) break;
   }
   if (!selected) throw std::runtime_error("Required packet absent: " + name);
   return *selected;
}
static uint64_t packet_base(const Words &words, size_t i)
{
   return (uint64_t(words.at(i + 1)) << 32 | (words.at(i) & 0xfffff000u)) & mask48;
}
static void set_base(Words &words, size_t i, uint64_t va)
{
   va = canonical(va);
   words.at(i) = uint32_t(va) | (words.at(i) & 0xfff);
   words.at(i + 1) = uint32_t(va >> 32);
}
static json allocation_at(const json &allocations, uint64_t va)
{
   va &= mask48;
   for (const auto &allocation : allocations) {
      uint64_t start = address(allocation.at("address"));
      if (va >= start && va - start < allocation.at("bytes").get<uint64_t>()) return allocation;
   }
   throw std::runtime_error("Compute address is absent from snapshot");
}
static Bytes capture_file(const fs::path &directory, const json &allocation, bool metadata)
{
   auto name = allocation.at(metadata ? "metadataFile" : "memoryFile").get<std::string>();
   fs::path path = fs::u8path(name);
   if (path != path.filename()) throw std::runtime_error("Invalid capture filename");
   auto bytes = checked_read(directory / path, allocation.at(metadata ? "metadataSha256" : "memorySha256"));
   size_t expected = metadata ? 1096 : allocation.at("bytes").get<size_t>();
   if (bytes.size() != expected) throw std::runtime_error("Capture file size mismatch");
   return bytes;
}
static json profile(const fs::path &directory, const fs::path &bootstrap)
{
   auto capture = read_json(directory / "capture.json");
   size_t compared = 0;
   json sizes = json::array();
   for (const auto &allocation : capture.at("allocations")) {
      auto metadata = capture_file(directory, allocation, true);
      if (u64(metadata.data(), 20) != 0x20000000 || u64(metadata.data(), 64) != 0x21) continue;
      uint64_t logical_bytes = u64(metadata.data(), 40);
      if (linear_metadata(logical_bytes) != metadata) throw std::runtime_error("Generated linear profile differs from Intel capture");
      ++compared; sizes.push_back(logical_bytes);
   }
   if (compared < 3 || command_header_8192() != submit_header(bootstrap)) throw std::runtime_error("Insufficient or mismatching ABI fixtures");
   return {{"passed", true}, {"capturedDescriptorsCompared", compared}, {"logicalSizes", sizes}, {"submitHeaderMatches", true}};
}
static json va_pool(Device &device, const fs::path &output)
{
   constexpr uint64_t window_bytes = 1ull << 32;
   uint64_t base = device.reserve_va(window_bytes);
   json mappings = json::array();
   try {
      Buffer commands(device, linear_metadata(8192), 8192);
      std::vector<std::unique_ptr<Buffer>> buffers;
      std::vector<Words> expected;
      Words batch;
      for (uint64_t offset : {0ull, 65536ull, window_bytes - 4096}) {
         auto buffer = std::make_unique<Buffer>(device, linear_metadata(4096), 4096, 1, base + offset);
         memset(buffer->cpu(), 0xcd, 4096);
         Words values(1024, 0xcdcdcdcdu);
         for (uint32_t index : {0u, 1023u}) {
            uint64_t target = buffer->address() + index * 4;
            values[index] = random_word() ^ 0x13579bdfu;
            batch.insert(batch.end(), {0x10000002u, uint32_t(target), uint32_t(target >> 32), values[index]});
         }
         mappings.push_back({{"offset", offset}, {"gpuVa", buffer->address()}});
         buffers.push_back(std::move(buffer)); expected.push_back(std::move(values));
      }
      batch.insert(batch.end(), {0x05000000u, 0u});
      memset(commands.cpu(), 0, 8192); memcpy(commands.cpu(), batch.data(), batch.size() * 4);
      device.submit_and_wait(commands.address(), static_cast<uint32_t>(batch.size() * 4), command_header_8192());
      for (size_t i = 0; i < buffers.size(); ++i) verify(*buffers[i], expected[i]);
      write(output / "va-pool-batch.bin", batch.data(), batch.size() * 4);
   } catch (...) { device.free_va(base, window_bytes); throw; }
   device.free_va(base, window_bytes);
   return {{"passed", true}, {"reservedBytes", window_bytes}, {"base", base},
           {"simultaneousMappings", mappings}, {"wordsVerifiedIncludingGuards", 3072}, {"generatedAllocationMetadata", true}};
}
static json va_layout(Device &device, const fs::path &output, bool low = false)
{
   // Match the independently reserved address domains used by pinned HasVK.
   const std::pair<uint64_t, uint64_t> ranges[] = {
      {0x200000, 0x3fe00000}, {0x40000000, 0x40000000}, {0xc0000000, 0x40000000},
      {0x100000000, 0x40000000}, {0x140000000, 0x40000000}, {0x180000000, 0x40000000},
      {0x1c0000000, 0x100000000}, {0x2c0000000, 0x40000000},
   };
   std::vector<std::pair<uint64_t, uint64_t>> reservations;
   json mappings = json::array();
   uint64_t origin = 0;
   try {
      if (!low) {
         uint64_t reserved = device.reserve_va(16ull << 30);
         reservations.emplace_back(reserved, 16ull << 30);
         origin = (reserved + 0xffffffffull) & ~0xffffffffull;
      }
      Buffer commands(device, linear_metadata(8192), 8192);
      std::vector<std::unique_ptr<Buffer>> buffers;
      std::vector<Words> expected;
      Words batch;
      for (const auto &range : ranges) {
         uint64_t base = origin + range.first;
         if (low) {
            uint64_t actual = device.reserve_va(range.second, base);
            reservations.emplace_back(actual, range.second);
            if (actual != base) throw std::runtime_error("HasVK pool VA was not reserved exactly");
         }
         for (uint64_t offset : {0ull, range.second - 4096}) {
            auto buffer = std::make_unique<Buffer>(device, linear_metadata(4096), 4096, 1, base + offset);
            memset(buffer->cpu(), 0x92, 4096);
            Words values(1024, 0x92929292u);
            for (uint32_t index : {0u, 1023u}) {
               uint64_t target = buffer->address() + index * 4;
               values[index] = random_word();
               batch.insert(batch.end(), {0x10000002u, uint32_t(target), uint32_t(target >> 32), values[index]});
            }
            mappings.push_back({{"gpuVa", buffer->address()}, {"poolBase", base}, {"poolSize", range.second}});
            buffers.push_back(std::move(buffer)); expected.push_back(std::move(values));
         }
      }
      batch.insert(batch.end(), {0x05000000u, 0u});
      memset(commands.cpu(), 0, 8192); memcpy(commands.cpu(), batch.data(), batch.size() * 4);
      device.submit_and_wait(commands.address(), static_cast<uint32_t>(batch.size() * 4), command_header_8192());
      for (size_t i = 0; i < buffers.size(); ++i) verify(*buffers[i], expected[i]);
      write(output / "va-layout-batch.bin", batch.data(), batch.size() * 4);
   } catch (...) {
      for (const auto &range : reservations) device.free_va(range.first, range.second);
      throw;
   }
   for (const auto &range : reservations) device.free_va(range.first, range.second);
   return {{"passed", true}, {"poolReservations", reservations.size()}, {"layoutOrigin", origin}, {"mappings", mappings},
           {"wordsVerifiedIncludingGuards", mappings.size() * 1024}};
}
static json batch_chain(Device &device, const fs::path &output)
{
   VaRange range(device, 65536);
   Buffer commands(device, linear_metadata(8192), 8192);
   Buffer result(device, linear_metadata(4096), 4096);
   constexpr size_t tail_offset = 24576;
   Words expected(1024, 0x61616161u);
   memcpy(result.cpu(), expected.data(), 4096);
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      // Destroy/recreate the chained BO at the same reserved address. This
      // checks mapping reuse and that completion precedes every destruction.
      Buffer chained(device, linear_metadata(32768), 32768, 1, range.address());
      Words tail;
      for (uint32_t index : {0u, 511u, 1023u}) {
         uint64_t target = result.address() + index * 4;
         expected[index] = random_word();
         tail.insert(tail.end(), {0x10000002u, uint32_t(target), uint32_t(target >> 32), expected[index]});
      }
      tail.insert(tail.end(), {0x05000000u, 0u});
      memset(chained.cpu(), 0, 32768);
      memcpy(static_cast<uint8_t *>(chained.cpu()) + tail_offset, tail.data(), tail.size() * 4);
      memset(commands.cpu(), 0, 8192);
      size_t bytes = hasvk_gen8_batch_jump(commands.cpu(), chained.address() + tail_offset);
      if (!bytes) throw std::runtime_error("Batch jump generation failed");
      device.submit_and_wait(commands.address(), static_cast<uint32_t>(bytes), command_header_8192());
      verify(result, expected);
      if (iteration == 31) {
         write(output / "entry-batch.bin", commands.cpu(), bytes);
         write(output / "chained-batch.bin", chained.cpu(), 32768);
      }
   }
   return {{"passed", true}, {"entryBytes", 16}, {"chainedAllocationBytes", 32768}, {"iterations", 32},
           {"sameReservedAddressReused", true}, {"chainedStartOffset", tail_offset},
           {"wordsVerifiedIncludingGuards", expected.size() * 32}};
}
static json compute(Device &device, const fs::path &directory, const fs::path &bootstrap,
                    const fs::path &output, uint32_t count, bool generated = false)
{
   const auto capture = read_json(directory / "capture.json");
   const auto parameters = capture.at("parameters");
   if (capture.at("driverVersion") != "20.19.15.5171" || parameters.at("hlslSha256") != "feaa223a6382390994b235b082e4688e274a7de004a69b5df98f731b6879c0b0")
      throw std::runtime_error("Unsupported compute snapshot");
   const auto allocations = capture.at("allocations");
   auto stream = packets(directory);
   auto walker = pick(stream, "GPGPU_WALKER");
   if (walker.words.size() != 15 || walker.words[0] != 0x7105000d || walker.words[4] != 0x40000300)
      throw std::runtime_error("Unsupported walker layout");
   auto bases = pick(stream, "STATE_BASE_ADDRESS", [&](const Packet &p) { return p.va < walker.va && packet_base(p.words, 6) && packet_base(p.words, 10); });
   auto surface_bases = pick(stream, "STATE_BASE_ADDRESS", [&](const Packet &p) { return p.va < walker.va && (p.words[4] & 1) && packet_base(p.words, 4); }, true);
   auto descriptor_load = pick(stream, "MEDIA_INTERFACE_DESCRIPTOR_LOAD", [&](const Packet &p) { return p.va < walker.va; }, true);
   uint64_t curbe_va = packet_base(bases.words, 8) + walker.words[3];
   uint64_t descriptor_va = packet_base(bases.words, 6) + descriptor_load.words[3];
   auto state_record = allocation_at(allocations, curbe_va);
   auto descriptor_record = allocation_at(allocations, descriptor_va);
   if (state_record.at("address") != descriptor_record.at("address")) throw std::runtime_error("Separate descriptor/CURBE heaps unsupported by this regression");
   auto state_bytes = capture_file(directory, state_record, false);
   size_t descriptor_offset = static_cast<size_t>(descriptor_va - address(state_record.at("address")));
   if (descriptor_offset + 8 > state_bytes.size()) throw std::runtime_error("Descriptor outside heap");
   uint64_t kernel_va = packet_base(bases.words, 10) + (u64(state_bytes.data(), descriptor_offset) & 0xffffffffffc0ull);
   auto code_record = allocation_at(allocations, kernel_va);
   auto surface_record = allocation_at(allocations, packet_base(surface_bases.words, 4));
   auto input_record = allocation_at(allocations, address(parameters.at("inputAddress")));
   auto output_record = allocation_at(allocations, address(parameters.at("outputAddress")));
   auto upload_record = allocation_at(allocations, address(parameters.at("uploadAddress")));
   size_t buffer_bytes = input_record.at("bytes");
   if (!count || count > 16381 || buffer_bytes != 65536 || output_record.at("bytes") != buffer_bytes)
      throw std::runtime_error("Unsupported compute count/buffer shape");
   auto load_state = [&](const json &record) {
      if (!record.at("cpuReadSucceeded").get<bool>()) throw std::runtime_error("Snapshot contains unreadable state");
      auto memory = capture_file(directory, record, false);
      auto buffer = std::make_unique<Buffer>(device, generated ? linear_metadata(memory.size()) : capture_file(directory, record, true), memory.size(), 0);
      memcpy(buffer->cpu(), memory.data(), memory.size());
      return buffer;
   };
   auto state = load_state(state_record), code = load_state(code_record), surfaces = load_state(surface_record);
   auto cpu_metadata = generated ? linear_metadata(buffer_bytes) : capture_file(directory, upload_record, true);
   Buffer input(device, cpu_metadata, buffer_bytes), result(device, cpu_metadata, buffer_bytes);
   Buffer scratch(device, generated ? linear_metadata(4096) : result_metadata(bootstrap), 4096);
   Buffer commands(device, generated ? linear_metadata(8192) : command_metadata(bootstrap), 8192);
   uint32_t seed = random_word(), salt = random_word();
   Words input_words(buffer_bytes / 4), expected(buffer_bytes / 4, 0xcdcdcdcd);
   for (size_t i = 0; i < input_words.size(); ++i) {
      input_words[i] = mix(seed + uint32_t(i) * 0x9e3779b9u);
      if (i < count) expected[i] = mix(input_words[i] ^ salt ^ (uint32_t(i) * 0x9e3779b9u));
   }
   memcpy(input.cpu(), input_words.data(), buffer_bytes);
   memset(result.cpu(), 0xcd, buffer_bytes);
   size_t curbe_offset = static_cast<size_t>(curbe_va - address(state_record.at("address")));
   if (curbe_offset + 48 > state->size() || (u64(state->cpu(), curbe_offset + 32) & mask48) != address(parameters.at("inputAddress")) ||
       (u64(state->cpu(), curbe_offset + 40) & mask48) != address(parameters.at("outputAddress")))
      throw std::runtime_error("Captured root pointer layout mismatch");
   put32(state->cpu(), curbe_offset, count); put32(state->cpu(), curbe_offset + 4, salt);
   put64(state->cpu(), curbe_offset + 32, canonical(input.address()));
   put64(state->cpu(), curbe_offset + 40, canonical(result.address()));
   auto kernel_offset = kernel_va - address(code_record.at("address"));
   auto descriptor_kernel = u64(state->cpu(), descriptor_offset);
   put64(state->cpu(), descriptor_offset, (descriptor_kernel & ~0xffffffffffc0ull) | kernel_offset);
   unsigned pointer_patches = 0;
   for (size_t i = 0; i + 8 <= surfaces->size(); i += 4) {
      uint64_t va = u64(surfaces->cpu(), i) & mask48;
      if (va == address(parameters.at("inputAddress"))) { put64(surfaces->cpu(), i, canonical(input.address())); ++pointer_patches; }
      else if (va == address(parameters.at("outputAddress"))) { put64(surfaces->cpu(), i, canonical(result.address())); ++pointer_patches; }
   }
   Words batch;
   json emitted = json::array();
   auto emit = [&](const char *name, const Words &words) { emitted.push_back(name); batch.insert(batch.end(), words.begin(), words.end()); };
   emit("PIPE_CONTROL", pick(stream, "PIPE_CONTROL").words);
   emit("PIPELINE_SELECT", pick(stream, "PIPELINE_SELECT", [](const Packet &p) { return (p.words[0] & 3) == 2; }).words);
   /* Intel's dummy MEDIA_VFE_STATE behind the switch to GPGPU (see gen8_compute.c). */
   emit("MEDIA_VFE_STATE", {0x70000007, 0, 0, 0x00000140, 0, 0x000001df, 0, 0, 0});
   for (uint32_t reg : {0x20c0u, 0x7004u, 0x7034u})
      emit("MI_LOAD_REGISTER_IMM", pick(stream, "MI_LOAD_REGISTER_IMM", [&](const Packet &p) { return p.va < walker.va && p.words.size() == 3 && p.words[1] == reg; }, true).words);
   auto state_bases = bases.words;
   state_bases[1] &= 0xffe; state_bases[2] = 0;
   state_bases[4] = surface_bases.words[4]; state_bases[5] = surface_bases.words[5];
   set_base(state_bases, 4, surfaces->address()); set_base(state_bases, 6, state->address());
   set_base(state_bases, 8, state->address()); set_base(state_bases, 10, code->address());
   emit("STATE_BASE_ADDRESS", state_bases);
   emit("PIPE_CONTROL", {0x7a000004, 0x01000c1c, 0, 0, 0, 0});
   emit("MEDIA_VFE_STATE", pick(stream, "MEDIA_VFE_STATE", [&](const Packet &p) { return p.va < walker.va; }, true).words);
   auto descriptor_packet = descriptor_load.words;
   descriptor_packet[3] = static_cast<uint32_t>(descriptor_offset);
   emit("MEDIA_INTERFACE_DESCRIPTOR_LOAD", descriptor_packet);
   emit("MI_LOAD_REGISTER_IMM", {0x11000001, 0x2600, 0xffffffff});
   auto dispatch = walker.words;
   dispatch[3] = static_cast<uint32_t>(curbe_offset); dispatch[7] = (count + 63) / 64;
   emit("GPGPU_WALKER", dispatch);
   emit("MI_LOAD_REGISTER_IMM", {0x11000001, 0x2600, 0});
   emit("MEDIA_STATE_FLUSH", {0x70040000, 0});
   emit("PIPE_CONTROL", {0x7a000004, 0x00105021, uint32_t(scratch.address()), uint32_t(scratch.address() >> 32), 0, 0});
   emit("MI_BATCH_BUFFER_END", {0x05000000});
   if (batch.size() & 1) batch.push_back(0);
   if (batch.size() * 4 > 8180) throw std::runtime_error("Compute batch exceeds command buffer");
   memset(commands.cpu(), 0, 8192);
   memcpy(commands.cpu(), batch.data(), batch.size() * 4);
   write(output / "compute-batch.bin", batch.data(), batch.size() * 4);
   device.submit_and_wait(commands.address(), static_cast<uint32_t>(batch.size() * 4), generated ? command_header_8192() : submit_header(bootstrap));
   write(output / "compute-readback.bin", result.cpu(), buffer_bytes);
   verify(result, expected);
   return {{"passed", true}, {"elementsVerified", count}, {"guardWordsVerified", expected.size() - count},
           {"seed", seed}, {"salt", salt}, {"groupsX", dispatch[7]}, {"nativeBatchBytes", batch.size() * 4},
           {"emittedPackets", emitted}, {"surfacePointerPatches", pointer_patches},
           {"capturedFullBatchReplayed", false}, {"usesCapturedKernelAndState", true},
           {"usesCapturedAllocationMetadata", !generated}, {"generatedSubmitHeader", generated}};
}
static json forbidden_modules()
{
   HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
   if (snapshot == INVALID_HANDLE_VALUE) throw Error("Module snapshot", HRESULT_FROM_WIN32(GetLastError()));
   MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
   json found = json::array();
   if (!Module32FirstW(snapshot, &entry)) { CloseHandle(snapshot); throw std::runtime_error("Cannot enumerate modules"); }
   do {
      std::wstring name = entry.szModule;
      std::transform(name.begin(), name.end(), name.begin(), towlower);
      if (name.rfind(L"d3d12", 0) == 0 || name.rfind(L"igd", 0) == 0)
         found.push_back(fs::path(name).u8string());
   } while (Module32NextW(snapshot, &entry));
   DWORD error = GetLastError();
   CloseHandle(snapshot);
   if (error != ERROR_NO_MORE_FILES) throw Error("Module32NextW", HRESULT_FROM_WIN32(error));
   return found;
}
static json elk_compute(Device &device, const fs::path &kernel_directory, const fs::path &output)
{
   auto metadata = read_json(kernel_directory / "kernel.json");
   auto code = checked_read(kernel_directory / "kernel.bin", metadata.at("kernelSha256"));
   uint32_t mask = metadata.at("progMask");
   unsigned variant = mask == 1 ? 0 : mask == 2 ? 1 : mask == 4 ? 2 : 3;
   bool push_supported = metadata.at("nrParams") == 0 && metadata.at("perThreadRegs") == 0;
   push_supported |= metadata.at("nrParams") == 1 && metadata.at("perThreadRegs") == 1 && metadata.value("pushSubgroupIdOnly", false);
   if (metadata.at("compiler") != "Mesa 26.2.7 ELK" || metadata.at("gen") != 8 || !metadata.at("isaValidated").get<bool>() ||
       code.empty() || code.size() > 4096 || metadata.at("programBytes") != code.size() || variant == 3 ||
       !push_supported || metadata.at("crossThreadRegs") != 0 ||
       metadata.at("scratchBytes") != 0 || metadata.at("sharedBytes") != 0)
      throw std::runtime_error("ELK smoke kernel requires a single SIMD variant and no push/scratch/shared storage");
   uint32_t offset = metadata.at("programOffsets").at(variant);
   if (offset >= code.size() || (offset & 63)) throw std::runtime_error("Invalid ELK program offset");
   const auto local = metadata.value("localSize", std::vector<uint32_t>{1, 1, 1});
   uint32_t threads = metadata.value("dispatchThreads", 1u);
   if (local.size() != 3 || !local[0] || local[0] > 64 || local[1] != 1 || local[2] != 1 ||
       threads != (local[0] + (8u << variant) - 1) / (8u << variant) ||
       metadata.value("usesBarrier", false)) throw std::runtime_error("Unsupported dispatch geometry");
   uint32_t count = metadata.value("elementCount", 1u);
   if (!count || count > 16381) throw std::runtime_error("Unsupported test element count");
   uint32_t result_bytes = metadata.contains("elementCount") ? ((count + 32) * 4 + 4095) & ~4095u : 4096;
   VaRange layout(device, 16ull << 30);
   uint64_t origin = (layout.address() + 0xffffffffull) & ~0xffffffffull;
   constexpr uint32_t pool_offset = 32768;
   Buffer instructions(device, linear_metadata(4096), 4096, 0, origin + 0x180000000 + pool_offset);
   Buffer dynamic(device, linear_metadata(4096), 4096, 0, origin + 0xc0000000 + pool_offset);
   Buffer surfaces(device, linear_metadata(4096), 4096, 0, origin + 0x140000000 + pool_offset);
   Buffer result(device, linear_metadata(result_bytes), result_bytes, 1, origin + 0x1c0000000);
   Buffer scratch(device, linear_metadata(4096), 4096, 1, origin + 0x200000);
   Buffer commands(device, linear_metadata(8192), 8192, 1, origin + 0x2c0000000);
   memset(instructions.cpu(), 0, 4096); memcpy(instructions.cpu(), code.data(), code.size());
   uint32_t value = metadata.at("expectedValue"), poison = value ^ 0xa5a5a5a5u;
   Words expected(result_bytes / 4, poison);
   memcpy(result.cpu(), expected.data(), result_bytes);
   if (metadata.contains("initialValue")) {
      uint32_t initial = metadata.at("initialValue");
      if (initial == value) throw std::runtime_error("Initial value must differ from expected GPU result");
      *static_cast<uint32_t *>(result.cpu()) = initial;
   }
   expected[0] = value;
   if (metadata.contains("elementCount")) {
      const auto vectors = read_json(kernel_directory / "vectors.json");
      const auto initial = vectors.at("initialWords").get<Words>();
      const auto wanted = vectors.at("expectedWords").get<Words>();
      if (initial.size() != count || wanted.size() != count) throw std::runtime_error("Invalid reference array size");
      std::copy(initial.begin(), initial.end(), static_cast<uint32_t *>(result.cpu()) + 16);
      std::copy(wanted.begin(), wanted.end(), expected.begin() + 16);
      *static_cast<uint32_t *>(result.cpu()) = expected[0] = count;
   }
   memset(scratch.cpu(), 0, 4096);
   hasvk_gen8_compute_info info{};
   info.instruction_base = instructions.address() - pool_offset; info.dynamic_base = dynamic.address() - pool_offset;
   info.surface_base = surfaces.address() - pool_offset; info.output_base = result.address(); info.scratch_base = scratch.address();
   info.instruction_offset = info.dynamic_offset = info.surface_offset = pool_offset;
   info.kernel_offset = offset; info.simd_width = 8u << variant;
   info.per_thread_regs = metadata.at("perThreadRegs");
   info.output_bytes = result_bytes; info.dispatch_threads = threads;
   info.right_mask = metadata.value("rightMask", 1u); info.group_count_x = (count + local[0] - 1) / local[0];
   info.max_threads = metadata.at("maxCsThreads").get<uint32_t>() * metadata.at("subsliceTotal").get<uint32_t>();
   size_t batch_bytes = hasvk_gen8_constant_compute(&info, commands.cpu(), 8192, dynamic.cpu(), surfaces.cpu());
   if (!batch_bytes) throw std::runtime_error("Gen8 compiler smoke state generation failed");
   write(output / "elk-batch.bin", commands.cpu(), batch_bytes);
   write(output / "elk-dynamic-state.bin", dynamic.cpu(), 4096);
   write(output / "elk-surface-state.bin", surfaces.cpu(), 4096);
   write(output / "elk-before-submit.bin", result.cpu(), result_bytes);
   device.submit_and_wait(commands.address(), static_cast<uint32_t>(batch_bytes), command_header_8192());
   write(output / "elk-readback.bin", result.cpu(), result_bytes);
   write(output / "elk-completion.bin", scratch.cpu(), 4096);
   if (*static_cast<uint64_t *>(scratch.cpu()) != 1) throw std::runtime_error("ELK completion write missing");
   verify(result, expected);
   return {{"passed", true}, {"compiler", metadata.at("compiler")}, {"compilerInput", metadata.at("input")},
           {"kernelSha256", sha256(code)}, {"expectedValue", metadata.contains("elementCount") ? expected[16] : value},
           {"actualValue", static_cast<uint32_t *>(result.cpu())[metadata.contains("elementCount") ? 16 : 0]},
           {"guardWordsVerified", expected.size() - count - (metadata.contains("elementCount") ? 1 : 0)},
           {"elementCount", count}, {"groupCountX", info.group_count_x}, {"dispatchThreads", threads},
           {"relocatedHasvkLayoutOrigin", origin}, {"statePoolOffset", pool_offset},
           {"localSize", local}, {"simdWidth", info.simd_width}, {"nativeBatchBytes", batch_bytes},
           {"capturedKernelOrStateUsed", false}, {"generatedAllocationMetadata", true}, {"stateGeneratedFromMesaGenxml", true}};
}
int wmain(int argc, wchar_t **argv)
{
   _set_error_mode(_OUT_TO_STDERR);
   if (argc != 6) {
      std::cerr << "Usage: HasvkWddmProbe.exe store|compute|va|generated-store|generated-compute COUNT_OR_BYTES CAPTURE_DIR BOOTSTRAP_DIR OUTPUT_DIR\n";
      return 2;
   }
   fs::path output(argv[5]);
   json report = {{"schema", 1}, {"implementation", "native C++ WDDM"}, {"isVulkanDriver", false},
                  {"usesCapturedIntelMetadata", true}, {"hardware", "8086:162b"}, {"driverVersion", "20.19.15.5171"}};
   int status = 1;
   try {
      fs::create_directories(output);
      wchar_t module_path[32768];
      DWORD module_length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
      if (!module_length || module_length >= std::size(module_path)) throw std::runtime_error("Cannot identify probe executable");
      report["executableSha256"] = sha256(read(fs::path(module_path)));
      std::wstring mode(argv[1]);
      report["mode"] = fs::path(mode).u8string();
      bool generated = mode == L"generated-store" || mode == L"generated-compute" || mode == L"va-pool" ||
                       mode == L"va-layout" || mode == L"va-layout-low" || mode == L"batch-chain" || mode == L"elk";
      report["usesCapturedIntelMetadata"] = !generated;
      Device device;
      if (mode == L"store") report["store"] = store(device, argv[4], output);
      else if (mode == L"compute") report["compute"] = compute(device, argv[3], argv[4], output, static_cast<uint32_t>(std::stoul(argv[2])));
      else if (mode == L"generated-store") report["store"] = store(device, argv[4], output, 0, true, std::stoull(argv[2]));
      else if (mode == L"generated-compute") report["compute"] = compute(device, argv[3], argv[4], output, static_cast<uint32_t>(std::stoul(argv[2])), true);
      else if (mode == L"profile") report["profile"] = profile(argv[3], argv[4]);
      else if (mode == L"va-pool") report["vaPool"] = va_pool(device, output);
      else if (mode == L"va-layout") report["vaLayout"] = va_layout(device, output);
      else if (mode == L"va-layout-low") report["vaLayout"] = va_layout(device, output, true);
      else if (mode == L"batch-chain") report["batchChain"] = batch_chain(device, output);
      else if (mode == L"elk") report["elkCompute"] = elk_compute(device, argv[3], output);
      else if (mode == L"va") {
         uint64_t reservation = device.reserve_va(65536);
         try { report["store"] = store(device, argv[4], output, reservation); }
         catch (...) { device.free_va(reservation, 65536); throw; }
         device.free_va(reservation, 65536);
         report["reservedVaBytes"] = 65536;
         report["exactMappingValidated"] = true;
      } else throw std::runtime_error("Unknown probe mode");
      device.shutdown();
      report["cleanupValidated"] = true;
      auto modules = forbidden_modules();
      report["forbiddenModules"] = modules;
      if (!modules.empty()) throw std::runtime_error("Intel UMD/D3D12 loaded in native process");
      status = 0;
   } catch (const std::exception &error) { report["error"] = error.what(); std::cerr << error.what() << '\n'; }
   report["overallPassed"] = status == 0;
   try { std::ofstream file(output / "report.json"); file << report.dump(2); if (!file) return 1; }
   catch (...) { return 1; }
   std::cout << report.dump(2) << '\n';
   return status;
}
