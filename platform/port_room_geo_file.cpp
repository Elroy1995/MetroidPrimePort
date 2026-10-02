// The .roomgeo file. See port_room_geo.h.
#include "port_room_geo.h"

#include <cmath>
#include <cstring>

namespace PortRoomGeo {
namespace {

constexpr uint32_t kMagic = 0x4752504D; // 'MPRG'
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kInstanceBytes = 4 + 12 * 4;

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomgeo";
  if (fileName.size() != 8 + sizeof(kSuffix) - 1) {
    return false;
  }
  for (size_t i = 0; i + 1 < sizeof(kSuffix); ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c | 0x20) : c) != kSuffix[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error) {
  out.clear();
  if (data.size() < kHeaderBytes || ReadU32(data.data()) != kMagic) {
    error = "not a room geometry file";
    return false;
  }
  if (ReadU32(data.data() + 4) != kVersion) {
    error = "unknown version";
    return false;
  }
  const uint32_t count = ReadU32(data.data() + 8);
  if (count > (data.size() - kHeaderBytes) / kInstanceBytes) {
    error = "truncated";
    return false;
  }
  out.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* const p = data.data() + kHeaderBytes + size_t(i) * kInstanceBytes;
    out[i].model = ReadU32(p);
    for (int j = 0; j < 12; ++j) {
      const uint32_t bits = ReadU32(p + 4 + j * 4);
      std::memcpy(&out[i].transform[j], &bits, 4);
      if (!std::isfinite(out[i].transform[j])) {
        error = "bad transform";
        out.clear();
        return false;
      }
    }
  }
  return true;
}

std::vector<uint8_t> Write(const std::vector<Instance>& instances) {
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + instances.size() * kInstanceBytes);
  PutU32(out, kMagic);
  PutU32(out, kVersion);
  PutU32(out, uint32_t(instances.size()));
  for (const Instance& instance : instances) {
    PutU32(out, instance.model);
    for (int j = 0; j < 12; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.transform[j], 4);
      PutU32(out, bits);
    }
  }
  return out;
}

} // namespace PortRoomGeo
