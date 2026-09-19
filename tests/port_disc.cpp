#include "port_disc.h"
#include <dolphin/dvd.h>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace {
std::array<uint8_t, 0x108> dol{};
s32 dolSize = dol.size();
void Big32(size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) dol[offset + i] = value >> (24 - i * 8);
}
void Check(bool condition) { if (!condition) std::abort(); }
bool Rejected(uint32_t address, uint32_t size) {
  try { PortReadDolResource(address, size); }
  catch (const std::runtime_error&) { return true; }
  return false;
}
}
extern "C" const u8* DVDGetDOLLocation(s32* size) {
  *size = dolSize;
  return dol.data();
}
int main() {
  Big32(7 * 4, 0x100);
  Big32(0x48 + 7 * 4, 0x81230000);
  Big32(0x90 + 7 * 4, 8);
  for (int i = 0; i < 8; ++i) dol[0x100 + i] = 0x30 + i;
  const auto data = PortReadDolResource(0x81230002, 4);
  Check(data.size() == 4 && data.front() == 0x32 && data.back() == 0x35);
  Check(Rejected(0x81230000, 9));
  Check(Rejected(0x8122ffff, 4));
  Big32(7 * 4, 0xfffffff0);
  Check(Rejected(0x81230002, 4));
  dolSize = 4;
  Check(Rejected(0x81230000, 4));
}
