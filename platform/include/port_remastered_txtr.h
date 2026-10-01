#pragma once

// Decodes a Metroid Prime Remastered TXTR resource into plain RGBA8 pixels.
//
// A TXTR is one of the game's RFRM forms: a HEAD chunk holding the texture
// header, a GPU chunk holding the (still block linear, still compressed)
// texel data, and a FOOT footer retrotool appends to every extracted file.
// The footer carries the META chunk that says where each compressed piece of
// the texel data sits inside the file and where it decompresses to, so a
// reader has to do three things: walk the forms, decompress the META's
// buffers, and untile the Tegra block linear surface. All of that is a
// faithful port of retrotool's retrolib (format/txtr.rs, format/rfrm.rs,
// format/foot.rs, util/compression.rs, util/lzss.rs) together with the three
// crates it calls, because retrotool's output is the port's reference: the
// Remastered assets the port reads are the ones retrotool extracted.
//
// Texel formats are decoded by porting tegra_swizzle 0.3.2 (the untiling),
// astc-decode 0.3.1 and bcdec_rs 0.1.2 (the block decoders), so the pixels
// match retrotool byte for byte rather than merely looking right. sRGB is
// reported but never applied: a consumer wanting linear values converts
// afterwards, and doing it here would make the comparison with retrotool's
// PNGs impossible.
//
// Errors come back as std::string rather than exceptions, because the callers
// are the Remastered importers and the F1 overlay, which report failures to
// the player instead of unwinding.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PortRemastered {

// One decoded texture: the top mip, as RGBA8, in the row order it is stored
// in (row 0 is the texture's first stored row, so no flip is applied).
struct TxtrImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;     // the file's own format code, see kTxtrFormat* below
  bool srgb = false;       // the format code says sRGB; the pixels are still encoded
  uint32_t mipCount = 0;   // mipmaps in the file; only the top one is decoded
  std::vector<uint8_t> rgba;  // width * height * 4 bytes
};

// Texture format codes, as they appear in the HEAD chunk. Only the ones the
// Remastered data actually uses are decoded; the rest are named so that an
// unsupported one can be reported by name instead of by number.
enum TxtrFormat : uint32_t {
  kTxtrFormatR8Unorm = 0,
  kTxtrFormatRgb8Unorm = 11,
  kTxtrFormatRgba8Unorm = 12,
  kTxtrFormatRgba8Srgb = 13,
  kTxtrFormatBc1Unorm = 20,
  kTxtrFormatBc1Srgb = 21,
  kTxtrFormatBc2Unorm = 22,
  kTxtrFormatBc2Srgb = 23,
  kTxtrFormatBc3Unorm = 24,
  kTxtrFormatBc3Srgb = 25,
  kTxtrFormatBc4Unorm = 26,
  kTxtrFormatBc4Snorm = 27,
  kTxtrFormatBc5Unorm = 28,
  kTxtrFormatBc5Snorm = 29,
  kTxtrFormatAstc4x4 = 53,    // 53..66 are the linear ASTC footprints
  kTxtrFormatAstc12x12 = 66,
  kTxtrFormatAstc4x4Srgb = 67,  // 67..80 are the sRGB ones
  kTxtrFormatAstc12x12Srgb = 80,
  kTxtrFormatBc6hUfloat = 81,
  kTxtrFormatBc6hSfloat = 82,
  kTxtrFormatBc7Unorm = 83,
  kTxtrFormatBc7UnormSrgb = 84,
};

// Reads the HEAD chunk only: dimensions, format, sRGB flag and mip count.
// `data` is the complete RFRM TXTR file. Nothing is decompressed, so this is
// cheap enough to run over every texture in a pak.
bool ReadTxtrInfo(const uint8_t* data, size_t size, TxtrImage& out, std::string& error);

// Decodes the top mip into `out.rgba`. `data` is the complete RFRM TXTR file,
// the same bytes a pak reader hands back for the asset.
//
// Only layer 0 of the top mip is decoded. A cube map or an array texture
// therefore yields face 0 / element 0; the rest of the surface is untiled but
// discarded, because every consumer so far (the converter's texture writing,
// the port's material maps) wants a plain 2D image, and a later slice or mip
// can be added behind the same call once something asks for one.
//
// Supported formats are the ones the Remastered data uses: R8, RGB8, RGBA8,
// BC1 through BC5, BC6H, BC7 and every ASTC footprint. Anything else fails
// with a message naming the format, rather than producing garbage.
bool DecodeTxtr(const uint8_t* data, size_t size, TxtrImage& out, std::string& error);

}  // namespace PortRemastered