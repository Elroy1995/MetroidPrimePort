// Mods folder startup (port_mods.h): finds the mods, works out which disc files
// they replace and which PAKs their loose resources patch, and registers the
// result with Aurora's DVD overlays.

#include "port_mods.h"

#include "port_debug.h"
#include "port_hd_font.h"
#include "port_log.h"
#include "port_room_env.h"
#include "port_room_geo.h"

#include <aurora/dvd.h>
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <dolphin/dvd.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace PortMods {
namespace {

Status sStatus;
// Registered with Aurora; filled once, so the pointers handed out stay put.
std::vector<std::shared_ptr<const VirtualFile>> sFiles;
std::vector<std::string> sOverlayNames;

// <id>.dds files, and the textures (by owner) currently drawn from one.
std::unordered_map<uint32_t, std::string> sNativeTextures;
// <MREA id>.roomenv files, by area.
std::unordered_map<uint32_t, std::string> sRoomEnvs;
std::string sFont;
std::unordered_map<uint32_t, std::string> sRoomGeos;
struct BoundTexture {
  aurora::texture::ReplacementRegistration registration;
  uint32_t id = 0;
};
std::unordered_map<const void*, BoundTexture> sBoundTextures;
// Reload: the textures that were bound, to bind again from the new set.
std::vector<std::pair<const void*, uint32_t>> sRebind;
bool sOverlaysRegistered = false;

std::string PathString(const fs::path& path) {
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

fs::path PathFromString(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return text;
}

bool EndsWith(const std::string& text, const char* suffix) {
  const size_t length = std::strlen(suffix);
  return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// --- Base disc access -------------------------------------------------------

struct BaseFile {
  void* handle = nullptr;
  int64_t pos = -1;
};

void* OpenBase(const VirtualFile& file) {
  void* handle = aurora_dvd_base_open(file.sourceEntry);
  return handle != nullptr ? new BaseFile{handle, -1} : nullptr;
}

int64_t ReadBaseAt(void* opaque, uint64_t offset, uint8_t* buffer, size_t length) {
  auto* file = static_cast<BaseFile*>(opaque);
  if (file->pos != int64_t(offset)) {
    if (aurora_dvd_base_seek(file->handle, int64_t(offset), 0) != int64_t(offset)) {
      file->pos = -1;
      return -1;
    }
    file->pos = int64_t(offset);
  }
  size_t done = 0;
  while (done < length) {
    const int64_t got = aurora_dvd_base_read(file->handle, buffer + done, length - done);
    if (got <= 0) {
      break;
    }
    done += size_t(got);
  }
  file->pos += int64_t(done);
  return int64_t(done);
}

void CloseBase(void* opaque) {
  auto* file = static_cast<BaseFile*>(opaque);
  aurora_dvd_base_close(file->handle);
  delete file;
}

const SourceIo sBaseIo{OpenBase, ReadBaseAt, CloseBase};

void* OverlayOpen(void* userData) {
  const auto& file = *static_cast<const std::shared_ptr<const VirtualFile>*>(userData);
  return new Reader(file, &sBaseIo);
}

void OverlayClose(void* handle) { delete static_cast<Reader*>(handle); }

int64_t OverlayRead(void* handle, uint8_t* buffer, size_t length) {
  return static_cast<Reader*>(handle)->Read(buffer, length);
}

int64_t OverlaySeek(void* handle, int64_t offset, int32_t whence) {
  return static_cast<Reader*>(handle)->Seek(offset, whence);
}

// The PAK's header: its first bytes, read until the table fits.
bool ReadPakHeader(const std::string& hostPath, int32_t entry, std::vector<uint8_t>& header, uint64_t& size,
                   PakTable& table) {
  VirtualFile probe;
  probe.sourceEntry = entry;
  Segment whole;
  if (!hostPath.empty()) {
    std::error_code ec;
    size = fs::file_size(PathFromString(hostPath), ec);
    if (ec) {
      return false;
    }
    whole.kind = Segment::kHost;
    whole.hostPath = hostPath;
    whole.hostSize = size;
  } else {
    void* handle = aurora_dvd_base_open(entry);
    if (handle == nullptr) {
      return false;
    }
    const int64_t end = aurora_dvd_base_seek(handle, 0, 2);
    aurora_dvd_base_close(handle);
    if (end < 0) {
      return false;
    }
    size = uint64_t(end);
    whole.kind = Segment::kSource;
  }
  whole.length = size;
  probe.size = size;
  probe.segments.push_back(whole);
  Reader reader(std::make_shared<const VirtualFile>(std::move(probe)), &sBaseIo);

  size_t want = 64 * 1024;
  for (;;) {
    want = size_t(std::min<uint64_t>(want, size));
    header.resize(want);
    reader.Seek(0, 0);
    if (reader.Read(header.data(), want) != int64_t(want)) {
      return false;
    }
    size_t needed = 0;
    if (ParsePakTable(header.data(), header.size(), table, needed)) {
      return true;
    }
    if (needed == 0 || needed > size || want >= size) {
      return false;
    }
    want = std::max(needed, want * 2);
  }
}

std::string DiscPathFor(int32_t entry) {
  char path[512];
  if (!DVDConvertEntrynumToPath(entry, path, sizeof(path))) {
    return {};
  }
  std::string text = path;
  if (text.empty() || text[0] != '/') {
    text.insert(text.begin(), '/');
  }
  return text;
}

void Message(std::string text) {
  PortLog::Write("mods: %s\n", text.c_str());
  sStatus.messages.push_back(std::move(text));
}

// A mod's own notes are not disc files, and saying so for each is noise.
bool IsDocument(const std::string& name) {
  const std::string lower = Lower(name);
  return EndsWith(lower, ".txt") || EndsWith(lower, ".md") || EndsWith(lower, ".json") ||
         EndsWith(lower, ".png") || EndsWith(lower, ".jpg") || EndsWith(lower, ".ini") || lower == "license";
}

struct Replacement {
  std::string hostPath;
  uint64_t size = 0;
  size_t mod = 0;
};

} // namespace

std::string Folder() {
  std::string dir;
  if (const char* env = std::getenv("MP_MODS"); env != nullptr && env[0] != '\0') {
    dir = env;
  } else {
    if (const char* env = std::getenv("MP_USER_PATH"); env != nullptr && env[0] != '\0') {
      dir = env;
    } else if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
      dir = pref;
      SDL_free(pref);
    } else {
      return {};
    }
    if (dir.back() != '/' && dir.back() != '\\') {
      dir += '/';
    }
    dir += "mods";
  }
  std::error_code ec;
  fs::create_directories(PathFromString(dir), ec);
  return dir;
}

bool HasRoomGeometry() {
  const std::string dir = Folder();
  if (dir.empty()) {
    return false;
  }
  std::error_code ec;
  for (fs::recursive_directory_iterator it(PathFromString(dir), fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    uint32_t id = 0;
    if (PortRoomGeo::ParseFileName(PathString(it->path().filename()), id)) {
      return true;
    }
  }
  return false;
}

const Status& CurrentStatus() { return sStatus; }

// Every .pak on the disc, as (entryNum, path).
std::vector<std::pair<int32_t, std::string>> DiscPaks() {
  std::vector<std::pair<int32_t, std::string>> paks;
  std::vector<std::string> dirs{"/"};
  while (!dirs.empty()) {
    const std::string dirPath = dirs.back();
    dirs.pop_back();
    DVDDir dir;
    if (!DVDOpenDir(dirPath.c_str(), &dir)) {
      continue;
    }
    DVDDirEntry entry;
    while (DVDReadDir(&dir, &entry)) {
      if (entry.name == nullptr) {
        continue;
      }
      const std::string path = dirPath + entry.name;
      if (entry.isDir) {
        dirs.push_back(path + "/");
      } else if (EndsWith(Lower(path), ".pak")) {
        paks.emplace_back(int32_t(entry.entryNum), path);
      }
    }
    DVDCloseDir(&dir);
  }
  std::sort(paks.begin(), paks.end());
  return paks;
}

bool HasNativeTexture(uint32_t id) { return sNativeTextures.count(id) != 0; }

bool BindTexture(const void* owner, uint32_t id) {
  UnbindTexture(owner);
  const auto found = sNativeTextures.find(id);
  if (found == sNativeTextures.end()) {
    return false;
  }
  const auto registration = aurora::texture::register_file_replacement(
      aurora::texture::TexturePointerKey{owner}, PathFromString(found->second));
  if (registration.id == 0) {
    return false;
  }
  sBoundTextures[owner] = {registration, id};
  return true;
}

void UnbindTexture(const void* owner) {
  const auto found = sBoundTextures.find(owner);
  if (found != sBoundTextures.end()) {
    aurora::texture::unregister_replacement(found->second.registration);
    sBoundTextures.erase(found);
  }
}

std::string FontPath() { return sFont; }

std::string RoomEnvPath(uint32_t mrea) {
  const auto found = sRoomEnvs.find(mrea);
  return found != sRoomEnvs.end() ? found->second : std::string();
}

std::string RoomGeoPath(uint32_t mrea) {
  const auto found = sRoomGeos.find(mrea);
  return found != sRoomGeos.end() ? found->second : std::string();
}

size_t NativeTextureCount() { return sNativeTextures.size(); }
size_t NativeTexturesBound() { return sBoundTextures.size(); }

void BeginReload() {
  // Its models go while the pool they came from is still there.
  PortRoomGeo::Reset();
  sRebind.clear();
  for (const auto& [owner, bound] : sBoundTextures) {
    aurora::texture::unregister_replacement(bound.registration);
    sRebind.emplace_back(owner, bound.id);
  }
  sBoundTextures.clear();
}

void FinishReload() {
  Initialize();
  PortRoomEnv::Reset();
  PortHdFont::Reset();
  // A texture whose image is gone keeps its stub until the game loads it again.
  for (const auto& [owner, id] : sRebind) {
    BindTexture(owner, id);
  }
  sRebind.clear();
}

void Initialize() {
  sStatus = {};
  sNativeTextures.clear();
  sRoomEnvs.clear();
  sFont.clear();
  sRoomGeos.clear();
  sStatus.folder = Folder();
  sStatus.active = PortDebug::ModsEnabled();
  if (sStatus.folder.empty()) {
    return;
  }
  const std::vector<std::string> disabled = SplitDisabled(PortDebug::ModsDisabled());

  std::error_code ec;
  std::vector<fs::path> modDirs;
  for (fs::directory_iterator it(PathFromString(sStatus.folder), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = PathString(it->path().filename());
    if (!name.empty() && name[0] != '.' && it->is_directory(ec)) {
      modDirs.push_back(it->path());
    }
  }
  std::sort(modDirs.begin(), modDirs.end());

  // Keyed by disc entry and by (type, id); a later mod overwrites an earlier.
  std::map<int32_t, Replacement> replaced;
  std::map<std::pair<uint32_t, uint32_t>, LooseResource> loose;
  std::map<std::pair<uint32_t, uint32_t>, size_t> looseMod;
  for (const fs::path& modDir : modDirs) {
    ModInfo& info = sStatus.mods.emplace_back();
    info.name = PathString(modDir.filename());
    info.enabled = sStatus.active && std::find(disabled.begin(), disabled.end(), info.name) == disabled.end();
    if (!info.enabled) {
      continue;
    }
    const size_t modIndex = sStatus.mods.size() - 1;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(modDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      const std::string name = PathString(it->path().filename());
      if (!name.empty() && name[0] == '.') {
        if (it->is_directory(ec)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (it->is_regular_file(ec)) {
        files.push_back(it->path());
      }
    }
    std::sort(files.begin(), files.end());
    std::unordered_map<uint32_t, std::string> native;
    for (const fs::path& file : files) {
      const std::string name = PathString(file.filename());
      const std::string relative = PathString(file.lexically_relative(modDir).generic_u8string());
      const uint64_t size = fs::file_size(file, ec);
      if (ec) {
        Message(info.name + ": cannot read " + relative);
        continue;
      }
      uint32_t type = 0;
      uint32_t id = 0;
      if (ParseNativeTextureName(name, id)) {
        native[id] = PathString(file);
        continue;
      }
      if (PortHdFont::ParseFileName(name)) {
        sFont = PathString(file);
        continue;
      }
      if (PortRoomEnv::ParseFileName(name, id)) {
        sRoomEnvs[id] = PathString(file);
        continue;
      }
      if (PortRoomGeo::ParseFileName(name, id)) {
        sRoomGeos[id] = PathString(file);
        continue;
      }
      if (ParseLooseName(name, type, id)) {
        if (size == 0) {
          Message(info.name + ": " + relative + " is empty; ignored");
          continue;
        }
        loose[{type, id}] = {type, id, PathString(file), size, info.name};
        looseMod[{type, id}] = modIndex;
        if (type == 0x54585452) { // 'TXTR': an earlier mod's image was made for another texture
          sNativeTextures.erase(id);
        }
        continue;
      }
      const int32_t entry = DVDConvertPathToEntrynum(("/" + relative).c_str());
      DVDDir dir;
      if (entry < 0 || entry >= aurora_dvd_base_entry_count() || DVDFastOpenDir(entry, &dir)) {
        if (!IsDocument(name)) {
          Message(info.name + ": " + relative + " is not a disc file; ignored");
        }
        continue;
      }
      if (size >= 0xFFFFFFE0u) {
        Message(info.name + ": " + relative + " is too big; ignored");
        continue;
      }
      replaced[entry] = {PathString(file), size, modIndex};
    }
    info.textures = int(native.size());
    for (auto& [id, path] : native) {
      sNativeTextures[id] = std::move(path);
    }
  }

  // Which loose resources each PAK takes. Ids no PAK holds are added to
  // NoARAM.pak, which the game keeps loaded from boot.
  std::set<std::pair<uint32_t, uint32_t>> used;
  std::map<int32_t, std::shared_ptr<VirtualFile>> patched;
  if (!loose.empty()) {
    struct Pending {
      int32_t entry;
      std::string path;
      std::string hostPath;
      std::vector<uint8_t> header;
      uint64_t size = 0;
      PakTable table;
      std::vector<const LooseResource*> take;
    };
    std::vector<Pending> pending;
    std::set<uint32_t> knownIds;
    int32_t homeEntry = -1;
    for (const auto& [entry, path] : DiscPaks()) {
      const auto whole = replaced.find(entry);
      Pending pak{entry, path, whole != replaced.end() ? whole->second.hostPath : std::string()};
      if (!ReadPakHeader(pak.hostPath, entry, pak.header, pak.size, pak.table)) {
        Message("cannot read the table of " + path + (pak.hostPath.empty() ? "" : " (from a mod)"));
        continue;
      }
      std::set<std::pair<uint32_t, uint32_t>> taken;
      for (const PakResource& resource : pak.table.resources) {
        knownIds.insert(resource.id);
        const auto found = loose.find({resource.type, resource.id});
        if (found != loose.end() && taken.insert(found->first).second) {
          pak.take.push_back(&found->second);
          used.insert(found->first);
        }
      }
      if (Lower(path) == "/noaram.pak") {
        homeEntry = entry;
      } else if (pak.take.empty()) {
        continue;
      }
      pending.push_back(std::move(pak));
    }
    std::vector<const LooseResource*> added;
    for (const auto& [key, resource] : loose) {
      if (used.count(key) != 0) {
        continue;
      }
      const std::string name = PathString(PathFromString(resource.hostPath).filename());
      if (knownIds.count(key.second) != 0) {
        Message(resource.mod + ": " + name + " reuses an id another type holds; ignored");
      } else if (homeEntry < 0) {
        Message(resource.mod + ": " + name + " is in no PAK and NoARAM.pak is missing; ignored");
      } else {
        added.push_back(&resource);
        used.insert(key);
      }
    }
    for (Pending& pak : pending) {
      const bool home = pak.entry == homeEntry;
      if (pak.take.empty() && (!home || added.empty())) {
        continue;
      }
      auto file = std::make_shared<VirtualFile>(PatchPak(pak.header, pak.table, pak.size, pak.take, pak.hostPath,
                                                         home ? added : std::vector<const LooseResource*>{}));
      if (file->size == 0 || file->size >= 0xFFFFFFE0u) {
        Message("cannot patch " + pak.path);
        continue;
      }
      file->discPath = pak.path;
      file->sourceEntry = pak.entry;
      patched[pak.entry] = std::move(file);
    }
    for (const auto& [key, resource] : loose) {
      if (used.count(key) != 0) {
        ++sStatus.mods[looseMod[key]].resources;
      }
    }
  }

  if (!sNativeTextures.empty()) {
    PortLog::Write("mods: %d native texture(s)\n", int(sNativeTextures.size()));
  }

  // Aurora holds pointers into the previous set until the new one is registered.
  const std::vector<std::shared_ptr<const VirtualFile>> previous = std::move(sFiles);
  sFiles.clear();
  sOverlayNames.clear();
  for (const auto& [entry, replacement] : replaced) {
    ++sStatus.mods[replacement.mod].files;
    if (patched.count(entry) != 0) {
      continue;
    }
    auto file = std::make_shared<VirtualFile>();
    file->discPath = DiscPathFor(entry);
    file->sourceEntry = entry;
    file->size = replacement.size;
    Segment whole;
    whole.kind = Segment::kHost;
    whole.length = replacement.size;
    whole.hostPath = replacement.hostPath;
    whole.hostSize = replacement.size;
    if (whole.length > 0) {
      file->segments.push_back(whole);
    }
    sFiles.push_back(std::move(file));
  }
  for (auto& [entry, file] : patched) {
    sFiles.push_back(std::move(file));
  }
  if (sFiles.empty()) {
    if (sOverlaysRegistered) {
      aurora_dvd_overlay_files(nullptr, 0, nullptr);
      sOverlaysRegistered = false;
    }
    if (!sStatus.mods.empty()) {
      PortLog::Write("mods: nothing to apply from %s\n", sStatus.folder.c_str());
    }
    return;
  }

  std::vector<AuroraOverlayFile> overlays;
  overlays.reserve(sFiles.size());
  for (const auto& file : sFiles) {
    overlays.push_back({file->discPath.c_str(), const_cast<std::shared_ptr<const VirtualFile>*>(&file),
                        size_t(file->size)});
  }
  static const AuroraOverlayCallbacks callbacks{OverlayOpen, OverlayClose, OverlayRead, OverlaySeek};
  aurora_dvd_overlay_callbacks(&callbacks);
  std::vector<s32> entries(overlays.size(), -1);
  aurora_dvd_overlay_files(overlays.data(), overlays.size(), entries.data());
  sOverlaysRegistered = true;
  for (size_t i = 0; i < overlays.size(); ++i) {
    if (entries[i] < 0) {
      Message("Aurora refused " + sFiles[i]->discPath);
    } else {
      ++sStatus.overlays;
    }
  }
  for (const ModInfo& mod : sStatus.mods) {
    if (mod.enabled) {
      PortLog::Write("mods: %s: %d file(s), %d resource(s), %d native texture(s)\n", mod.name.c_str(), mod.files,
                     mod.resources, mod.textures);
    }
  }
  PortLog::Write("mods: %d disc file(s) served from %s\n", sStatus.overlays, sStatus.folder.c_str());
}

} // namespace PortMods
