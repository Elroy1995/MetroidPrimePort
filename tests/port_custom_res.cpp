#include "port_custom_res.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

uint32_t Get32(const std::vector<uint8_t>& data, size_t at) {
  return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
}

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;

// A CMDL with two sections (sizes only) and material set 0 holding textures
// 0x100 + i; its data starts at 64, the next 32-byte boundary after 52.
std::vector<uint8_t> MakeCmdl(uint32_t textures) {
  std::vector<uint8_t> cmdl;
  Put32(cmdl, 0xDEADBABE);
  Put32(cmdl, 2);
  Put32(cmdl, 0);
  for (int i = 0; i < 6; ++i)
    Put32(cmdl, 0);
  Put32(cmdl, 2); // sections
  Put32(cmdl, 1); // material sets
  Put32(cmdl, 4 + 4 * textures);
  Put32(cmdl, 0);
  cmdl.resize(64, 0);
  Put32(cmdl, textures);
  for (uint32_t i = 0; i < textures; ++i)
    Put32(cmdl, 0x100 + i);
  return cmdl;
}

std::vector<uint8_t> MakeAncs(uint32_t model) {
  std::vector<uint8_t> ancs = {0, 1, 0, 1};
  Put32(ancs, 1);  // characters
  Put32(ancs, 0);  // character id
  ancs.push_back(0);
  ancs.push_back(6);
  for (char c : std::string("Node1_11"))
    ancs.push_back(uint8_t(c));
  ancs.push_back(0);
  Put32(ancs, model);
  Put32(ancs, 0x1234); // skin
  return ancs;
}
} // namespace

int main() {
  using namespace PortCustomRes;

  {
    std::vector<uint8_t> cmdl = MakeCmdl(4);
    Check(SetCmdlTexture(cmdl, 3, 0xCAFE) && Get32(cmdl, 64 + 4 + 12) == 0xCAFE &&
              Get32(cmdl, 64 + 4) == 0x100,
          "a CMDL texture slot is replaced in material set 0");
    const std::vector<uint8_t> before = cmdl;
    Check(!SetCmdlTexture(cmdl, 4, 0xCAFE) && cmdl == before, "a slot past the texture count is refused");
    std::vector<uint8_t> bad = cmdl;
    bad[0] = 0;
    Check(!SetCmdlTexture(bad, 0, 1), "a CMDL without the magic is refused");
    std::vector<uint8_t> tiny(20, 0);
    Check(!SetCmdlTexture(tiny, 0, 1), "a truncated CMDL is refused");
    std::vector<uint8_t> cut(cmdl.begin(), cmdl.begin() + 66);
    Check(!SetCmdlTexture(cut, 0, 1), "a CMDL cut inside the material set is refused");
  }
  {
    std::vector<uint8_t> ancs = MakeAncs(0x95946E41);
    Check(SetAncsModel(ancs, 0x95946E41, 0xDEAF0005) && Get32(ancs, ancs.size() - 8) == 0xDEAF0005 &&
              Get32(ancs, ancs.size() - 4) == 0x1234,
          "an ANCS character 0 model is replaced");
    Check(!SetAncsModel(ancs, 0x95946E41, 1), "an ANCS whose model isn't the expected one is refused");
    std::vector<uint8_t> cut(ancs.begin(), ancs.begin() + 20);
    Check(!SetAncsModel(cut, 0xDEAF0005, 1), "an ANCS cut inside the name is refused");
  }
  {
    int reads = 0;
    const DiscReader disc = [&](uint32_t id, std::vector<uint8_t>& out) {
      ++reads;
      if (id == 0x2F976E86) { // Metroid.CMDL
        out = MakeCmdl(8);
        return true;
      }
      if (id == 0x27A97006) { // Node1_11.ANCS
        out = MakeAncs(0x95946E41);
        return true;
      }
      return false;
    };
    const Resource* nothing = Find(kNothingCmdl, disc);
    bool allNothing = nothing != nullptr && nothing->type == kCMDL;
    for (uint32_t i = 0; allNothing && i < 8; ++i)
      allNothing = Get32(nothing->data, 64 + 4 + 4 * i) == kNothingTxtr;
    Check(allNothing, "Nothing is the Metroid model with every texture replaced");
    const Resource* anim = Find(kNothingAncs, disc);
    Check(anim != nullptr && anim->type == kANCS && Get32(anim->data, anim->data.size() - 8) == kNothingCmdl,
          "Nothing's ANCS draws the Nothing model");
    Check(Find(kNothingCmdl, disc) == nothing, "a built resource is kept");
    const Resource* cog = Find(kCogCmdl, disc);
    const Resource* zoomer = Find(kZoomerCmdl, disc);
    Check(cog != nullptr && Get32(cog->data, 0) == 0xDEADBABE && zoomer != nullptr &&
              Get32(zoomer->data, 0) == 0xDEADBABE,
          "the embedded Cog and Zoomer models build");
    const Resource* txtr = Find(kNothingTxtr, disc);
    Check(txtr != nullptr && txtr->type == 0x54585452 && !txtr->data.empty(), "the embedded textures build");
    const int readsBefore = reads;
    Check(Find(kThermalCmdl, disc) == nullptr && Find(kThermalCmdl, disc) == nullptr &&
              reads == readsBefore + 1,
          "a missing disc source fails once and stays failed");
    Check(Find(0xDEAF0100, disc) == nullptr && Find(0x12345678, disc) == nullptr,
          "unknown and non-custom ids have no resource");
  }

  if (sFailures == 0)
    std::printf("port_custom_res_tests: all passed\n");
  return sFailures == 0 ? 0 : 1;
}
