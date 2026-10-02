#include "port_remastered_import.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <aurora/dvd.h>

#include "port_mods.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_nsp.h"
#include "port_remastered_pak.h"
#include "port_remastered_room.h"
#include "port_remastered_table.h"
#include "port_remastered_txtr.h"
#include "port_ws.h"

namespace PortRemastered {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kSMDL = 0x534D444C;
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMREA = 0x4D524541;

constexpr const char* kStagingName = ".remastered-models.importing";
// Written last, so a staging folder without it is an import that was cut short.
constexpr const char* kMarkerName = "import-complete";
constexpr const char* kRoomFolder = "roomenv";
constexpr const char* kGeometryFolder = "roomgeo";
// Largest edge of a room geometry texture: there are thousands of them.
constexpr int kGeometryTexture = 1024;

// The rooms whose geometry is imported, from MP_REMASTERED_GEOMETRY: "all", or
// room names (any part of one) separated by commas. None without it, the
// port's drawing of room geometry being unfinished.
bool WantsGeometry(const std::string& room) {
  const char* env = std::getenv("MP_REMASTERED_GEOMETRY");
  if (env == nullptr || env[0] == '\0') {
    return false;
  }
  const std::string list = env;
  if (list == "all") {
    return true;
  }
  for (size_t at = 0; at <= list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at && room.find(list.substr(at, comma - at)) != std::string::npos) {
      return true;
    }
    at = comma + 1;
  }
  return false;
}

std::mutex sStateMutex;
ImportState sState;
std::thread sThread;
std::atomic<bool> sCancel{false};

fs::path PathFromString(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

void SetMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.message = message;
}

void AddLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.lines.push_back(line);
  if (sState.lines.size() > kImportMaxLines) {
    sState.lines.erase(sState.lines.begin(), sState.lines.end() - kImportMaxLines);
  }
}

void Finish(bool ok, const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.running = false;
  sState.finished = true;
  sState.ok = ok;
  sState.cancelled = !ok && sCancel.load();
  sState.message = message;
}

// --- The retail disc ----------------------------------------------------------

// The CMDL, CSKR, TXTR, MLVL and MREA resources of the unmodded disc, and every id on it.
class Retail {
public:
  ~Retail() {
    for (auto& [entry, handle] : m_handles) {
      aurora_dvd_base_close(handle);
    }
  }

  bool Index(std::string& error) {
    const int32_t baseCount = aurora_dvd_base_entry_count();
    for (const auto& [entry, path] : PortMods::DiscPaks()) {
      if (entry < 0 || entry >= baseCount) {
        continue;  // a PAK only a mod brings
      }
      void* handle = aurora_dvd_base_open(entry);
      if (handle == nullptr) {
        continue;
      }
      m_handles[entry] = handle;
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(needed);
        const size_t got = ReadAt(handle, 0, header.data(), header.size());
        if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
          parsed = true;
          break;
        }
        if (got < header.size() || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      for (const PortMods::PakResource& res : table.resources) {
        m_ids.insert(res.id);
        if (res.type == kCMDL || res.type == kCSKR || res.type == kTXTR || res.type == kMLVL || res.type == kMREA) {
          m_resources.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
        }
      }
    }
    if (m_resources.empty()) {
      error = "no models found on the disc";
      return false;
    }
    return true;
  }

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      return false;
    }
    const Where& where = found->second;
    std::vector<uint8_t> raw(where.size);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
        return false;
      }
    }
    if (!where.compressed) {
      out = std::move(raw);
      return true;
    }
    // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
    // data, and a checksum the decoder never reaches.
    if (raw.size() < 6) {
      return false;
    }
    const size_t length = size_t(raw[0]) << 24 | size_t(raw[1]) << 16 | size_t(raw[2]) << 8 | size_t(raw[3]);
    PortWs::Inflater inflater;
    inflater.SetKeepWindow(false);
    std::string inflated;
    if (!inflater.InflateMessage(std::string(raw.begin() + 6, raw.end()), inflated, length) ||
        inflated.size() != length) {
      return false;
    }
    out.assign(inflated.begin(), inflated.end());
    return true;
  }

private:
  struct Where {
    int32_t entry;
    uint32_t offset;
    uint32_t size;
    bool compressed;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return uint64_t(type) << 32 | id; }

  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
      return 0;
    }
    size_t done = 0;
    while (done < size) {
      const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }

  std::mutex m_mutex;
  std::map<int32_t, void*> m_handles;
  std::unordered_map<uint64_t, Where> m_resources;
  std::unordered_set<uint32_t> m_ids;
};

// --- The Remastered image ------------------------------------------------------

// Every model and texture in the image's paks, by id.
class Remastered {
public:
  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
    if (!m_nsp.Open(nspPath, keysPath, error)) {
      return false;
    }
    std::vector<const RomfsFile*> files;
    for (const RomfsFile& file : m_nsp.Files()) {
      if (file.path.size() > 4 && file.path.compare(file.path.size() - 4, 4, ".pak") == 0) {
        files.push_back(&file);
      }
    }
    for (size_t i = 0; i < files.size(); ++i) {
      if (sCancel) {
        error = "cancelled";
        return false;
      }
      SetMessage("Reading the paks (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");
      const RomfsFile* file = files[i];
      auto pak = std::make_unique<Pak>();
      std::string pakError;
      const ReadFn read = [this, file](uint64_t offset, void* out, size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string ignored;
        return m_nsp.Read(*file, offset, out, size, ignored);
      };
      if (!pak->Open(read, file->size, pakError)) {
        AddLine(file->path + ": " + pakError);
        continue;
      }
      const std::vector<PakAsset>& assets = pak->Assets();
      for (size_t a = 0; a < assets.size(); ++a) {
        const uint32_t type = assets[a].type;
        if (type == kCMDL || type == kSMDL) {
          m_models.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kTXTR) {
          m_textures.emplace(assets[a].id, Where{m_paks.size(), a});
        }
      }
      m_paks.push_back(std::move(pak));
      m_paths.push_back(file->path);
    }
    if (m_models.empty()) {
      error = "no models in this image; is it Metroid Prime Remastered?";
      return false;
    }
    return true;
  }

  bool ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_models, id, out, error);
  }
  bool ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_textures, id, out, error);
  }

  // The paks of one world directory ("Intro_Master") as the room writer takes
  // them, and every pak of the image for the assets rooms share.
  void World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const {
    const std::string folder = "/!" + dir + "/";
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      const size_t at = path.find(folder);
      if (at == std::string::npos) {
        continue;
      }
      const std::string name = path.substr(at + folder.size(), path.size() - at - folder.size() - 4);
      if (name == "!" + dir) {
        master = RoomPak{dir, m_paks[i].get()};
      } else if (name.find('/') == std::string::npos) {
        rooms.push_back(RoomPak{name, m_paks[i].get()});
      }
    }
  }
  std::vector<RoomPak> AllPaks() const {
    std::vector<RoomPak> all;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      all.push_back(RoomPak{m_paths[i], m_paks[i].get()});
    }
    return all;
  }

private:
  struct Where {
    size_t pak;
    size_t asset;
  };
  using Index = std::unordered_map<std::array<uint8_t, 16>, Where, PakIdHash>;

  bool Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = index.find(id);
    if (found == index.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  Nsp m_nsp;
  // Nsp::Read is for one thread at a time.
  mutable std::mutex m_mutex;
  std::vector<std::unique_ptr<Pak>> m_paks;
  std::vector<std::string> m_paths;  // of m_paks, in the image
  Index m_models;
  Index m_textures;
};

// --- The import ------------------------------------------------------------------

fs::path StagingFolder() {
  const std::string mods = PortMods::Folder();
  return mods.empty() ? fs::path() : PathFromString(mods) / kStagingName;
}

ConvertOptions OptionsFor(const TableEntry& entry) {
  ConvertOptions options;
  options.retail = entry.retail;
  for (int i = 0; i < 9; ++i) {
    options.orient[i / 3][i % 3] = entry.orient[i];
  }
  for (int i = 0; i < 3; ++i) {
    options.offset[i] = entry.offset[i];
  }
  const uint32_t* skins = TableSkins(entry);
  options.skins.assign(skins, skins + entry.skinCount);
  options.pbr = entry.pbr;
  if (const TableOptions* extra = TableExtra(entry)) {
    options.material = extra->material;
    options.maxTexture = extra->maxTexture;
    if (extra->squeezeRole != nullptr) {
      options.squeeze = true;
      options.squeezeRole = extra->squeezeRole;
      options.squeezeFrom[0] = extra->squeeze[0];
      options.squeezeFrom[1] = extra->squeeze[1];
      options.squeezeTo[0] = extra->squeeze[2];
      options.squeezeTo[1] = extra->squeeze[3];
    }
  }
  return options;
}

void Run(std::string nspPath, std::string keysPath, int threads, fs::path staging) {
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    Finish(false, "Cannot create the mod folder: " + ec.message());
    return;
  }
  auto fail = [&](const std::string& message) {
    fs::remove_all(staging, ec);
    Finish(false, sCancel ? std::string("Cancelled.") : message);
  };

  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }

  size_t count = 0;
  const TableEntry* table = Table(count);
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.total = int(count);
  }
  std::atomic<size_t> next{0};
  std::atomic<int> converted{0};
  // Ids the import has given out: no two resources of it share one, whatever their types.
  std::mutex takenMutex;
  std::unordered_set<uint32_t> taken;
  auto makeIO = [&](int worker, const fs::path& folder) {
    ConvertIO io;
    io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    io.retailId = [&](uint32_t id) { return retail.HasId(id); };
    io.texture = [&](const ModelUuid& id, Image& out, std::string& textureError) {
      std::vector<uint8_t> raw;
      TxtrImage image;
      if (!remastered.ReadTexture(id, raw, textureError) ||
          !DecodeTxtr(raw.data(), raw.size(), image, textureError)) {
        return false;
      }
      out.width = int(image.width);
      out.height = int(image.height);
      out.rgba = std::move(image.rgba);
      return true;
    };
    // Workers can meet the same texture at once; each writes its own temporary
    // file and the rename decides, the content being the same either way.
    io.write = [&, worker, folder](const std::string& name, const std::vector<uint8_t>& data) {
      {
        std::lock_guard<std::mutex> lock(takenMutex);
        taken.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
      }
      const fs::path path = folder / PathFromString(name);
      const fs::path tmp = folder / PathFromString(name + ".tmp" + std::to_string(worker));
      {
        std::ofstream file(tmp, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) {
          return false;
        }
      }
      std::error_code renameError;
      fs::rename(tmp, path, renameError);
      return !renameError;
    };
    return io;
  };
  auto work = [&](int worker) {
    Converter converter(makeIO(worker, staging));
    for (size_t i = next++; i < count && !sCancel; i = next++) {
      const TableEntry& entry = table[i];
      ModelUuid id;
      std::memcpy(id.data(), entry.rem, 16);
      std::string modelError;
      std::vector<uint8_t> raw;
      Model model;
      const bool ok = remastered.ReadModel(id, raw, modelError) &&
                      ParseModel(raw.data(), raw.size(), model, modelError) &&
                      converter.Convert(model, OptionsFor(entry), modelError);
      if (ok) {
        ++converted;
      } else {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X", entry.retail);
        AddLine(std::string(name) + ": " + modelError);
      }
      std::lock_guard<std::mutex> lock(sStateMutex);
      ++sState.done;
      if (!ok) {
        ++sState.failed;
      }
      sState.message = "Converting models (" + std::to_string(sState.done) + "/" + std::to_string(sState.total) + ")";
    }
  };
  SetMessage("Converting models");
  std::vector<std::thread> workers;
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(work, i);
  }
  work(0);
  for (std::thread& worker : workers) {
    worker.join();
  }

  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  if (converted == 0) {
    fail("No model could be converted.");
    return;
  }

  // The rooms' reflection cubes and baked ambient light, a file per area. A
  // world that cannot be read costs its rooms their environment, not the import.
  SetMessage("Writing the room environments");
  const fs::path roomFolder = staging / kRoomFolder;
  fs::create_directories(roomFolder, ec);
  fs::create_directories(staging / kGeometryFolder, ec);
  const std::vector<RoomPak> allPaks = remastered.AllPaks();
  const std::vector<RoomWorld>& worlds = RoomWorlds();
  std::atomic<size_t> nextWorld{0};
  std::atomic<int> roomFiles{0};
  // A room's geometry names its models; they are given ids here and converted
  // afterwards, on every thread. A model that then fails leaves its id naming
  // nothing, which the port skips.
  const fs::path geometryFolder = staging / kGeometryFolder;
  struct GeometryModel {
    ModelUuid uuid;
    uint32_t id;
  };
  std::vector<GeometryModel> geometry;
  std::unordered_map<ModelUuid, uint32_t, PakIdHash> geometryIds;
  auto geometryId = [&](const ModelUuid& uuid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = geometryIds.find(uuid);
    if (known != geometryIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u;  // FNV-1a
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometryIds.emplace(uuid, id);
    geometry.push_back({uuid, id});
    return true;
  };
  auto roomWork = [&] {
    for (size_t i = nextWorld++; i < worlds.size() && !sCancel; i = nextWorld++) {
      RoomPak master;
      std::vector<RoomPak> rooms;
      remastered.World(worlds[i].dir, master, rooms);
      RoomIO io;
      io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
      io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
        const bool isGeometry = name.size() > 8 && name.compare(name.size() - 8, 8, ".roomgeo") == 0;
        std::ofstream file((isGeometry ? geometryFolder : roomFolder) / PathFromString(name), std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        return bool(file);
      };
      io.model = geometryId;
      io.wantsGeometry = WantsGeometry;
      io.cancelled = [] { return sCancel.load(); };
      int written = 0;
      std::string worldError;
      if (!WriteWorldRoomEnvs(worlds[i].mlvl, master, rooms, allPaks, io, written, worldError) && !sCancel) {
        AddLine(std::string(worlds[i].dir) + ": " + worldError);
      }
      roomFiles += written;
    }
  };
  workers.clear();
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(roomWork);
  }
  roomWork();
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }

  std::atomic<int> geometryDone{0};
  if (!geometry.empty()) {
    std::unordered_set<uint32_t> modelIds;
    for (const GeometryModel& g : geometry) {
      modelIds.insert(g.id);
    }
    std::atomic<size_t> nextModel{0};
    std::atomic<int> seen{0};
    std::mutex claimMutex;
    std::unordered_set<uint32_t> claimed;
    auto geometryWork = [&](int worker) {
      ConvertIO io = makeIO(worker, geometryFolder);
      // A texture never takes a geometry model's id either.
      io.retailId = [&](uint32_t id) { return retail.HasId(id) || modelIds.count(id) != 0; };
      io.claim = [&](uint32_t id) {
        std::lock_guard<std::mutex> lock(claimMutex);
        return claimed.insert(id).second;
      };
      Converter converter(std::move(io));
      for (size_t i = nextModel++; i < geometry.size() && !sCancel; i = nextModel++) {
        ConvertOptions options;
        options.retail = geometry[i].id;
        options.standalone = true;
        options.nativeMax = kGeometryTexture;
        std::string modelError;
        std::vector<uint8_t> raw;
        Model model;
        if (remastered.ReadModel(geometry[i].uuid, raw, modelError) &&
            ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError)) {
          ++geometryDone;
        } else {
          char name[16];
          std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
          AddLine(std::string("room model ") + name + ": " + modelError);
        }
        SetMessage("Converting room models (" + std::to_string(++seen) + "/" + std::to_string(geometry.size()) + ")");
      }
    };
    workers.clear();
    for (int i = 1; i < threads; ++i) {
      workers.emplace_back(geometryWork, i);
    }
    geometryWork(0);
    for (std::thread& worker : workers) {
      worker.join();
    }
    if (sCancel) {
      fail("Cancelled.");
      return;
    }
  }
  if (geometry.empty()) {
    fs::remove(geometryFolder, ec);
  }
  {
    std::ofstream marker(staging / kMarkerName);
    if (!marker) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  const int failed = int(count) - converted.load();
  std::string message = std::to_string(converted.load()) + " models converted";
  if (failed != 0) {
    message += ", " + std::to_string(failed) + " failed";
  }
  message += ", " + std::to_string(roomFiles.load()) + " room environments";
  if (!geometry.empty()) {
    message += ", " + std::to_string(geometryDone.load()) + " of " + std::to_string(geometry.size()) + " room models";
  }
  Finish(true, message + ".");
}

}  // namespace

std::string DefaultKeysPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    home = std::getenv("USERPROFILE");
  }
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  const fs::path path = PathFromString(home) / ".switch" / "prod.keys";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return {};
  }
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const fs::path staging = StagingFolder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (staging.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  if (threads <= 0) {
    threads = std::max(1, int(std::thread::hardware_concurrency()) - 2);
  }
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(Run, nspPath, keysPath, threads, staging);
  return true;
}

ImportState ImportStatus() {
  std::lock_guard<std::mutex> lock(sStateMutex);
  return sState;
}

void CancelImport() { sCancel = true; }

void StopImport() {
  sCancel = true;
  if (sThread.joinable()) {
    sThread.join();
  }
}

bool ApplyPendingImport() {
  const fs::path staging = StagingFolder();
  std::error_code ec;
  // A running import is still writing there.
  if (staging.empty() || ImportStatus().running || !fs::is_directory(staging, ec)) {
    return false;
  }
  if (!fs::exists(staging / kMarkerName, ec)) {
    fs::remove_all(staging, ec);
    return false;
  }
  const fs::path target = staging.parent_path() / kImportModName;
  fs::remove_all(target, ec);
  fs::rename(staging, target, ec);
  if (ec) {
    return false;
  }
  fs::remove(target / kMarkerName, ec);
  return true;
}

int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath) {
  std::string keys = keysPath.empty() ? DefaultKeysPath() : keysPath;
  if (keys.empty()) {
    std::fprintf(stderr, "no key file given and no ~/.switch/prod.keys\n");
    return 2;
  }
  if (!StartImport(nspPath, keys, int(std::max(1u, std::thread::hardware_concurrency())))) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  if (!state.ok) {
    return 1;
  }
  if (!ApplyPendingImport()) {
    std::fprintf(stderr, "could not move the mod into %s\n", PortMods::Folder().c_str());
    return 1;
  }
  std::printf("installed as %s/%s\n", PortMods::Folder().c_str(), kImportModName);
  return 0;
}

}  // namespace PortRemastered
