// Reruns a list of model conversions through PortRemastered::Converter, for
// comparing against the reference converter's output. Everything it reads is
// the developer's own extracted data; see build/mpr/gc/cpp_prep.py.
//
//   port_remastered_convert_tool <retail dump> <remastered dir> <tag prefix> <out dir> [retail id...]

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "port_remastered_convert.h"
#include "port_remastered_pak.h"
#include "port_remastered_txtr.h"

using namespace PortRemastered;

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

static std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string item;
  std::istringstream in(s);
  while (std::getline(in, item, sep)) {
    out.push_back(item);
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: %s <retail dump> <remastered dir> <tag prefix> <out dir> [retail id...]\n", argv[0]);
    return 2;
  }
  const std::string dump = argv[1], remDir = argv[2], prefix = argv[3], outDir = argv[4];
  std::set<std::string> only;
  for (int i = 5; i < argc; ++i) {
    only.insert(argv[i]);
  }
  std::set<uint32_t> ids;
  {
    std::vector<uint8_t> raw;
    if (!ReadFile(dump + "/ids.bin", raw)) {
      std::fprintf(stderr, "no ids.bin in %s\n", dump.c_str());
      return 2;
    }
    for (size_t i = 0; i + 4 <= raw.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, &raw[i], 4);
      ids.insert(v);
    }
  }
  std::vector<std::vector<std::string>> jobs;
  {
    std::ifstream f(dump + "/jobs.tsv");
    std::string line;
    while (std::getline(f, line)) {
      auto cols = Split(line, '\t');
      if (cols.size() == 11 && (only.empty() || only.count(cols[1]))) {
        jobs.push_back(std::move(cols));
      }
    }
  }
  std::atomic<size_t> next{0};
  std::atomic<int> failed{0};
  std::mutex print;
  auto work = [&](int worker) {
    auto make = [&](const std::string& where) {
      ConvertIO io;
      io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
        char name[32];
        std::snprintf(name, sizeof(name), "/%s/%08X", FourCCString(type).c_str(), id);
        return ReadFile(dump + name, out);
      };
      io.retailId = [&](uint32_t id) { return ids.count(id) != 0; };
      io.texture = [&](const ModelUuid& id, Image& out, std::string& error) {
        std::vector<uint8_t> raw;
        if (!ReadFile(remDir + "/" + IdToString(id) + ".TXTR", raw)) {
          error = "not extracted";
          return false;
        }
        TxtrImage img;
        if (!DecodeTxtr(raw.data(), raw.size(), img, error)) {
          return false;
        }
        out.width = int(img.width);
        out.height = int(img.height);
        out.rgba = std::move(img.rgba);
        return true;
      };
      io.write = [&, where, worker](const std::string& name, const std::vector<uint8_t>& data) {
        const std::string path = outDir + "/" + where + "/" + name;
        const std::string tmp = path + "." + std::to_string(worker);
        {
          std::ofstream f(tmp, std::ios::binary);
          f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
          if (!f) {
            return false;
          }
        }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
      };
      if (!only.empty()) {
        io.log = [&](const std::string& line) {
          std::lock_guard<std::mutex> lock(print);
          std::printf("%s\n", line.c_str());
        };
      }
      return new Converter(std::move(io));
    };
    Converter* pbr = make("pbr");
    Converter* player = make("player");
    for (size_t i = next++; i < jobs.size(); i = next++) {
      const auto& j = jobs[i];
      std::string error;
      std::vector<uint8_t> raw;
      Model model;
      bool ok = ReadFile(remDir + "/" + j[2] + ".CMDL", raw) || ReadFile(remDir + "/" + j[2] + ".SMDL", raw);
      if (!ok) {
        error = "model not extracted";
      }
      ok = ok && ParseModel(raw.data(), raw.size(), model, error);
      if (ok) {
        ConvertOptions opt;
        opt.retail = uint32_t(std::strtoul(j[1].c_str(), nullptr, 16));
        const auto m = Split(j[3], ','), off = Split(j[4], ',');
        for (int k = 0; k < 9; ++k) {
          opt.orient[k / 3][k % 3] = std::strtod(m[k].c_str(), nullptr);
        }
        for (int k = 0; k < 3; ++k) {
          opt.offset[k] = std::strtod(off[k].c_str(), nullptr);
        }
        if (j[5] != "-") {
          for (const auto& s : Split(j[5], ',')) {
            opt.skins.push_back(uint32_t(std::strtoul(s.c_str(), nullptr, 16)));
          }
        }
        opt.material = std::atoi(j[6].c_str());
        opt.maxTexture = std::atoi(j[7].c_str());
        opt.pbr = j[8] == "1";
        if (j[9] != "-") {
          const auto sq = Split(j[9], ',');
          opt.squeeze = true;
          opt.squeezeRole = sq[0];
          opt.squeezeFrom[0] = std::strtod(sq[1].c_str(), nullptr);
          opt.squeezeFrom[1] = std::strtod(sq[2].c_str(), nullptr);
          opt.squeezeTo[0] = std::strtod(sq[3].c_str(), nullptr);
          opt.squeezeTo[1] = std::strtod(sq[4].c_str(), nullptr);
        }
        opt.skip = j[10] == "-" ? std::vector<std::string>() : Split(j[10], ',');
        opt.texturePrefix = prefix + "/" + j[2] + "/";
        opt.textureSuffix = ".png";
        ok = (j[0] == "pbr" ? pbr : player)->Convert(model, opt, error);
      }
      if (!ok) {
        ++failed;
        std::lock_guard<std::mutex> lock(print);
        std::printf("FAIL %s %s: %s\n", j[1].c_str(), j[2].c_str(), error.c_str());
      }
    }
    delete pbr;
    delete player;
  };
  std::vector<std::thread> threads;
  const int count = only.empty() ? int(std::max(1u, std::thread::hardware_concurrency())) : 1;
  for (int i = 0; i < count; ++i) {
    threads.emplace_back(work, i);
  }
  for (auto& t : threads) {
    t.join();
  }
  std::printf("%zu models, %d failed\n", jobs.size(), failed.load());
  return failed ? 1 : 0;
}
