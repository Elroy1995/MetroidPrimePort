// Runs one of the importer's image functions on a raw RGBA file, for
// tests/port_remastered_image_check.py to compare against Pillow.
//
//   port_remastered_image_tool resize <w> <h> <in.rgba> <out.rgba> <out w> <out h>
//   port_remastered_image_tool bc7|bc5|rgba8|cmpr|cmpra <w> <h> <in.rgba> <out>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "port_remastered_image.h"

using namespace PortRemastered;

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: %s resize|bc7|bc5|rgba8|cmpr|cmpra <w> <h> <in.rgba> <out> [out w, out h]\n", argv[0]);
    return 2;
  }
  const std::string op = argv[1];
  Image img;
  img.width = std::atoi(argv[2]);
  img.height = std::atoi(argv[3]);
  {
    std::ifstream f(argv[4], std::ios::binary);
    img.rgba.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  if (img.width <= 0 || img.height <= 0 || img.rgba.size() != size_t(img.width) * size_t(img.height) * 4) {
    std::fprintf(stderr, "%s is not %dx%d RGBA\n", argv[4], img.width, img.height);
    return 2;
  }
  const auto start = std::chrono::steady_clock::now();
  std::vector<uint8_t> out;
  if (op == "resize" && argc == 8) {
    out = Resize(img, std::atoi(argv[6]), std::atoi(argv[7])).rgba;
  } else if (op == "bc7") {
    out = EncodeDds(img, DdsFormat::BC7);
  } else if (op == "bc5") {
    out = EncodeDds(img, DdsFormat::BC5);
  } else if (op == "rgba8") {
    out = EncodeTxtrRgba8(img);
  } else if (op == "cmpr" || op == "cmpra") {
    out = EncodeTxtrCmpr(img, op == "cmpra");
  } else {
    std::fprintf(stderr, "unknown operation %s\n", op.c_str());
    return 2;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::ofstream f(argv[5], std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  std::printf("%.1f ms\n", ms);
  return f ? 0 : 1;
}
