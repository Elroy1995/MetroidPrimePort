// Extracts every asset of a Remastered pak, the way `retrotool pak extract`
// does, to check the C++ pak reader against it: same file names, same bytes.
//
//   tool <file.pak> <outdir>
//
// Assets are named after their first string-table name, or their UUID when they
// have none, with the FourCC as the extension. A duplicate name overwrites, as
// it does in retrotool (it truncates the file it opens).

#include "port_remastered_pak.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class FileReader {
public:
  bool Open(const std::string& path, std::string& error) {
    m_stream.open(path, std::ios::binary);
    if (!m_stream.is_open()) {
      error = "could not open '" + path + "'";
      return false;
    }
    m_stream.seekg(0, std::ios::end);
    m_size = uint64_t(m_stream.tellg());
    m_stream.seekg(0, std::ios::beg);
    return true;
  }

  uint64_t Size() const { return m_size; }

  bool Read(uint64_t offset, void* out, size_t size) {
    m_stream.clear();
    m_stream.seekg(std::streamoff(offset));
    if (!m_stream) {
      return false;
    }
    m_stream.read(static_cast<char*>(out), std::streamsize(size));
    return m_stream.gcount() == std::streamsize(size);
  }

private:
  std::ifstream m_stream;
  uint64_t m_size = 0;
};

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " <file.pak> <outdir>\n";
    return 2;
  }
  FileReader reader;
  std::string error;
  if (!reader.Open(argv[1], error)) {
    std::cerr << error << "\n";
    return 1;
  }

  PortRemastered::Pak pak;
  if (!pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                reader.Size(), error)) {
    std::cerr << error << "\n";
    return 1;
  }

  const std::filesystem::path outDir(argv[2]);
  std::error_code code;
  std::filesystem::create_directories(outDir, code);

  size_t written = 0;
  uint64_t bytes = 0;
  for (const PortRemastered::PakAsset& asset : pak.Assets()) {
    // First name, else the UUID: retrotool's file naming.
    const std::string stem = asset.names.empty() ? PortRemastered::IdToString(asset.id) : asset.names.front();
    const std::filesystem::path path = outDir / (stem + "." + PortRemastered::FourCCString(asset.type));

    std::vector<uint8_t> data;
    if (!pak.ReadAsset(asset, data, error)) {
      std::cerr << error << "\n";
      return 1;
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, code);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      std::cerr << "could not write '" << path.string() << "'\n";
      return 1;
    }
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    file.close();
    if (!file) {
      std::cerr << "could not write '" << path.string() << "'\n";
      return 1;
    }
    written += 1;
    bytes += data.size();
  }
  std::cout << pak.Assets().size() << " assets, " << written << " files, " << bytes << " bytes\n";
  return 0;
}