#include "port_custom_res.h"

#include "port_log.h"

#include <map>
#include <memory>
#include <mutex>

namespace PortCustomRes {
namespace {

#include "port_custom_assets_data.inc"

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kTXTR = 0x54585452;

// Disc sources (randomprime's resource_info names).
constexpr uint32_t kMetroidCmdl = 0x2F976E86;   // Metroid.CMDL
constexpr uint32_t kGravitySuitCmdl = 0x95946E41; // Node1_11.CMDL
constexpr uint32_t kGravitySuitAncs = 0x27A97006; // Node1_11.ANCS
constexpr uint32_t kVisorCmdl = 0x61DAB956;       // Node1_39_1.CMDL
constexpr uint32_t kVisorAncs = 0x9F0C908A;       // Node1_39_1.ANCS

uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) |
         (uint32_t(data[at + 2]) << 8) | data[at + 3];
}

void Put32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
  data[at] = uint8_t(value >> 24);
  data[at + 1] = uint8_t(value >> 16);
  data[at + 2] = uint8_t(value >> 8);
  data[at + 3] = uint8_t(value);
}

template <size_t N>
std::vector<uint8_t> Embedded(const unsigned char (&bytes)[N]) {
  return std::vector<uint8_t>(bytes, bytes + N);
}

bool Build(uint32_t id, const DiscReader& read, Resource& out) {
  // Each model's texture patches (material set 0 index -> texture).
  struct TexturePatch {
    uint32_t index, texture;
  };
  auto model = [&](std::vector<uint8_t> cmdl, std::initializer_list<TexturePatch> patches) {
    for (const TexturePatch& patch : patches)
      if (!SetCmdlTexture(cmdl, patch.index, patch.texture))
        return false;
    out.type = kCMDL;
    out.data.swap(cmdl);
    return true;
  };
  auto discModel = [&](uint32_t source, std::initializer_list<TexturePatch> patches) {
    std::vector<uint8_t> cmdl;
    return read(source, cmdl) && model(std::move(cmdl), patches);
  };
  // A copy of a retail pickup's ANCS whose character 0 draws the new model.
  auto anim = [&](uint32_t source, uint32_t sourceModel, uint32_t newModel) {
    std::vector<uint8_t> ancs;
    if (!read(source, ancs) || !SetAncsModel(ancs, sourceModel, newModel))
      return false;
    out.type = kANCS;
    out.data.swap(ancs);
    return true;
  };

  switch (id) {
  case kNothingTxtr:
    out.type = kTXTR;
    out.data = Embedded(kNothingTxtrData);
    return true;
  case kPhazonSuitTxtr1:
    out.type = kTXTR;
    out.data = Embedded(kPhazonSuitTxtr1Data);
    return true;
  case kPhazonSuitTxtr2:
    out.type = kTXTR;
    out.data = Embedded(kPhazonSuitTxtr2Data);
    return true;
  case kNothingCmdl:
    return discModel(kMetroidCmdl, {{0, kNothingTxtr}, {1, kNothingTxtr}, {2, kNothingTxtr},
                                    {3, kNothingTxtr}, {4, kNothingTxtr}, {5, kNothingTxtr},
                                    {6, kNothingTxtr}, {7, kNothingTxtr}});
  case kNothingAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kNothingCmdl);
  case kZoomerCmdl:
    return model(Embedded(kZoomerCmdlData), {{0, kNothingTxtr}});
  case kZoomerAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kZoomerCmdl);
  case kCogCmdl:
    return model(Embedded(kCogCmdlData), {});
  case kCogAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kCogCmdl);
  case kPhazonSuitCmdl:
    return discModel(kGravitySuitCmdl, {{0, kPhazonSuitTxtr1}, {3, kPhazonSuitTxtr2}});
  case kPhazonSuitAncs:
    return anim(kGravitySuitAncs, kGravitySuitCmdl, kPhazonSuitCmdl);
  case kThermalCmdl:
    return discModel(kVisorCmdl, {{0, 0xFC095F6C}});
  case kThermalAncs:
    return anim(kVisorAncs, kVisorCmdl, kThermalCmdl);
  case kXrayCmdl:
    return discModel(kVisorCmdl, {{0, 0xBE4CD99D}});
  case kXrayAncs:
    return anim(kVisorAncs, kVisorCmdl, kXrayCmdl);
  case kCombatCmdl:
    return discModel(kVisorCmdl, {{0, 0x1D588B22}});
  case kCombatAncs:
    return anim(kVisorAncs, kVisorCmdl, kCombatCmdl);
  default:
    return false;
  }
}

} // namespace

bool SetCmdlTexture(std::vector<uint8_t>& cmdl, uint32_t index, uint32_t texture) {
  // Header: magic, version, flags, AABB (6 floats), section count, material
  // set count, section sizes; data starts 32-byte aligned with material set 0,
  // whose first word is its texture count.
  if (cmdl.size() < 44 || Get32(cmdl, 0) != 0xDEADBABE || Get32(cmdl, 40) == 0)
    return false;
  const uint64_t sections = Get32(cmdl, 36);
  const uint64_t dataStart = (44 + 4 * sections + 31) & ~uint64_t(31);
  if (dataStart + 4 > cmdl.size())
    return false;
  const uint32_t count = Get32(cmdl, dataStart);
  const uint64_t at = dataStart + 4 + 4 * uint64_t(index);
  if (index >= count || at + 4 > cmdl.size())
    return false;
  Put32(cmdl, at, texture);
  return true;
}

bool SetAncsModel(std::vector<uint8_t>& ancs, uint32_t expectedModel, uint32_t model) {
  // u16 version, u16 character set version, u32 character count, then
  // character 0: u32 id, u16 version, name (NUL-terminated), u32 model.
  if (ancs.size() < 14 || Get32(ancs, 4) == 0)
    return false;
  size_t at = 14;
  while (at < ancs.size() && ancs[at] != 0)
    ++at;
  ++at;
  if (at + 4 > ancs.size() || Get32(ancs, at) != expectedModel)
    return false;
  Put32(ancs, at, model);
  return true;
}

const Resource* Find(uint32_t id, const DiscReader& read) {
  if (!IsCustomId(id))
    return nullptr;
  static std::mutex sMutex;
  static std::map<uint32_t, std::unique_ptr<Resource>> sBuilt;
  std::lock_guard<std::mutex> lock(sMutex);
  const auto found = sBuilt.find(id);
  if (found != sBuilt.end())
    return found->second.get();
  std::unique_ptr<Resource> resource(new Resource);
  if (!Build(id, read, *resource)) {
    // Unknown ids fail quietly (a randomprime disc's other custom assets are
    // found in its PAKs before this is asked); a known one means an odd disc.
    if (id <= kCombatAncs)
      PortLog::Write("custom resource %08X: disc source missing or unexpected\n", id);
    resource.reset();
  }
  return (sBuilt[id] = std::move(resource)).get();
}

} // namespace PortCustomRes
