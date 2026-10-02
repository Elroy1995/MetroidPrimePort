// Dumps one parsed Metroid Prime Remastered model (see platform/port_remastered_cmdl.h)
// next to what retrotool's `cmdl convert` writes, so the two can be compared.
//
//   tool <model.CMDL|model.SMDL> <outdir>
//
// Out of <outdir>:
//   materials.tsv  every material parameter, in exactly the layout the patched
//                  retrotool writes (build/mpr/gc/retrotool-materials.patch):
//                  one line per parameter, tab separated, FourCCs quoted.
//   vbuf<N>.bin    the decoded vertex attributes of VBUF entry N (see below).
//   mesh<N>.bin    mesh N's material, buffer references and indices (see below).
//   model.txt      a short summary, for the check script's coverage report.
//
// The vbuf/mesh layout is this tool's own, and the check script reads it:
//   vbuf<N>.bin: "RMD1", u32 vertexCount, u32 attributeCount, then per attribute
//                u32 nameLength, the name, u32 componentsPerVertex, u32 isInteger,
//                then vertexCount*components values: f32 when isInteger is 0, u32
//                when it is 1.
//   mesh<N>.bin: "RSH1", u32 material, u32 vertexBuffer, u32 indexBuffer,
//                u32 indexStart, u32 indexCount, u32 indexWidth, u32 vertexCount,
//                then indexCount indices as u32. All little endian.
//
// Floats are printed the way Rust's Display prints f32, which is what
// materials.tsv holds: the shortest decimal that reads back as the same float,
// never in exponent form, so 0.5 is "0.5" and 45 is "45".

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "port_remastered_cmdl.h"

namespace {

std::string FormatUuid(const std::array<uint8_t, 16>& id) {
  // The stored bytes are the little endian UUID retrotool reads with from_bytes_le,
  // which reverses the first three groups and leaves the last as it is. The same
  // mapping IdToString() applies in port_remastered_pak.cpp.
  static const char kDigits[] = "0123456789abcdef";
  static const int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  static const int kStart[] = {0, 4, 6, 8, 10};
  static const int kLength[] = {4, 2, 2, 2, 6};
  std::string text;
  text.reserve(36);
  for (int group = 0; group < 5; ++group) {
    if (group > 0) {
      text += '-';
    }
    for (int i = 0; i < kLength[group]; ++i) {
      const uint8_t byte = id[static_cast<size_t>(kOrder[kStart[group] + i])];
      text += kDigits[byte >> 4];
      text += kDigits[byte & 0x0F];
    }
  }
  return text;
}

// A FourCC as retrotool's Debug for it prints it: the four characters in quotes.
std::string FormatFourCC(uint32_t value) {
  std::string text = "\"";
  for (int i = 3; i >= 0; --i) {
    text += char(value >> (8 * i));
  }
  text += '"';
  return text;
}

std::string FormatFourCCText(uint32_t value) {
  std::string text;
  for (int i = 3; i >= 0; --i) {
    text += char(value >> (8 * i));
  }
  return text;
}

// Rust's Display for f32: the shortest decimal that reads back as the same value,
// written out in full (Rust never uses exponent form here). Nine digits is the most
// a float needs, so the shortest is found by asking for one more digit at a time.
std::string FormatFloat(float value) {
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value < 0.0f ? "-inf" : "inf";
  }
  char buffer[64];
  int precision = 1;
  for (; precision <= 9; ++precision) {
    std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, static_cast<double>(value));
    if (std::strtof(buffer, nullptr) == value) {
      break;
    }
  }
  // buffer is now "-d.dddde+xx": pull it apart and lay the digits out positionally.
  const char* p = buffer;
  std::string sign;
  if (*p == '-') {
    sign = "-";
    ++p;
  }
  std::string digits;
  int exponent = 0;
  for (; *p != '\0' && *p != 'e' && *p != 'E'; ++p) {
    if (*p >= '0' && *p <= '9') {
      digits += *p;
    }
  }
  if (*p == 'e' || *p == 'E') {
    exponent = int(std::strtol(p + 1, nullptr, 10));
  }
  while (digits.size() > 1 && digits[digits.size() - 1] == '0') {
    digits.resize(digits.size() - 1);
  }
  std::string out = sign;
  if (exponent < 0) {
    out += "0.";
    out.append(static_cast<size_t>(-exponent - 1), '0');
    out += digits;
    return out;
  }
  if (static_cast<size_t>(exponent) + 1 >= digits.size()) {
    out += digits;
    out.append(static_cast<size_t>(exponent) + 1 - digits.size(), '0');
    return out;
  }
  out += digits.substr(0, static_cast<size_t>(exponent) + 1);
  out += '.';
  out += digits.substr(static_cast<size_t>(exponent) + 1);
  return out;
}

// The tail every texture line of the tsv ends with: the id, and the sampler block
// when the slot has one.
std::string FormatTexture(const PortRemastered::ModelTextureRef& texture) {
  std::string out = FormatUuid(texture.id);
  if (texture.hasUsage) {
    out += '\t' + std::to_string(texture.texCoord) + '\t' + std::to_string(texture.wrapX) + '\t' +
           std::to_string(texture.wrapY);
  }
  return out;
}

std::string FormatMaterialsTsv(const PortRemastered::Model& model) {
  using PortRemastered::ModelMaterialData;
  std::string out;
  for (const PortRemastered::ModelMaterial& material : model.materials) {
    out += "material\t" + material.name + '\t' + FormatUuid(material.shaderId) + '\n';
    for (uint32_t type : material.types) {
      out += "type\t" + FormatFourCC(type) + '\n';
    }
    for (const PortRemastered::ModelRenderType& render : material.renderTypes) {
      out += "render\t" + FormatFourCC(render.dataId) + '\t' + FormatFourCC(render.dataType) + '\t' +
             std::to_string(render.flag1) + '\t' + std::to_string(render.flag2) + '\n';
    }
    for (const ModelMaterialData& data : material.data) {
      // The parameter tag is printed through the derived Debug of EMaterialDataId,
      // which prints the variant bare; only FourCC (types, render types) quotes.
      const std::string id = FormatFourCCText(data.usage);
      switch (data.kind) {
        case ModelMaterialData::Kind::Texture:
          out += "texture\t" + id + '\t' + FormatTexture(data.texture) + '\n';
          break;
        case ModelMaterialData::Kind::Color:
          out += "color\t" + id;
          for (int i = 0; i < 4; ++i) {
            out += '\t' + FormatFloat(data.color[i]);
          }
          out += '\n';
          break;
        case ModelMaterialData::Kind::Scalar:
          out += "scalar\t" + id + '\t' + FormatFloat(data.scalar) + '\n';
          break;
        case ModelMaterialData::Kind::Int1:
          out += "int\t" + id + '\t' + std::to_string(data.int1) + '\n';
          break;
        case ModelMaterialData::Kind::Int4:
          // retrotool has no case for these two, so the derived Debug of the value
          // (the variant name and its payload) lands in the file verbatim.
          out += "other\t" + id + "\tInt4(CVector4i { x: " + std::to_string(data.int4[0]) +
                 ", y: " + std::to_string(data.int4[1]) + ", z: " + std::to_string(data.int4[2]) +
                 ", w: " + std::to_string(data.int4[3]) + " })\n";
          break;
        case ModelMaterialData::Kind::Matrix4:
          out += "other\t" + id + "\tMat4(CMatrix4f { m: [";
          for (int i = 0; i < 16; ++i) {
            out += (i > 0 ? ", " : "") + FormatFloat(data.matrix4[i]);
          }
          out += "] })\n";
          break;
        case ModelMaterialData::Kind::LayeredTexture:
          out += "layered\t" + id + '\t' + std::to_string(data.layeredUnknown) + '\t' +
                 std::to_string(unsigned(data.layeredFlags)) + '\n';
          for (int layer = 0; layer < 3; ++layer) {
            out += "layer\t" + id;
            for (int c = 0; c < 4; ++c) {
              out += '\t' + FormatFloat(data.layeredColors[static_cast<size_t>(layer * 4 + c)]);
            }
            out += '\t' + FormatTexture(data.layeredTextures[static_cast<size_t>(layer)]) + '\n';
          }
          break;
      }
    }
  }
  return out;
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 24));
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  return written;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
  return WriteBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

// Every attribute of one VBUF entry, named the way the glTF names it where it has
// a name, so the check compares like with like. The port's named arrays are turned
// into ModelAttributes here, and the components it keeps under their own names
// (TANGENT_1 and the rest) go out as they are, for the script to map onto glTF's
// "_" prefixed custom names.
std::vector<PortRemastered::ModelAttribute> DumpAttributes(
    const PortRemastered::ModelVertexBuffer& buffer) {
  using PortRemastered::ModelAttribute;
  struct Named {
    const char* name;
    uint32_t components;
    bool isInteger;
    const std::vector<float>* floats;
    const std::vector<uint16_t>* joints;
  };
  const Named named[] = {
      {"POSITION", 3, false, &buffer.positions, nullptr}, {"NORMAL", 3, false, &buffer.normals, nullptr},
      {"TANGENT", 4, false, &buffer.tangents, nullptr},  {"COLOR", 4, false, &buffer.colors, nullptr},
      {"WEIGHTS_0", 4, false, &buffer.weights, nullptr}, {"JOINTS_0", 4, true, nullptr, &buffer.joints},
  };
  std::vector<ModelAttribute> attributes;
  for (const Named& entry : named) {
    const bool present = entry.joints ? !entry.joints->empty() : (entry.floats && !entry.floats->empty());
    if (!present) {
      continue;
    }
    ModelAttribute attribute;
    attribute.name = entry.name;
    attribute.components = entry.components;
    attribute.isInteger = entry.isInteger;
    if (entry.isInteger) {
      for (uint16_t joint : *entry.joints) {
        attribute.uints.push_back(joint);
      }
    } else {
      attribute.data = *entry.floats;
    }
    attributes.push_back(attribute);
  }
  for (uint32_t set = 0; set < buffer.uvs.size(); ++set) {
    if (buffer.uvs[set].empty()) {
      continue;
    }
    ModelAttribute attribute;
    attribute.name = "TEXCOORD_" + std::to_string(set);
    attribute.components = 2;
    attribute.data = buffer.uvs[set];
    attributes.push_back(attribute);
  }
  for (const ModelAttribute& attribute : buffer.attributes) {
    attributes.push_back(attribute);
  }
  return attributes;
}

bool WriteVertexBuffer(const std::filesystem::path& path,
                       const PortRemastered::ModelVertexBuffer& buffer) {
  const std::vector<PortRemastered::ModelAttribute> attributes = DumpAttributes(buffer);
  std::vector<uint8_t> out;
  out.insert(out.end(), {'R', 'M', 'D', '1'});
  PutU32(out, buffer.vertexCount);
  PutU32(out, uint32_t(attributes.size()));
  for (const PortRemastered::ModelAttribute& attribute : attributes) {
    PutU32(out, uint32_t(attribute.name.size()));
    out.insert(out.end(), attribute.name.begin(), attribute.name.end());
    PutU32(out, attribute.components);
    PutU32(out, attribute.isInteger ? 1u : 0u);
    if (attribute.isInteger) {
      for (uint32_t value : attribute.uints) {
        PutU32(out, value);
      }
    } else {
      for (float value : attribute.data) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        PutU32(out, bits);
      }
    }
  }
  return WriteBytes(path, out);
}

bool WriteMesh(const std::filesystem::path& path, const PortRemastered::ModelMesh& mesh) {
  std::vector<uint8_t> out;
  out.insert(out.end(), {'R', 'S', 'H', '1'});
  PutU32(out, mesh.material);
  PutU32(out, mesh.vertexBuffer);
  PutU32(out, mesh.indexBuffer);
  PutU32(out, mesh.indexStart);
  PutU32(out, mesh.indexCount);
  PutU32(out, mesh.indexWidth);
  PutU32(out, mesh.vertexCount);
  for (uint32_t index : mesh.indices) {
    PutU32(out, index);
  }
  return WriteBytes(path, out);
}

std::string FormatSummary(const PortRemastered::Model& model) {
  std::string out = "form " + FormatFourCCText(model.form) + " reader " +
                    std::to_string(model.readerVersion) + " writer " + std::to_string(model.writerVersion) +
                    "\n";
  out += std::string("skinned ") + (model.skinned ? "1" : "0") + "\n";
  out += "materials " + std::to_string(model.materials.size()) + "\n";
  out += "vertexBuffers " + std::to_string(model.vertexBuffers.size()) + "\n";
  out += "meshes " + std::to_string(model.meshes.size()) + "\n";
  out += "lodMeshes " + std::to_string(model.lodMeshes.size()) + "\n";
  out += "lods " + std::to_string(model.lods.size()) + " rules " + std::to_string(model.lodRules.size()) +
         "\n";
  out += "bounds";
  for (int i = 0; i < 3; ++i) {
    out += " " + FormatFloat(model.boundsMin[i]);
  }
  for (int i = 0; i < 3; ++i) {
    out += " " + FormatFloat(model.boundsMax[i]);
  }
  out += "\n";
  for (const PortRemastered::ModelVertexBuffer& buffer : model.vertexBuffers) {
    out += "vbuf " + std::to_string(buffer.vertexCount);
    for (size_t set = 0; set < buffer.uvs.size(); ++set) {
      out += buffer.uvs[set].empty() ? std::string(" -") : std::string(" uv");
    }
    out += buffer.tangents.empty() ? std::string(" -") : std::string(" tangent");
    out += buffer.colors.empty() ? std::string(" -") : std::string(" color");
    out += buffer.joints.empty() ? std::string(" -") : std::string(" joints");
    for (const PortRemastered::ModelAttribute& attribute : buffer.attributes) {
      out += " " + attribute.name;
    }
    out += "\n";
  }
  for (const PortRemastered::ModelMesh& mesh : model.meshes) {
    out += "mesh " + std::to_string(mesh.material) + " " + std::to_string(mesh.vertexBuffer) + " " +
           std::to_string(mesh.indexBuffer) + " " + std::to_string(mesh.indexStart) + " " +
           std::to_string(mesh.indexCount) + " " + std::to_string(mesh.indexWidth) + "\n";
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <model.CMDL|model.SMDL> <outdir>\n", argv[0]);
    return 2;
  }
  std::FILE* input = std::fopen(argv[1], "rb");
  if (input == nullptr) {
    std::fprintf(stderr, "%s: cannot open %s\n", argv[0], argv[1]);
    return 1;
  }
  std::vector<uint8_t> data;
  uint8_t chunk[64 * 1024];
  for (;;) {
    const size_t got = std::fread(chunk, 1, sizeof(chunk), input);
    if (got == 0) {
      break;
    }
    data.insert(data.end(), chunk, chunk + got);
  }
  std::fclose(input);

  PortRemastered::Model model;
  std::string error;
  if (!PortRemastered::ParseModel(data.data(), data.size(), model, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }

  std::error_code code;
  std::filesystem::create_directories(argv[2], code);
  if (code) {
    std::fprintf(stderr, "%s: cannot create %s: %s\n", argv[0], argv[2], code.message().c_str());
    return 1;
  }
  const std::filesystem::path outDir(argv[2]);
  if (!WriteText(outDir / "materials.tsv", FormatMaterialsTsv(model))) {
    std::fprintf(stderr, "%s: cannot write materials.tsv\n", argv[0]);
    return 1;
  }
  if (!WriteText(outDir / "model.txt", FormatSummary(model))) {
    std::fprintf(stderr, "%s: cannot write model.txt\n", argv[0]);
    return 1;
  }
  for (size_t i = 0; i < model.vertexBuffers.size(); ++i) {
    const std::string name = "vbuf" + std::to_string(i) + ".bin";
    if (!WriteVertexBuffer(outDir / name, model.vertexBuffers[i])) {
      std::fprintf(stderr, "%s: cannot write %s\n", argv[0], name.c_str());
      return 1;
    }
  }
  for (size_t i = 0; i < model.meshes.size(); ++i) {
    const std::string name = "mesh" + std::to_string(i) + ".bin";
    if (!WriteMesh(outDir / name, model.meshes[i])) {
      std::fprintf(stderr, "%s: cannot write %s\n", argv[0], name.c_str());
      return 1;
    }
  }
  return 0;
}