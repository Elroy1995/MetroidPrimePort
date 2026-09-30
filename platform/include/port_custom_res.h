#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Resources that are on no disc: randomprime's custom pickup models
// (custom_assets.rs), built from disc models and its MIT textures the first
// time something asks for them. CResLoader answers these ids from here.
namespace PortCustomRes {

// randomprime's ids, so its seeds' model choices mean the same thing here.
enum : uint32_t {
  kPhazonSuitTxtr1 = 0xDEAF0000,
  kPhazonSuitTxtr2 = 0xDEAF0001,
  kPhazonSuitCmdl = 0xDEAF0002,
  kPhazonSuitAncs = 0xDEAF0003,
  kNothingTxtr = 0xDEAF0004,
  kNothingCmdl = 0xDEAF0005,
  kNothingAncs = 0xDEAF0006,
  kZoomerCmdl = 0xDEAF0007,
  kZoomerAncs = 0xDEAF0008,
  kCogCmdl = 0xDEAF0009,
  kCogAncs = 0xDEAF000A,
  kThermalCmdl = 0xDEAF000B,
  kThermalAncs = 0xDEAF000C,
  kXrayCmdl = 0xDEAF000D,
  kXrayAncs = 0xDEAF000E,
  kCombatCmdl = 0xDEAF000F,
  kCombatAncs = 0xDEAF0010,
};

inline bool IsCustomId(uint32_t id) { return (id & 0xFFFF0000u) == 0xDEAF0000u; }

struct Resource {
  uint32_t type = 0; // FourCC
  std::vector<uint8_t> data; // uncompressed
};

// Reads a disc resource, decompressed. False when the disc lacks it.
using DiscReader = std::function<bool(uint32_t id, std::vector<uint8_t>& out)>;

// The resource for a custom id, built on first use; null for an unknown id or
// when its disc source is missing or not the expected layout. Pointers stay
// valid for the whole run.
const Resource* Find(uint32_t id, const DiscReader& read);

// In-place patches (exposed for the unit test). False, and data unchanged,
// when the layout isn't what randomprime's recipe expects.
bool SetCmdlTexture(std::vector<uint8_t>& cmdl, uint32_t index, uint32_t texture);
bool SetAncsModel(std::vector<uint8_t>& ancs, uint32_t expectedModel, uint32_t model);

} // namespace PortCustomRes
