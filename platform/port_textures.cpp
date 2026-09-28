// Device-aware HD texture replacement loading; see port_textures.h.

#include "port_textures.h"

#include "port_log.h"
#include "port_prompts.h"

#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <aurora/texture.hpp>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

namespace {
// One folder of replacements: the built-in set, or the user's pack over it.
struct Layer {
  const char* label;
  int32_t priority;
  std::string root;
  aurora::texture::ReplacementGroup group;
  bool loaded = false;
};

// The user's pack sits above the built-in set but below the binding icons
// (PortPrompts registers those at priority 2): a remapped action should still
// show the key or button it is bound to, whatever art the pack has for it.
Layer sBuiltIn{"built-in", 0};
Layer sUser{"user", 1};
std::string sDevice = "keyboard";
std::atomic<bool> sUserPending{false};
std::atomic<bool> sUserRemove{false};

// Every name DeviceDirForType can return; used to tell a device-pack root from
// a single device-agnostic pack.
constexpr const char* kDeviceDirs[] = {"xbox", "playstation", "switch", "gamecube", "standard", "keyboard"};

// SDL's gamepad types back PADControllerType, so this mirrors the pad in use.
const char* DeviceDirForType(PADControllerType type) {
  switch (type) {
  case PAD_TYPE_XBOX360:
  case PAD_TYPE_XBOXONE:
    return "xbox";
  case PAD_TYPE_PS3:
  case PAD_TYPE_PS4:
  case PAD_TYPE_PS5:
    return "playstation";
  case PAD_TYPE_SWITCH_PROCON:
  case PAD_TYPE_JOYCON_LEFT:
  case PAD_TYPE_JOYCON_RIGHT:
  case PAD_TYPE_JOYCON_PAIR:
    return "switch";
  case PAD_TYPE_GAMECUBE:
  case PAD_TYPE_NSO_GAMECUBE:
    return "gamecube";
  case PAD_TYPE_STANDARD:
    return "standard";
  default:
    return "keyboard";
  }
}

// The override, else the input in use. Never allocates.
const char* ResolveDeviceName() { return PortPrompts::ActiveDevice(); }

// Prefer the device folder, falling back to the root when it is absent. When
// the root holds device folders but not the one we need, load nothing: the
// directory scan is recursive, so falling back would merge the other devices'
// packs into this one. A root that does not exist loads nothing either, since
// Aurora would create it.
std::optional<std::filesystem::path> ResolveDirectory(const std::string& root) {
  const std::filesystem::path base(root);
  std::error_code ec;
  if (!std::filesystem::is_directory(base, ec)) {
    return std::nullopt;
  }
  const std::filesystem::path device = base / sDevice;
  if (std::filesystem::is_directory(device, ec)) {
    return device;
  }
  for (const char* name : kDeviceDirs) {
    if (std::filesystem::is_directory(base / name, ec)) {
      return std::nullopt;
    }
  }
  return base;
}

void Unload(Layer& layer) {
  if (layer.loaded) {
    aurora::texture::unregister_replacements(layer.group);
    layer.group = {};
    layer.loaded = false;
  }
}

void Load(Layer& layer) {
  if (layer.root.empty()) {
    return;
  }
  const std::optional<std::filesystem::path> dir = ResolveDirectory(layer.root);
  if (!dir.has_value()) {
    Unload(layer);
    std::error_code ec;
    if (std::filesystem::is_directory(layer.root, ec)) {
      PortLog::Write("metroid_prime_port: no %s texture replacements for device '%s' in %s\n",
                     layer.label, sDevice.c_str(), layer.root.c_str());
    }
    return;
  }
  const aurora::texture::ReplacementOptions options{.priority = layer.priority};
  if (layer.loaded) {
    aurora::texture::reload_replacement_directory(*dir, layer.group, options);
  } else {
    layer.group = aurora::texture::load_replacement_directory(*dir, options);
    layer.loaded = true;
  }
  PortLog::Write("metroid_prime_port: loaded %zu %s texture replacements from %s (device: %s)\n",
                 layer.group.registrations.size(), layer.label, dir->string().c_str(), sDevice.c_str());
}

// A new pack is copied beside the live one and swapped in here, on the main
// thread, so the folder never changes under registrations that still point
// into it. A copy finished after the game last ran is picked up at startup.
void ApplyPendingUserPack() {
  if (sUser.root.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path root(sUser.root);
  const std::filesystem::path pending(sUser.root + ".new");
  const bool remove = sUserRemove.exchange(false, std::memory_order_acq_rel);
  const bool swap = std::filesystem::is_directory(pending, ec);
  if (!remove && !swap) {
    return;
  }
  Unload(sUser);
  // The old pack is moved aside rather than deleted first, so a failed install
  // leaves the user with the pack they had instead of none.
  const std::filesystem::path old(sUser.root + ".old");
  std::filesystem::remove_all(old, ec);
  const bool hadPack = std::filesystem::exists(root, ec);
  if (hadPack) {
    std::filesystem::rename(root, old, ec);
    if (ec) {
      PortLog::Write("metroid_prime_port: could not replace %s: %s\n", root.string().c_str(),
                     ec.message().c_str());
      return;
    }
  }
  if (swap && !remove) {
    std::filesystem::rename(pending, root, ec);
    if (ec) {
      PortLog::Write("metroid_prime_port: could not install the texture pack from %s: %s\n",
                     pending.string().c_str(), ec.message().c_str());
      if (hadPack) {
        std::filesystem::rename(old, root, ec);
      }
      return;
    }
  } else if (swap) {
    std::filesystem::remove_all(pending, ec);
  }
  std::filesystem::remove_all(old, ec);
}
} // namespace

namespace PortTextures {
void Initialize(const char* root, const char* userRoot) {
  sBuiltIn.root = root != nullptr ? root : "";
  sUser.root = userRoot != nullptr ? userRoot : "";
  sDevice = ResolveDeviceName();
  ApplyPendingUserPack();
  Load(sBuiltIn);
  Load(sUser);
}

void Poll() {
  if (sUserPending.exchange(false, std::memory_order_acq_rel)) {
    ApplyPendingUserPack();
    Load(sUser);
  }
  const char* device = ResolveDeviceName();
  if (sDevice == device) {
    return;
  }
  sDevice = device;
  Load(sBuiltIn);
  Load(sUser);
}

void RequestUserPackReload() { sUserPending.store(true, std::memory_order_release); }

void RequestUserPackRemoval() {
  sUserRemove.store(true, std::memory_order_release);
  sUserPending.store(true, std::memory_order_release);
}

const char* UserRoot() { return sUser.root.c_str(); }

size_t UserPackCount() { return sUser.loaded ? sUser.group.registrations.size() : 0; }

const char* DeviceName() { return sDevice.c_str(); }

const char* PadDeviceName() { return DeviceDirForType(PADGetControllerType(PAD_CHAN0)); }
} // namespace PortTextures
