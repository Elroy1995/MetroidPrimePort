#include "port_savestate.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "savestate regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

using PortSaveState::Header;
} // namespace

int main() {
  Header in;
  in.worldId = 0x83F6FF6F;
  in.areaId = 17;
  in.position[0] = -120.5f;
  in.position[1] = 33.25f;
  in.position[2] = 9.f;
  in.forward[0] = 0.f;
  in.forward[1] = -1.f;
  in.forward[2] = 0.f;
  in.morphed = true;
  in.playTime = 3723.5;
  in.savedAt = 1790000000;
  in.world = "Chozo Ruins";
  in.room = "Main Plaza";
  std::vector< uint8_t > blob(4096, 0);
  for (size_t i = 0; i < 940; ++i)
    blob[i] = static_cast< uint8_t >(i * 7);

  const std::string data = PortSaveState::Encode(in, blob);

  // Round trip.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(PortSaveState::Decode(data, out, outBlob));
    CHECK(out.worldId == in.worldId);
    CHECK(out.areaId == in.areaId);
    for (int i = 0; i < 3; ++i) {
      CHECK(out.position[i] == in.position[i]);
      CHECK(out.forward[i] == in.forward[i]);
    }
    CHECK(out.morphed);
    CHECK(out.playTime == in.playTime);
    CHECK(out.savedAt == in.savedAt);
    CHECK(out.world == in.world);
    CHECK(out.room == in.room);
    CHECK(outBlob == blob);
  }

  // Every truncation fails and leaves the outputs alone.
  for (size_t len = 0; len < data.size(); ++len) {
    Header out;
    out.worldId = 1;
    std::vector< uint8_t > outBlob{9};
    CHECK(!PortSaveState::Decode(data.substr(0, len), out, outBlob));
    CHECK(out.worldId == 1);
    CHECK(outBlob.size() == 1);
  }

  // Trailing junk, a wrong magic, a wrong version and an empty blob fail.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(!PortSaveState::Decode(data + "x", out, outBlob));
    std::string bad = data;
    bad[0] = 'X';
    CHECK(!PortSaveState::Decode(bad, out, outBlob));
    bad = data;
    bad[4] = 2;
    CHECK(!PortSaveState::Decode(bad, out, outBlob));
    CHECK(!PortSaveState::Decode(PortSaveState::Encode(in, {}), out, outBlob));
  }

  // A NaN position is refused (it would teleport Samus into nowhere).
  {
    Header nan = in;
    nan.position[1] = std::numeric_limits< float >::quiet_NaN();
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(!PortSaveState::Decode(PortSaveState::Encode(nan, blob), out, outBlob));
  }

  // Long labels are cut, not rejected.
  {
    Header longLabel = in;
    longLabel.room.assign(1000, 'r');
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(PortSaveState::Decode(PortSaveState::Encode(longLabel, blob), out, outBlob));
    CHECK(out.room.size() == 256);
  }

  std::puts("savestate tests passed");
  return 0;
}
