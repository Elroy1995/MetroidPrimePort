#include "port_room_env.h"

#include <cstdio>
#include <cstring>
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
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// An axis-aligned box probe: centre and half extents.
void PutProbe(std::vector<uint8_t>& out, float cx, float cy, float cz, float hx, float hy, float hz, uint32_t cube) {
  const float centre[3] = {cx, cy, cz};
  const float half[3] = {hx, hy, hz};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      PutFloat(out, row == col ? 1.f / half[row] : 0.f);
    }
    PutFloat(out, -centre[row] / half[row]);
  }
  for (int i = 0; i < 9; ++i) {
    PutFloat(out, i % 4 == 0 ? 1.f : 0.f);
  }
  Put32(out, 0);
  Put32(out, cube);
  PutFloat(out, 1.f);
  PutFloat(out, 0.05f);
}

void PutCube(std::vector<uint8_t>& out, uint32_t size, uint32_t mips, uint8_t fill) {
  const size_t bytes = PortRoomEnv::CubeBytes(size, mips);
  Put32(out, size);
  Put32(out, mips);
  Put32(out, 0);
  Put32(out, uint32_t(bytes));
  out.insert(out.end(), bytes, fill);
}

std::vector<uint8_t> MakeFile() {
  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  Put32(out, 1);
  PutFloat(out, 4.f);
  PutFloat(out, 0.18f);
  PutFloat(out, 0.6f);
  PutFloat(out, 0.15f);
  Put32(out, 3);
  Put32(out, 2);
  PutProbe(out, 0.f, 0.f, 0.f, 10.f, 10.f, 10.f, 0);  // the room
  PutProbe(out, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1);     // an alcove in it
  PutProbe(out, 40.f, 0.f, 0.f, 5.f, 5.f, 5.f, 0);    // next door
  PutCube(out, 8, 4, 0x11);
  PutCube(out, 4, 3, 0x22);
  return out;
}

void TestNames() {
  uint32_t id = 0;
  Check(PortRoomEnv::ParseFileName("d1241219.RoomEnv", id) && id == 0xD1241219, "file name");
  Check(!PortRoomEnv::ParseFileName("D1241219.roomenvs", id), "file name: longer suffix");
  Check(!PortRoomEnv::ParseFileName("D124121G.roomenv", id), "file name: bad hex");
  Check(!PortRoomEnv::ParseFileName("D1241219.TXTR", id), "file name: a resource");
  Check(!PortRoomEnv::ParseFileName("", id), "file name: empty");
}

void TestParse() {
  Check(PortRoomEnv::CubeBytes(8, 4) == size_t(4 + 1 + 1 + 1) * 16 * 6, "cube bytes");
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeFile();
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "parse");
  Check(file.probes.size() == 3 && file.cubes.size() == 2, "parse: counts");
  Check(file.tonemap[1] == 0.18f, "parse: tonemap");
  Check(file.probes[1].cube == 1 && file.probes[1].blend == 0.05f, "parse: probe");
  Check(file.cubes[1].size == 4 && file.cubes[1].mipCount == 3 && file.data[file.cubes[1].offset] == 0x22 &&
            file.data[file.cubes[0].offset + file.cubes[0].length - 1] == 0x11,
        "parse: cubes");

  // Every way of cutting the file short fails, and none reads past the end.
  for (size_t length = 0; length < good.size(); length += length < 400 ? 1 : 97) {
    Check(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "parse: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[4] = 2;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: version");
  bad = good;
  bad[32 + 88] = 2;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube index");
  bad = good;
  bad[32 + 300] = 6; // the first cube's size
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube size");
  bad = good;
  bad[32 + 2] = 0xC0;
  bad[32 + 3] = 0x7F; // NaN
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: not finite");
  bad = good;
  bad[24] = 0xFF;
  bad[25] = 0xFF;
  bad[26] = 0xFF;
  bad[27] = 0xFF;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: probe count");
}

void TestPick() {
  PortRoomEnv::File file;
  std::string error;
  Check(PortRoomEnv::Parse(MakeFile(), file, error), "pick: parse");
  const float inRoom[3] = {-5.f, 0.f, 0.f};
  const float inAlcove[3] = {2.5f, 0.5f, 0.f};
  const float between[3] = {30.f, 0.f, 0.f}; // 20 from the room, 5 from next door
  const float nearRoom[3] = {12.f, 0.f, 0.f};
  PortRoomEnv::Pick pick = PortRoomEnv::PickProbe(file, inRoom);
  Check(pick.probe == 0 && pick.inside, "pick: inside");
  pick = PortRoomEnv::PickProbe(file, inAlcove);
  Check(pick.probe == 1 && pick.inside && pick.score > 7.99f && pick.score < 8.01f, "pick: the smaller of two boxes");
  pick = PortRoomEnv::PickProbe(file, between);
  Check(pick.probe == 2 && !pick.inside && pick.score > 4.99f && pick.score < 5.01f, "pick: the nearest box");
  pick = PortRoomEnv::PickProbe(file, nearRoom);
  Check(pick.probe == 0 && !pick.inside, "pick: outside, nearest");

  PortRoomEnv::Pick none;
  Check(!none.Better(pick) && pick.Better(none), "pick: against none");
  Check(PortRoomEnv::PickProbe(PortRoomEnv::File(), inRoom).probe < 0, "pick: no probes");
}
} // namespace

int main() {
  TestNames();
  TestParse();
  TestPick();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_room_env tests passed");
  return 0;
}
