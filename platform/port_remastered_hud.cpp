// Remastered's HUD frames as frames the original loads (port_remastered_hud.h).

#include "port_remastered_hud.h"

#include "port_hud_bars.h"
#include "port_remastered_pak.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <unordered_map>

namespace PortRemastered {
namespace {

constexpr uint32_t Tag(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

// GUIF widget types.
enum : uint32_t {
  kBaseWidget = 0,
  kCamera = 1,
  kEnergyBar = 2,
  kHeadWidget = 4,
  kMeter = 8,
  kModel = 9,
  kTextPane = 12,
  kLastType = 12,
};

constexpr uint32_t kNoModel = 0xFFFFFFFF;
// A disc model whose fourth material is the one a HUD picture wants: the
// texture times the widget's colour, alpha from the texture.
constexpr uint32_t kTemplateModel = 0xE64E5DBA;
constexpr size_t kTemplateMaterial = 3;
// The ids the output is written under start here and step past the disc's own.
constexpr uint32_t kModelIds = 0x52480000;
constexpr uint32_t kTextureIds = 0x52470000;
constexpr int kNativeSize = 2048;  // largest edge of a picture
constexpr int kStubSize = 64;      // and of the TXTR standing in for it on the game's heap
// What the game looks a frame's head up by; Remastered names its own after the frame.
constexpr const char* kHeadName = "kGSYS_HeadWidgetID";

// Remastered widgets with no use here: the face reflection, the second set of
// missile digits and a spare camera.
const char* const kSkip[] = {"model_samusface", "textpane_missiledigits1", "camera_default"};
// The game moves the visor and beam menus' items about a base widget; in
// Remastered the place they are laid out around is another widget.
const char* const kAnchor[][2] = {
    {"BaseWidget_VisorMenu", "basewidget_visormenuicons"},
    {"BaseWidget_BeamMenu", "basewidget_beammenuicons"},
};
const char* const kAlias[][2] = {
    {"group_energytank", "group_energytank0"},
};

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return s;
}

bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string Hex8(uint32_t v) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", v);
  return text;
}

int NextPow2(int v) {
  int p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

// --- Transforms ------------------------------------------------------------------

struct Mat {
  double m[4][4];
};

Mat Identity() {
  Mat r{};
  for (int i = 0; i < 4; ++i) {
    r.m[i][i] = 1.0;
  }
  return r;
}

Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (int k = 0; k < 4; ++k) {
        r.m[i][j] += a.m[i][k] * b.m[k][j];
      }
    }
  }
  return r;
}

// The disc's frame is z-up, Remastered's y-up: disc = (x, -z, y) of Remastered.
void ToDisc(const float* in, float* out) {
  out[0] = in[0];
  out[1] = -in[2];
  out[2] = in[1];
}

// A Remastered transform between the disc's axes.
Mat ToDisc(const Mat& w) {
  Mat a{};
  a.m[0][0] = 1.0;
  a.m[1][2] = -1.0;
  a.m[2][1] = 1.0;
  a.m[3][3] = 1.0;
  Mat t{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      t.m[i][j] = a.m[j][i];
    }
  }
  return Mul(Mul(a, w), t);
}

// The inverse of a rotation, scale and translation; false for a flattened one.
bool Inverse(const Mat& a, Mat& out) {
  const double (*m)[4] = a.m;
  const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
  const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
  const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
  const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
  if (det == 0.0 || !std::isfinite(det)) {
    return false;
  }
  out = Identity();
  out.m[0][0] = c00 / det;
  out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  out.m[1][0] = c01 / det;
  out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  out.m[2][0] = c02 / det;
  out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  for (int i = 0; i < 3; ++i) {
    out.m[i][3] = -(out.m[i][0] * m[0][3] + out.m[i][1] * m[1][3] + out.m[i][2] * m[2][3]);
  }
  return true;
}

double AxisLength(const Mat& a, int k) {
  return std::sqrt(a.m[0][k] * a.m[0][k] + a.m[1][k] * a.m[1][k] + a.m[2][k] * a.m[2][k]);
}

// --- Reading and writing ---------------------------------------------------------

// Bounds-checked: a read past the end yields zeros and clears `ok`.
struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at;
  bool big;
  bool ok = true;

  bool Has(size_t count) {
    if (at > size || count > size - at) {
      ok = false;
    }
    return ok;
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    if (!Has(2)) {
      return 0;
    }
    const uint16_t v = big ? uint16_t(data[at] << 8 | data[at + 1]) : uint16_t(data[at + 1] << 8 | data[at]);
    at += 2;
    return v;
  }
  uint32_t U32() {
    if (!Has(4)) {
      return 0;
    }
    const uint8_t* p = data + at;
    at += 4;
    return big ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3])
               : uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[0]);
  }
  float Float() {
    const uint32_t bits = U32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  void Skip(size_t count) {
    if (Has(count)) {
      at += count;
    }
  }
  std::string CString() {
    std::string s;
    while (Has(1) && data[at] != 0) {
      s.push_back(char(data[at++]));
    }
    Skip(1);
    return s;
  }
};

using Blob = std::vector<uint8_t>;

void Put32(Blob& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Put16(Blob& out, uint16_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void PutFloat(Blob& out, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  Put32(out, bits);
}

void Set32(Blob& out, size_t at, uint32_t v) {
  out[at] = uint8_t(v >> 24);
  out[at + 1] = uint8_t(v >> 16);
  out[at + 2] = uint8_t(v >> 8);
  out[at + 3] = uint8_t(v);
}

void SetFloat(Blob& out, size_t at, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  Set32(out, at, bits);
}

uint32_t Get32(const Blob& in, size_t at) {
  return uint32_t(in[at]) << 24 | uint32_t(in[at + 1]) << 16 | uint32_t(in[at + 2]) << 8 | uint32_t(in[at + 3]);
}

float GetFloat(const Blob& in, size_t at) {
  const uint32_t bits = Get32(in, at);
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

void Pad32(Blob& out) { out.resize((out.size() + 31) & ~size_t(31)); }

// --- The disc's frame ------------------------------------------------------------

// Offsets into a text pane's type data.
constexpr size_t kTextPaneSize = 74;
constexpr size_t kTextPaneExtent = 66;
constexpr size_t kModelSize = 12;

struct Widget {
  uint32_t type = 0;
  std::string name;
  std::string parent;
  uint8_t flags[4] = {};  // animated, visible, active, cull faces
  float color[4] = {};
  uint32_t draw = 0;
  Blob typeData;
  bool hasWorker = false;
  uint16_t worker = 0;
  Mat local = Identity();
  Mat world = Identity();
  bool placed = false;      // `world` is known
  uint8_t tail[18] = {};
  int guif = -1;            // the Remastered widget of the same name
  bool added = false;       // Remastered's own, not on the disc
};

bool ParseFrame(const Blob& data, uint32_t header[4], std::vector<Widget>& out, std::string& error) {
  Reader in{data.data(), data.size(), 0, true};
  for (int i = 0; i < 4; ++i) {
    header[i] = in.U32();
  }
  const uint32_t count = in.U32();
  for (uint32_t k = 0; k < count && in.ok; ++k) {
    Widget w;
    w.type = in.U32();
    w.name = in.CString();
    w.parent = in.CString();
    for (uint8_t& flag : w.flags) {
      flag = in.U8();
    }
    for (float& c : w.color) {
      c = in.Float();
    }
    w.draw = in.U32();
    const size_t start = in.at;
    switch (w.type) {
    case Tag('C', 'A', 'M', 'R'):
      in.Skip(in.U32() == 0 ? 16 : 24);
      break;
    case Tag('L', 'I', 'T', 'E'): {
      const uint32_t light = in.U32();
      in.Skip(light == 0 ? 32 : 28);
      break;
    }
    case Tag('M', 'O', 'D', 'L'):
      in.Skip(kModelSize);
      break;
    case Tag('M', 'E', 'T', 'R'):
      in.Skip(10);
      break;
    case Tag('G', 'R', 'U', 'P'):
      in.Skip(3);
      break;
    case Tag('T', 'B', 'G', 'P'):
      in.Skip(35);
      break;
    case Tag('S', 'L', 'G', 'P'):
      in.Skip(16);
      break;
    case Tag('P', 'A', 'N', 'E'):
      in.Skip(20);
      break;
    case Tag('T', 'X', 'P', 'N'):
      in.Skip(kTextPaneSize);
      break;
    case Tag('I', 'M', 'G', 'P'): {
      in.Skip(12);
      const uint32_t coords = in.U32();
      in.Skip(size_t(coords) * 12);
      const uint32_t uvs = in.U32();
      in.Skip(size_t(uvs) * 8);
      break;
    }
    case Tag('E', 'N', 'R', 'G'):
      in.Skip(4);
      break;
    case Tag('H', 'W', 'I', 'G'):
    case Tag('B', 'W', 'I', 'G'):
      break;
    default:
      error = "the disc's frame has a widget of an unknown type (" + w.name + ")";
      return false;
    }
    if (!in.ok) {
      break;
    }
    w.typeData.assign(data.begin() + long(start), data.begin() + long(in.at));
    w.hasWorker = in.U8() != 0;
    if (w.hasWorker) {
      w.worker = in.U16();
    }
    for (int i = 0; i < 3; ++i) {
      w.local.m[i][3] = in.Float();
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        w.local.m[i][j] = in.Float();
      }
    }
    if (in.Has(sizeof(w.tail))) {
      std::memcpy(w.tail, in.data + in.at, sizeof(w.tail));
      in.at += sizeof(w.tail);
    }
    out.push_back(std::move(w));
  }
  if (!in.ok || out.empty()) {
    error = "the disc's frame is cut short";
    return false;
  }
  return true;
}

// --- Remastered's frame ----------------------------------------------------------

struct RemWidget {
  uint32_t type = 0;
  uint32_t index = 0;
  uint32_t parent = 0;
  std::string name;
  uint8_t flags[3] = {};
  float color[4] = {};
  uint32_t draw = 0;
  uint32_t variant = 0;  // above 1: another language's copy of a widget
  Mat local = Identity();
  Mat world = Identity();
  bool placed = false;
  uint32_t projection = 0;
  float camera[4] = {};  // fov, aspect; or right, left, top, bottom
  std::vector<uint32_t> meshes;
};

constexpr size_t kFrameHeader = 32;

bool IsFrame(const uint8_t* data, size_t size) {
  return size >= kFrameHeader + 20 && std::memcmp(data, "RFRM", 4) == 0 && std::memcmp(data + 0x14, "GUIF", 4) == 0;
}

// Whether the widget numbered `index` starts at `at`. Several widget types end
// in data of no fixed size, so the next one is found by what a header must
// look like: the next index, a parent before it, a short plain name.
bool LooksLikeWidget(const uint8_t* data, size_t size, size_t at, uint32_t index) {
  if (at > size || size - at < 20) {
    return false;
  }
  Reader in{data, size, at, false};
  const uint32_t type = in.U32();
  const uint32_t own = in.U32();
  const uint32_t parent = in.U32();
  const int32_t worker = int32_t(in.U32());
  const uint32_t length = in.U32();
  if (type > kLastType || own != index || parent >= index || worker < -1 || worker >= 256 || length == 0 ||
      length >= 64 || !in.Has(length)) {
    return false;
  }
  for (uint32_t i = 0; i < length; ++i) {
    if (data[in.at + i] >= 0x80) {
      return false;
    }
  }
  return true;
}

bool ParseRemFrame(const uint8_t* data, size_t size, std::vector<RemWidget>& out, std::string& error) {
  if (!IsFrame(data, size)) {
    error = "not a frame";
    return false;
  }
  Reader in{data, size, kFrameHeader + 16, false};
  const uint32_t count = in.U32();
  for (uint32_t k = 0; k < count; ++k) {
    RemWidget w;
    w.type = in.U32();
    w.index = in.U32();
    w.parent = in.U32();
    in.U32();  // worker id; the disc's frame supplies those
    const uint32_t length = in.U32();
    if (!in.Has(length)) {
      break;
    }
    w.name.assign(reinterpret_cast<const char*>(data + in.at), length);
    in.at += length;
    for (uint8_t& flag : w.flags) {
      flag = in.U8();
    }
    for (float& c : w.color) {
      c = in.Float();
    }
    w.draw = in.U32();
    w.variant = in.U32();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 4; ++j) {
        w.local.m[i][j] = in.Float();
      }
    }
    bool fixed = true;
    switch (w.type) {
    case kBaseWidget:
    case kHeadWidget:
      break;
    case kCamera:
      w.projection = in.U32();
      for (int i = 0; i < (w.projection == 0 ? 2 : 4); ++i) {
        w.camera[i] = in.Float();
      }
      break;
    case kEnergyBar:
      w.meshes.push_back(in.U32());
      break;
    case kMeter:
      in.Skip(8);
      break;
    case kModel:
    case kTextPane: {
      const uint32_t meshes = in.U32();
      if (!in.Has(size_t(meshes) * 4)) {
        break;
      }
      for (uint32_t i = 0; i < meshes; ++i) {
        w.meshes.push_back(in.U32());
      }
      if (w.type == kModel) {
        in.U32();
      } else {
        fixed = false;
      }
      break;
    }
    default:
      fixed = false;
      break;
    }
    if (!in.ok) {
      break;
    }
    if (k + 1 < count) {
      if (!fixed) {
        while (in.at < size && !LooksLikeWidget(data, size, in.at, w.index + 1)) {
          ++in.at;
        }
      }
      if (!LooksLikeWidget(data, size, in.at, w.index + 1)) {
        error = "lost after the widget " + w.name;
        return false;
      }
    }
    out.push_back(std::move(w));
  }
  if (!in.ok || out.empty()) {
    error = "the frame is cut short";
    return false;
  }
  return true;
}

// --- Meshes ----------------------------------------------------------------------

// One mesh of the frame's model, on its own vertices, in the disc's axes.
struct Part {
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 2>> uvs;
  std::vector<uint32_t> indices;
  const ModelMaterial* material = nullptr;
  uint32_t slot = 0;  // which of the model's textures it draws with
};

bool MeshPart(const Model& model, uint32_t mesh, Part& out) {
  if (mesh >= model.meshes.size()) {
    return false;
  }
  const ModelMesh& m = model.meshes[mesh];
  if (m.vertexBuffer >= model.vertexBuffers.size() || m.material >= model.materials.size() || m.indices.empty() ||
      m.indices.size() % 3 != 0) {
    return false;
  }
  const ModelVertexBuffer& vb = model.vertexBuffers[m.vertexBuffer];
  if (vb.positions.size() != size_t(vb.vertexCount) * 3 || vb.uvs.empty() ||
      vb.uvs[0].size() != size_t(vb.vertexCount) * 2) {
    return false;
  }
  std::vector<uint32_t> used(m.indices);
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  if (used.back() >= vb.vertexCount) {
    return false;
  }
  out = {};
  out.material = &model.materials[m.material];
  for (uint32_t v : used) {
    std::array<float, 3> p;
    ToDisc(&vb.positions[size_t(v) * 3], p.data());
    out.positions.push_back(p);
    out.uvs.push_back({vb.uvs[0][size_t(v) * 2], vb.uvs[0][size_t(v) * 2 + 1]});
  }
  for (uint32_t v : m.indices) {
    out.indices.push_back(uint32_t(std::lower_bound(used.begin(), used.end(), v) - used.begin()));
  }
  return true;
}

// The picture a HUD material shows: its base colour map, or failing that its
// diffuse one. In a pak's byte order.
bool MaterialTexture(const ModelMaterial& material, ModelUuid& out) {
  for (uint32_t usage : {Tag('B', 'C', 'L', 'R'), Tag('D', 'I', 'F', 'T')}) {
    for (const ModelMaterialData& d : material.data) {
      if (d.kind != ModelMaterialData::Kind::Texture || d.usage != usage || d.texture.id == ModelUuid{}) {
        continue;
      }
      out = d.texture.id;
      std::swap(out[0], out[3]);
      std::swap(out[1], out[2]);
      std::swap(out[4], out[5]);
      std::swap(out[6], out[7]);
      return true;
    }
  }
  return false;
}

// A bar's mesh as the strip the game fills (port_hud_bars.h). The mesh is a
// ribbon: each of its two edges holds one value of one texture coordinate, and
// the other coordinate runs along it from the empty end to the full one.
bool Stations(const Part& part, PortHudBars::Bar& bar) {
  const size_t count = part.positions.size();
  if (count < 4) {
    return false;
  }
  std::set<long long> values[2];
  double lo[3], hi[3], mean[2] = {0.0, 0.0};
  for (int c = 0; c < 3; ++c) {
    lo[c] = hi[c] = part.positions[0][c];
  }
  for (size_t i = 0; i < count; ++i) {
    for (int c = 0; c < 2; ++c) {
      values[c].insert(std::llrint(double(part.uvs[i][c]) * 1000.0));
      mean[c] += double(part.uvs[i][c]) / double(count);
    }
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min(lo[c], double(part.positions[i][c]));
      hi[c] = std::max(hi[c], double(part.positions[i][c]));
    }
  }
  const int across = values[0].size() <= values[1].size() ? 0 : 1;
  if (values[across].size() != 2) {
    return false;
  }
  int along = 0;
  for (int c = 1; c < 3; ++c) {
    if (hi[c] - lo[c] > hi[along] - lo[along]) {
      along = c;
    }
  }
  using Row = std::array<double, 5>;  // position, then texture coordinate
  std::vector<Row> edges[2];
  for (int side = 0; side < 2; ++side) {
    std::set<Row> rows;
    for (size_t i = 0; i < count; ++i) {
      if ((double(part.uvs[i][across]) > mean[across]) != (side == 1)) {
        continue;
      }
      Row row;
      for (int c = 0; c < 3; ++c) {
        row[c] = std::nearbyint(double(part.positions[i][c]) * 1e5) / 1e5;
      }
      for (int c = 0; c < 2; ++c) {
        row[3 + c] = std::nearbyint(double(part.uvs[i][c]) * 1e5) / 1e5;
      }
      rows.insert(row);
    }
    edges[side].assign(rows.begin(), rows.end());
    std::stable_sort(edges[side].begin(), edges[side].end(), [&](const Row& a, const Row& b) {
      return a[along] != b[along] ? a[along] < b[along] : a[4 - across] < b[4 - across];
    });
  }
  if (edges[0].size() != edges[1].size() || edges[0].size() < 2) {
    return false;
  }
  bar.stations.clear();
  for (size_t i = 0; i < edges[0].size(); ++i) {
    PortHudBars::Station s;
    for (int c = 0; c < 3; ++c) {
      s.a[c] = float(edges[0][i][c]);
      s.b[c] = float(edges[1][i][c]);
    }
    for (int c = 0; c < 2; ++c) {
      s.uvA[c] = float(edges[0][i][3 + c]);
      s.uvB[c] = float(edges[1][i][3 + c]);
    }
    bar.stations.push_back(s);
  }
  return true;
}

// A disc model of `parts`, one surface and one material each: `material` with
// its texture slot set, unlit.
bool BuildModel(const std::vector<Part>& parts, const std::vector<uint32_t>& textures, const Blob& material,
                Blob& out) {
  size_t vertices = 0;
  for (const Part& part : parts) {
    vertices += part.positions.size();
    if (part.indices.size() >= 0xFFFF) {
      return false;
    }
  }
  if (vertices >= 0xFFFF) {
    return false;  // the display list's indices are 16 bit
  }
  std::vector<Blob> sections;
  Blob set;
  Put32(set, uint32_t(textures.size()));
  for (uint32_t id : textures) {
    Put32(set, id);
  }
  Put32(set, uint32_t(parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) {
    Put32(set, uint32_t((i + 1) * material.size()));
  }
  for (const Part& part : parts) {
    const size_t at = set.size();
    set.insert(set.end(), material.begin(), material.end());
    Set32(set, at + 8, part.slot);
    Set32(set, at + 0x1c, 0x3000);  // both colour channels unlit
  }
  sections.push_back(std::move(set));

  Blob positions, uvs;
  float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool first = true;
  for (const Part& part : parts) {
    for (size_t i = 0; i < part.positions.size(); ++i) {
      for (int c = 0; c < 3; ++c) {
        const float v = part.positions[i][c];
        PutFloat(positions, v);
        lo[c] = first ? v : std::min(lo[c], v);
        hi[c] = first ? v : std::max(hi[c], v);
      }
      first = false;
      PutFloat(uvs, part.uvs[i][0]);
      PutFloat(uvs, part.uvs[i][1]);
    }
  }
  sections.push_back(std::move(positions));
  Blob normals;  // one, facing the camera; nothing is lit by it
  Put16(normals, 0);
  Put16(normals, uint16_t(-0x4000));
  Put16(normals, 0);
  sections.push_back(std::move(normals));
  sections.push_back(Blob(32, 0));  // colours
  sections.push_back(std::move(uvs));
  sections.push_back(Blob());  // lightmap coordinates

  std::vector<Blob> surfaces;
  uint32_t base = 0;
  for (size_t k = 0; k < parts.size(); ++k) {
    const Part& part = parts[k];
    Blob list;
    list.push_back(0x91);  // triangles, 16-bit position / normal / texture coordinate indices
    Put16(list, uint16_t(part.indices.size()));
    for (uint32_t index : part.indices) {
      Put16(list, uint16_t(base + index));
      Put16(list, 0);
      Put16(list, uint16_t(base + index));
    }
    Pad32(list);
    base += uint32_t(part.positions.size());
    double centre[3] = {0.0, 0.0, 0.0};
    for (const auto& p : part.positions) {
      for (int c = 0; c < 3; ++c) {
        centre[c] += double(p[c]) / double(part.positions.size());
      }
    }
    Blob surface;
    for (double c : centre) {
      PutFloat(surface, float(c));
    }
    Put32(surface, uint32_t(k));
    Put32(surface, uint32_t(list.size()) | 0x80000000u);
    Put32(surface, 0);
    Put32(surface, 0);
    Put32(surface, 0);
    PutFloat(surface, 0.0f);
    PutFloat(surface, -1.0f);
    PutFloat(surface, 0.0f);
    Pad32(surface);
    surface.insert(surface.end(), list.begin(), list.end());
    surfaces.push_back(std::move(surface));
  }
  Blob ends;
  Put32(ends, uint32_t(surfaces.size()));
  uint32_t end = 0;
  for (const Blob& surface : surfaces) {
    end += uint32_t(surface.size());
    Put32(ends, end);
  }
  sections.push_back(std::move(ends));
  for (Blob& surface : surfaces) {
    sections.push_back(std::move(surface));
  }

  out.clear();
  Put32(out, 0xDEADBABE);
  Put32(out, 2);
  Put32(out, 6);  // 16-bit normals, 16-bit texture coordinates off
  for (float v : lo) {
    PutFloat(out, v);
  }
  for (float v : hi) {
    PutFloat(out, v);
  }
  Put32(out, uint32_t(sections.size()));
  Put32(out, 1);
  for (Blob& section : sections) {
    Pad32(section);
    Put32(out, uint32_t(section.size()));
  }
  Pad32(out);
  for (const Blob& section : sections) {
    out.insert(out.end(), section.begin(), section.end());
  }
  return true;
}

// The fourth material of the template model.
bool TemplateMaterial(const Blob& model, Blob& out) {
  Reader in{model.data(), model.size(), 0x24, true};
  const uint32_t sections = in.U32();
  in.at = (0x2c + size_t(sections) * 4 + 31) & ~size_t(31);
  const uint32_t textures = in.U32();
  in.Skip(size_t(textures) * 4);
  const uint32_t materials = in.U32();
  if (!in.ok || materials <= kTemplateMaterial || !in.Has(size_t(materials) * 4)) {
    return false;
  }
  std::vector<uint32_t> ends;
  for (uint32_t i = 0; i < materials; ++i) {
    ends.push_back(in.U32());
  }
  const uint32_t from = ends[kTemplateMaterial - 1], to = ends[kTemplateMaterial];
  if (to < from + 0x20 || !in.Has(to)) {
    return false;
  }
  out.assign(model.begin() + long(in.at + from), model.begin() + long(in.at + to));
  return true;
}

}  // namespace

const std::vector<HudFrame>& HudFrames() {
  static const std::vector<HudFrame> frames = {
      {"FRME_CombatHud", 0xB10E1DCD}, {"FRME_ScanHud", 0xE47CD0DC}, {"FRME_ThermalHud", 0x143ACA19},
      {"FRME_XRayHudNew", 0x6493BB4F}, {"FRME_BallHud", 0xBF687554}, {"FRME_BaseHud", 0x2F972D0C},
  };
  return frames;
}

bool HudFrameModel(const uint8_t* guif, size_t size, ModelUuid& model) {
  if (!IsFrame(guif, size)) {
    return false;
  }
  // Stored with its first three groups little endian; a pak has them the other way round.
  static const uint8_t kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (size_t i = 0; i < 16; ++i) {
    model[i] = guif[kFrameHeader + kOrder[i]];
  }
  return true;
}

HudConverter::HudConverter(ConvertIO io)
: m_io(std::move(io)), m_nextModel(kModelIds), m_nextTexture(kTextureIds) {}

uint32_t HudConverter::NewId(uint32_t& next) {
  while (m_io.retailId && m_io.retailId(next)) {
    ++next;
  }
  return next++;
}

bool HudConverter::Convert(uint32_t retailFrame, const uint8_t* guif, size_t size, const Model& model,
                           HudCounts& counts, std::string& error) {
  auto log = [&](const std::string& line) {
    if (m_io.log) {
      m_io.log(Hex8(retailFrame) + ": " + line);
    }
  };
  Blob retail;
  if (!m_io.retail || !m_io.write || !m_io.retail(Tag('F', 'R', 'M', 'E'), retailFrame, retail)) {
    error = "the disc has no frame " + Hex8(retailFrame);
    return false;
  }
  uint32_t header[4];
  std::vector<Widget> disc;
  std::vector<RemWidget> rem;
  if (!ParseFrame(retail, header, disc, error) || !ParseRemFrame(guif, size, rem, error)) {
    return false;
  }
  if (m_material.empty()) {
    Blob templateModel;
    if (!m_io.retail(Tag('C', 'M', 'D', 'L'), kTemplateModel, templateModel) ||
        !TemplateMaterial(templateModel, m_material)) {
      error = "the disc's model " + Hex8(kTemplateModel) + " is not usable";
      return false;
    }
  }

  // Where each widget is in its frame, with all its parents applied. A name
  // used twice means its last widget, as it does to the game's lookup.
  std::unordered_map<uint32_t, size_t> remByIndex;
  std::unordered_map<std::string, size_t> remByName;
  for (size_t i = 0; i < rem.size(); ++i) {
    remByIndex[rem[i].index] = i;
    remByName[Lower(rem[i].name)] = i;
  }
  for (RemWidget& w : rem) {  // parents come first: a header's parent is a lower index
    const auto parent = remByIndex.find(w.parent);
    const bool root = w.parent == 0 || parent == remByIndex.end() || !rem[parent->second].placed;
    w.world = root ? w.local : Mul(rem[parent->second].world, w.local);
    w.placed = true;
  }
  auto remWorld = [&](size_t i) { return ToDisc(rem[i].world); };
  std::unordered_map<std::string, size_t> discByName;
  for (size_t i = 0; i < disc.size(); ++i) {
    discByName[disc[i].name] = i;
  }
  for (size_t pass = 0; pass < disc.size(); ++pass) {  // a parent may come after its child
    bool waiting = false;
    for (Widget& w : disc) {
      if (w.placed) {
        continue;
      }
      const auto parent = discByName.find(w.parent);
      if (parent == discByName.end() || &disc[parent->second] == &w) {
        w.world = w.local;
        w.placed = true;
      } else if (disc[parent->second].placed) {
        w.world = Mul(disc[parent->second].world, w.local);
        w.placed = true;
      } else {
        waiting = true;
      }
    }
    if (!waiting) {
      break;
    }
  }
  const std::vector<Widget> original = disc;

  // The disc's widgets, moved to where Remastered has them.
  std::unordered_map<std::string, std::string> newName;  // Remastered's name, lower case -> the output's
  int camera = -1;
  for (size_t i = 0; i < rem.size() && camera < 0; ++i) {
    if (rem[i].type == kCamera) {
      camera = int(i);
    }
  }
  std::vector<Widget> out;
  for (Widget w : disc) {
    std::string name = Lower(w.name);
    for (const auto& alias : kAlias) {
      if (name == alias[0]) {
        name = alias[1];
      }
    }
    const auto found = remByName.find(name);
    if (found != remByName.end()) {
      w.guif = int(found->second);
      newName[Lower(rem[found->second].name)] = w.name;
      switch (w.type) {
      case Tag('B', 'W', 'I', 'G'):
      case Tag('E', 'N', 'R', 'G'):
      case Tag('G', 'R', 'U', 'P'):
      case Tag('M', 'E', 'T', 'R'):
      case Tag('M', 'O', 'D', 'L'):
      case Tag('T', 'X', 'P', 'N'):
        w.world = remWorld(found->second);
        break;
      default:
        break;
      }
    }
    if (w.type == Tag('C', 'A', 'M', 'R') && camera >= 0 && w.typeData.size() >= 20) {
      // The layout is made for Remastered's camera, a 16:9 one. The clip planes stay the disc's.
      const RemWidget& g = rem[size_t(camera)];
      const float zNear = GetFloat(w.typeData, w.typeData.size() - 8);
      const float zFar = GetFloat(w.typeData, w.typeData.size() - 4);
      w.world = remWorld(size_t(camera));
      newName[Lower(g.name)] = w.name;
      w.typeData.clear();
      Put32(w.typeData, g.projection == 0 ? 0 : 1);
      if (g.projection == 0) {
        PutFloat(w.typeData, g.camera[0]);
        PutFloat(w.typeData, g.camera[1]);
      } else {
        PutFloat(w.typeData, std::min(g.camera[0], g.camera[1]));
        PutFloat(w.typeData, std::max(g.camera[0], g.camera[1]));
        PutFloat(w.typeData, std::max(g.camera[2], g.camera[3]));
        PutFloat(w.typeData, std::min(g.camera[2], g.camera[3]));
      }
      PutFloat(w.typeData, zNear);
      PutFloat(w.typeData, zFar);
    }
    for (const auto& anchor : kAnchor) {
      const auto at = remByName.find(anchor[1]);
      if (w.name == anchor[0] && at != remByName.end()) {
        w.world = remWorld(at->second);
      }
    }
    out.push_back(std::move(w));
  }

  // Remastered's own widgets, each after its parent and its parent's children.
  for (const RemWidget& g : rem) {
    if (g.type == kHeadWidget && g.parent == 0) {
      newName[Lower(g.name)] = kHeadName;
    }
  }
  const Widget& pattern = disc[std::min<size_t>(6, disc.size() - 1)];
  for (size_t i = 0; i < rem.size(); ++i) {
    const RemWidget& g = rem[i];
    const std::string name = Lower(g.name);
    const auto parent = remByIndex.find(g.parent);
    if (g.parent == 0 || parent == remByIndex.end()) {
      continue;
    }
    if (rem[parent->second].type == kTextPane) {
      continue;  // shown with its text pane's string; nothing here drives it
    }
    if (newName.count(name) != 0 || g.variant > 1 || EndsWith(name, "_jp") || EndsWith(name, "_ck") ||
        std::any_of(std::begin(kSkip), std::end(kSkip), [&](const char* skip) { return name == skip; })) {
      continue;
    }
    if (g.type != kBaseWidget && g.type != kModel) {
      log("left out " + g.name);
      continue;
    }
    if (g.type == kModel && g.color[3] == 0.0f) {
      continue;  // a damage flash: the game's code for it is not the disc's
    }
    const auto under = newName.find(Lower(rem[parent->second].name));
    if (under == newName.end()) {
      log("no parent for " + g.name);
      continue;
    }
    newName[name] = g.name;
    Widget w;
    w.type = g.type == kBaseWidget ? Tag('B', 'W', 'I', 'G') : Tag('M', 'O', 'D', 'L');
    w.name = g.name;
    w.parent = under->second;
    w.flags[1] = g.flags[0];
    w.flags[2] = g.flags[1];
    std::memcpy(w.color, g.color, sizeof(w.color));
    w.draw = g.draw;
    w.world = remWorld(i);
    std::memcpy(w.tail, pattern.tail, sizeof(w.tail));
    w.guif = int(i);
    w.added = true;
    if (g.type == kModel) {
      w.typeData.assign(kModelSize, 0);
    }
    size_t at = out.size();
    for (size_t k = out.size(); k-- > 0;) {
      if (out[k].name == w.parent || out[k].parent == w.parent) {
        at = k + 1;
        break;
      }
    }
    out.insert(out.begin() + long(at), std::move(w));
  }

  // Models, pictures, text boxes and bars.
  auto texture = [&](const Part& part) -> std::optional<uint32_t> {
    ModelUuid id;
    if (!MaterialTexture(*part.material, id)) {
      return std::nullopt;
    }
    const auto known = m_textures.find(id);
    if (known != m_textures.end()) {
      return known->second == 0 ? std::nullopt : std::optional<uint32_t>(known->second);
    }
    m_textures[id] = 0;
    Image image;
    std::string textureError;
    if (!m_io.texture || !m_io.texture(id, image, textureError) || image.width <= 0 || image.height <= 0) {
      log("texture " + IdToString(id) + ": " + textureError);
      return std::nullopt;
    }
    const int w = std::clamp(NextPow2(image.width), 8, kNativeSize);
    const int h = std::clamp(NextPow2(image.height), 8, kNativeSize);
    if (w != image.width || h != image.height) {
      image = Resize(image, w, h);
    }
    const int edge = std::max(w, h);
    const int sw = edge <= kStubSize ? w : std::max(8, w * kStubSize / edge);
    const int sh = edge <= kStubSize ? h : std::max(8, h * kStubSize / edge);
    const uint32_t tid = NewId(m_nextTexture);
    if ((edge > kStubSize && !m_io.write(Hex8(tid) + ".dds", EncodeDds(image, DdsFormat::BC7))) ||
        !m_io.write(Hex8(tid) + ".TXTR", EncodeTxtrRgba8(edge > kStubSize ? Resize(image, sw, sh) : image, 8))) {
      log("texture " + IdToString(id) + ": cannot write");
      return std::nullopt;
    }
    m_textures[id] = tid;
    ++counts.textures;
    return tid;
  };
  PortHudBars::Bars bars;
  for (Widget& w : out) {
    if (w.type == Tag('M', 'O', 'D', 'L') && w.typeData.size() >= kModelSize) {
      if (w.guif < 0) {
        Set32(w.typeData, 0, kNoModel);  // the disc's art has no place in this layout
        continue;
      }
      const RemWidget& g = rem[size_t(w.guif)];
      std::vector<Part> parts;
      std::vector<uint32_t> textures;
      for (uint32_t mesh : g.meshes) {
        Part part;
        if (!MeshPart(model, mesh, part)) {
          log(g.name + ": mesh " + std::to_string(mesh) + " is not usable");
          continue;
        }
        const std::optional<uint32_t> tid = texture(part);
        if (!tid) {
          log(g.name + ": no picture for " + part.material->name);
          continue;
        }
        const auto slot = std::find(textures.begin(), textures.end(), *tid);
        part.slot = uint32_t(slot - textures.begin());
        if (slot == textures.end()) {
          textures.push_back(*tid);
        }
        parts.push_back(std::move(part));
      }
      uint32_t id = kNoModel;
      Blob file;
      if (!parts.empty() && !BuildModel(parts, textures, m_material, file)) {
        log(g.name + ": too many vertices");
      } else if (!parts.empty()) {
        id = NewId(m_nextModel);
        if (!m_io.write(Hex8(id) + ".CMDL", file)) {
          error = "cannot write a model";
          return false;
        }
        ++counts.models;
      }
      Set32(w.typeData, 0, id);
      Set32(w.typeData, 4, 0);
      std::memcpy(w.color, g.color, sizeof(w.color));
      w.draw = g.draw;
    } else if (w.type == Tag('T', 'X', 'P', 'N') && w.guif >= 0 && w.typeData.size() >= kTextPaneSize) {
      // The box is Remastered's mesh for the text. The disc's text is laid out
      // in whole units of its font, so the box keeps the disc's line height
      // and gets the width that gives its glyphs the proportions they had.
      const RemWidget& g = rem[size_t(w.guif)];
      Part part;
      if (g.meshes.empty() || !MeshPart(model, g.meshes[0], part)) {
        continue;
      }
      double lo[3], hi[3];
      for (int c = 0; c < 3; ++c) {
        lo[c] = hi[c] = part.positions[0][c];
      }
      for (const auto& p : part.positions) {
        for (int c = 0; c < 3; ++c) {
          lo[c] = std::min(lo[c], double(p[c]));
          hi[c] = std::max(hi[c], double(p[c]));
        }
      }
      const double dim[2] = {hi[0] - lo[0], hi[2] - lo[2]};
      const Mat& old = original[discByName[w.name]].world;
      const double extent[2] = {GetFloat(w.typeData, kTextPaneExtent), GetFloat(w.typeData, kTextPaneExtent + 4)};
      const double px = double(GetFloat(w.typeData, 0)) * AxisLength(old, 0) / extent[0];
      const double pz = double(GetFloat(w.typeData, 4)) * AxisLength(old, 2) / extent[1];
      const double pz2 = dim[1] * AxisLength(w.world, 2) / extent[1];
      const double unit = pz2 * px / pz;
      if (std::min({std::abs(pz2), std::abs(px), std::abs(pz), dim[0], dim[1]}) < 1e-9 || !std::isfinite(unit)) {
        log(w.name + ": no box to lay the text out in, the disc's kept");
        continue;
      }
      Mat centre = Identity();
      for (int c = 0; c < 3; ++c) {
        centre.m[c][3] = (lo[c] + hi[c]) / 2.0;
      }
      w.world = Mul(w.world, centre);
      SetFloat(w.typeData, 0, float(dim[0]));
      SetFloat(w.typeData, 4, float(dim[1]));
      for (size_t at = 8; at < 20; at += 4) {
        SetFloat(w.typeData, at, 0.0f);
      }
      SetFloat(w.typeData, kTextPaneExtent, float(std::max(1.0, std::nearbyint(dim[0] * AxisLength(w.world, 0) / unit))));
      SetFloat(w.typeData, kTextPaneExtent + 4, float(std::max(1.0, std::nearbyint(extent[1]))));
    } else if (w.type == Tag('E', 'N', 'R', 'G') && w.guif >= 0 && w.typeData.size() >= 4) {
      const RemWidget& g = rem[size_t(w.guif)];
      Part part;
      if (g.meshes.empty() || !MeshPart(model, g.meshes[0], part)) {
        log(w.name + ": no mesh for the bar");
        continue;
      }
      const std::optional<uint32_t> tid = texture(part);
      PortHudBars::Bar bar;
      bar.name = w.name;
      if (!tid || !Stations(part, bar)) {
        log(w.name + ": the bar's mesh is not a strip");
        continue;
      }
      Set32(w.typeData, 0, *tid);
      w.draw = g.draw;
      bars.push_back(std::move(bar));
    }
  }

  // The frame: every widget relative to its parent again.
  std::unordered_map<std::string, const Widget*> byName;
  for (const Widget& w : out) {
    byName[w.name] = &w;
  }
  Blob file;
  for (uint32_t v : header) {
    Put32(file, v);
  }
  Put32(file, uint32_t(out.size()));
  for (const Widget& w : out) {
    Mat local = w.world;
    const auto parent = byName.find(w.parent);
    Mat inverse;
    if (parent != byName.end() && Inverse(parent->second->world, inverse)) {
      local = Mul(inverse, w.world);
    }
    Put32(file, w.type);
    file.insert(file.end(), w.name.begin(), w.name.end());
    file.push_back(0);
    file.insert(file.end(), w.parent.begin(), w.parent.end());
    file.push_back(0);
    file.insert(file.end(), w.flags, w.flags + 4);
    for (float c : w.color) {
      PutFloat(file, c);
    }
    Put32(file, w.draw);
    file.insert(file.end(), w.typeData.begin(), w.typeData.end());
    file.push_back(w.hasWorker ? 1 : 0);
    if (w.hasWorker) {
      Put16(file, w.worker);
    }
    for (int i = 0; i < 3; ++i) {
      PutFloat(file, float(local.m[i][3]));
    }
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        PutFloat(file, float(local.m[i][j]));
      }
    }
    file.insert(file.end(), w.tail, w.tail + sizeof(w.tail));
  }
  if (!bars.empty()) {
    Blob barFile;
    PortHudBars::WriteFile(bars, barFile);
    if (!m_io.write(Hex8(retailFrame) + ".hudbars", barFile)) {
      error = "cannot write the bars";
      return false;
    }
  }
  if (!m_io.write(Hex8(retailFrame) + ".FRME", file)) {
    error = "cannot write the frame";
    return false;
  }
  counts.widgets += int(out.size());
  counts.bars += int(bars.size());
  return true;
}

}  // namespace PortRemastered
