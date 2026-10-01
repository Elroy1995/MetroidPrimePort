// Decodes one Remastered TXTR to raw RGBA8, so the decoder can be compared
// against retrotool's own output (tests/port_remastered_txtr_check.py).
//
//   tool <in.TXTR> <out.rgba>
//
// stdout gets "width height format mips", so the caller can check the
// dimensions without reading the raw file back.

#include <cstdio>
#include <string>
#include <vector>

#include "port_remastered_txtr.h"

namespace {

bool ReadFile(const char* path, std::vector<uint8_t>& out) {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  out.clear();
  uint8_t chunk[64 * 1024];
  size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
    out.insert(out.end(), chunk, chunk + got);
  }
  std::fclose(file);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <in.TXTR> <out.rgba>\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> file;
  if (!ReadFile(argv[1], file)) {
    std::fprintf(stderr, "could not read %s\n", argv[1]);
    return 1;
  }

  std::string error;
  PortRemastered::TxtrImage image;
  if (!PortRemastered::DecodeTxtr(file.data(), file.size(), image, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  std::FILE* out = std::fopen(argv[2], "wb");
  if (out == nullptr) {
    std::fprintf(stderr, "could not write %s\n", argv[2]);
    return 1;
  }
  const bool written =
      image.rgba.empty() ||
      std::fwrite(image.rgba.data(), 1, image.rgba.size(), out) == image.rgba.size();
  std::fclose(out);
  if (!written) {
    std::fprintf(stderr, "short write on %s\n", argv[2]);
    return 1;
  }

  std::printf("%u %u %u %u\n", image.width, image.height, image.format, image.mipCount);
  return 0;
}