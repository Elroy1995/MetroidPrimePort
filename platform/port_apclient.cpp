#include "port_apclient.h"
#include "port_log.h"

#include "port_ap_protocol.h"
#include "port_ws.h"

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PortAp {
namespace {

using Protocol::Config;
using Protocol::ItemGrant;
using Protocol::Session;

bool EnvEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::string UserDirectory() {
  if (const char* env = std::getenv("MP_USER_PATH")) {
    if (env[0] != '\0')
      return env;
  }
  if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
    std::string dir(pref);
    SDL_free(pref);
    return dir;
  }
  return ".";
}

std::string ConfigPath() {
  if (const char* env = std::getenv("MP_AP_CONFIG")) {
    if (env[0] != '\0')
      return env;
  }
  return (std::filesystem::path(UserDirectory()) / "archipelago.json").string();
}

std::string ErrorText(const char* text) {
  return text != nullptr && text[0] != '\0' ? text : "connection failed";
}

rstl::wstring ToHudWide(const std::string& text) {
  std::wstring wide;
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    uint32_t codepoint = 0xfffd;
    size_t length = 1;
    if (first < 0x80) {
      codepoint = first;
    } else {
      size_t expected = 0;
      uint32_t minimum = 0;
      if (first >= 0xc2 && first <= 0xdf) {
        expected = 2;
        minimum = 0x80;
        codepoint = first & 0x1f;
      } else if (first >= 0xe0 && first <= 0xef) {
        expected = 3;
        minimum = 0x800;
        codepoint = first & 0x0f;
      } else if (first >= 0xf0 && first <= 0xf4) {
        expected = 4;
        minimum = 0x10000;
        codepoint = first & 0x07;
      }
      bool valid = expected != 0 && i + expected <= text.size();
      for (size_t offset = 1; valid && offset < expected; ++offset) {
        const uint8_t continuation = static_cast<uint8_t>(text[i + offset]);
        if ((continuation & 0xc0) != 0x80) {
          valid = false;
          break;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
      valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
              !(codepoint >= 0xd800 && codepoint <= 0xdfff);
      if (valid) {
        length = expected;
      } else {
        codepoint = 0xfffd;
      }
    }
    i += length;
    if constexpr (sizeof(wchar_t) >= 4) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else if (codepoint <= 0xffff) {
      wide.push_back(static_cast<wchar_t>(codepoint));
    } else {
      codepoint -= 0x10000;
      wide.push_back(static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
      wide.push_back(static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff)));
    }
  }
  return rstl::wstring(wide.c_str());
}

struct Runtime {
  ~Runtime() {
    stop.store(true, std::memory_order_release);
    wake.notify_all();
    if (worker.joinable())
      worker.join();
  }

  void LogStateLocked(const std::string& message) {
    if (lastLogged != message) {
      PortLog::Write( "archipelago: %s\n", message.c_str());
      lastLogged = message;
    }
  }

  void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    connected = false;
    stateLabel = "error";
    lastError = error;
    LogStateLocked(error);
  }

  bool WaitBackoff(int seconds) {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_for(lock, std::chrono::seconds(seconds), [this] {
      return stop.load(std::memory_order_acquire);
    });
    return !stop.load(std::memory_order_acquire);
  }

  void SaveStateLocked() {
    if (session != nullptr)
      Protocol::SaveStateFile(statePath, session->GetState());
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  std::thread worker;
  bool attempted = false;
  bool enabled = false;
  bool connected = false;
  std::string stateLabel = "off";
  std::string lastError;
  std::string lastLogged;
  std::string lastMessage;
  std::string statePath;
  Config config;
  std::unique_ptr<Session> session;
  std::deque<ItemGrant> grants;
  std::deque<int64_t> queuedChecks;
  // DeathLink: a death noticed on the game thread, waiting for the socket
  // thread to announce it. Non-empty means one is owed.
  std::string pendingBounce;
  std::deque<std::string> notifications;
  int itemCount = 0;
  int checkCount = 0;
  // Locations collected in game that the table has no id for. Counted and
  // reported once, because dropping them is otherwise invisible.
  int unmappedCount = 0;
  // DeathLink: whether this client's death has already been announced, so a
  // death is sent once rather than on every tick the flag stays clear for.
  bool deathAnnounced = false;
};

Runtime& GetRuntime() {
  static Runtime runtime;
  return runtime;
}

void CountChecks(Runtime& runtime, size_t count) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                       runtime.checkCount));
  runtime.checkCount += static_cast<int>(std::min(count, room));
}

bool SendPacket(PortWs::Client& client, const std::string& packet, std::string& error) {
  if (client.SendText(packet))
    return true;
  error = ErrorText(client.Error());
  return false;
}

void Worker(Runtime& runtime) {
  Config config;
  std::string statePath;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    config = runtime.config;
    statePath = runtime.statePath;
  }

  int backoff = 5;
  while (!runtime.stop.load(std::memory_order_acquire)) {
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.connected = false;
      runtime.stateLabel = "connecting";
      runtime.LogStateLocked("connecting to " + config.server);
    }

    std::string host;
    std::string path;
    uint16_t port = 0;
    bool secure = false;
    std::string connectionError;
    PortWs::Client client;
    PortWs::TlsOptions tls;
    tls.caFile = config.tlsCa;
    bool transportReady = false;
    if (!PortWs::ParseUrl(config.server, host, port, path, secure)) {
      connectionError = "invalid server URL: " + config.server;
    } else if (!client.Connect(host, port, path, 10000, secure, tls)) {
      connectionError = ErrorText(client.Error());
    } else {
      transportReady = true;
      client.SetTimeoutMs(10000);
    }

    bool connectionFailed = !transportReady;
    bool apConnected = false;
    if (!transportReady) {
      runtime.SetError(connectionError);
    } else {
      while (!runtime.stop.load(std::memory_order_acquire) && client.IsOpen()) {
        std::string message;
        const bool received = client.ReceiveText(message, 1000);
        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
        if (!received && client.IsOpen() &&
            std::string(client.Error()) != "receive timed out") {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }

        if (received) {
          PortJson::Value root;
          size_t errorOffset = 0;
          const char* parseReason = nullptr;
          if (!PortJson::Parse(message, root, errorOffset, &parseReason)) {
            connectionError = "invalid server JSON at byte " + std::to_string(errorOffset) +
                              (parseReason != nullptr ? std::string(": ") + parseReason : "");
            connectionFailed = true;
            break;
          }

          std::vector<PortJson::Value> commands;
          if (root.IsArray())
            commands = root.AsArray();
          else
            commands.push_back(std::move(root));

          for (const PortJson::Value& command : commands) {
            const PortJson::Value* cmdValue = command.Find("cmd");
            const std::string cmd = cmdValue != nullptr && cmdValue->IsString()
                                        ? cmdValue->AsString()
                                        : std::string();
            std::vector<std::string> outgoing;
            std::vector<ItemGrant> newGrants;
            std::string connectPacket;
            std::vector<int64_t> initialChecks;
            std::vector<int64_t> sendAllChecks;
            bool connectedNow = false;
            bool refused = false;
            {
              std::lock_guard<std::mutex> lock(runtime.mutex);
              if (runtime.session == nullptr)
                continue;
              const int64_t oldIndex = runtime.session->GetState().nextItemIndex;
              const std::vector<int64_t> oldChecks = runtime.session->GetState().checkedLocations;
              const std::map<int64_t, int64_t> oldProgressive =
                  runtime.session->GetState().progressive;
              const std::string oldSeed = runtime.session->GetState().seed;
              const std::string oldError = runtime.session->LastError();
              const std::string oldResetReason = runtime.session->ResetReason();
              runtime.session->HandlePacket(command, outgoing, newGrants);
              if (runtime.session->ResetReason() != oldResetReason) {
                // The recorded checks belonged to another session, so the state
                // file has just been rewritten empty. Say why, because the
                // symptom otherwise is a multiworld that sends nothing.
                PortLog::Write(
                             "archipelago: saved progress was %s; starting %s fresh (set "
                             "MP_AP_RESET_STATE=1 to discard it deliberately)\n",
                             runtime.session->ResetReason().c_str(), config.slot.c_str());
              }
              const Protocol::State& state = runtime.session->GetState();
              if (state.nextItemIndex != oldIndex || state.checkedLocations != oldChecks ||
                  state.progressive != oldProgressive || state.seed != oldSeed)
                runtime.SaveStateLocked();
              runtime.grants.insert(runtime.grants.end(), newGrants.begin(), newGrants.end());
              runtime.lastMessage = runtime.session->LastMessage();
              const std::string& packetError = runtime.session->LastError();
              if (cmd != "ConnectionRefused" && !packetError.empty() && packetError != oldError &&
                  packetError != runtime.lastError) {
                runtime.lastError = packetError;
                runtime.LogStateLocked(packetError);
              }
              if (cmd == "RoomInfo") {
                connectPacket = runtime.session->BuildConnect();
              } else if (cmd == "Connected" && runtime.session->HandshakeComplete()) {
                connectedNow = true;
                runtime.connected = true;
                runtime.stateLabel = "connected";
                runtime.lastError.clear();
                runtime.LogStateLocked("connected as " + config.slot);
                initialChecks = state.checkedLocations;
                for (auto queued = runtime.queuedChecks.begin(); queued != runtime.queuedChecks.end();) {
                  if (std::find(initialChecks.begin(), initialChecks.end(), *queued) !=
                      initialChecks.end())
                    queued = runtime.queuedChecks.erase(queued);
                  else
                    ++queued;
                }
                if (EnvEnabled("MP_AP_SEND_ALL"))
                  sendAllChecks = runtime.session->AllLocationIds();
              } else if (cmd == "ConnectionRefused") {
                refused = true;
                runtime.connected = false;
                runtime.stateLabel = "error";
                runtime.lastError = runtime.session->LastError();
                if (runtime.lastError.empty())
                  runtime.lastError = "connection refused";
                connectionError = runtime.lastError;
                runtime.LogStateLocked(runtime.lastError);
              } else if (runtime.session->HandshakeComplete() && !apConnected) {
                apConnected = true;
              }
            }

            if (!connectPacket.empty() && !SendPacket(client, connectPacket, connectionError)) {
              connectionFailed = true;
              break;
            }
            if (connectedNow) {
              apConnected = true;
              backoff = 5;
              if (!initialChecks.empty() &&
                  !SendPacket(client, Session::BuildLocationChecks(initialChecks), connectionError)) {
                connectionFailed = true;
                break;
              }
              if (!initialChecks.empty())
                CountChecks(runtime, initialChecks.size());
              if (!sendAllChecks.empty() &&
                  !SendPacket(client, Session::BuildLocationChecks(sendAllChecks), connectionError)) {
                connectionFailed = true;
                break;
              }
              if (!sendAllChecks.empty())
                CountChecks(runtime, sendAllChecks.size());
            }
            for (const std::string& packet : outgoing) {
              if (!SendPacket(client, packet, connectionError)) {
                connectionFailed = true;
                break;
              }
            }
            if (connectionFailed)
              break;
            if (refused) {
              connectionFailed = true;
              break;
            }
          }
          if (connectionFailed)
            break;
        }

        std::vector<int64_t> pendingChecks;
        std::string pendingBounce;
        if (apConnected) {
          std::lock_guard<std::mutex> lock(runtime.mutex);
          while (!runtime.queuedChecks.empty()) {
            pendingChecks.push_back(runtime.queuedChecks.front());
            runtime.queuedChecks.pop_front();
          }
          // DeathLink: a death the game thread noticed, announced once. The
          // pending flag is *cleared* rather than moved-from, because this block
          // runs on every loop iteration and a moved-from-but-not-cleared
          // string is still non-empty, which announced the same death again on
          // every pass round the socket loop.
          if (!runtime.pendingBounce.empty()) {
            runtime.pendingBounce.clear();
            if (runtime.session != nullptr)
              pendingBounce = runtime.session->BuildBounce();
            if (pendingBounce.empty())
              runtime.deathAnnounced = false; // not in DeathLink; do not latch
          }
        }
        if (!pendingChecks.empty()) {
          if (!SendPacket(client, Session::BuildLocationChecks(pendingChecks), connectionError)) {
            connectionFailed = true;
            break;
          }
          CountChecks(runtime, pendingChecks.size());
        }
        if (!pendingBounce.empty()) {
          if (!SendPacket(client, pendingBounce, connectionError)) {
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: announced a death to the multiworld\n");
        }

        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
      }
    }

    client.Close();
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      runtime.connected = false;
    }
    if (runtime.stop.load(std::memory_order_acquire))
      break;
    if (connectionError.empty())
      connectionError = "connection closed";
    if (connectionFailed || !apConnected)
      runtime.SetError(connectionError);
    if (!runtime.WaitBackoff(backoff))
      break;
    backoff = std::min(backoff * 2, 60);
  }
}

void EnsureLoadedImpl(Runtime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.attempted)
    return;
  runtime.attempted = true;
  if (EnvEnabled("MP_AP_DISABLE")) {
    runtime.stateLabel = "off";
    return;
  }

  const std::string configPath = ConfigPath();
  runtime.config = Protocol::LoadConfigFile(configPath);
  if (!runtime.config.valid) {
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: %s\n", runtime.config.error.c_str());
    return;
  }

  std::filesystem::path parent = std::filesystem::path(configPath).parent_path();
  if (parent.empty())
    parent = ".";
  // tls_ca is written relative to the config file, not the working directory.
  if (!runtime.config.tlsCa.empty() && std::filesystem::path(runtime.config.tlsCa).is_relative())
    runtime.config.tlsCa = (parent / runtime.config.tlsCa).string();
  runtime.statePath = (parent / "archipelago_state.json").string();
  Protocol::State state = Protocol::LoadStateFile(runtime.statePath);
  if (state.slot != runtime.config.slot)
    state = Protocol::State();
  // A rewind the client cannot see - a new game or an older save on the same
  // slot and seed - leaves the recorded checks looking valid, so this is the
  // way out: it drops them before the first connect.
  if (EnvEnabled("MP_AP_RESET_STATE") &&
      (state.nextItemIndex != 0 || !state.checkedLocations.empty() || !state.progressive.empty())) {
    PortLog::Write( "archipelago: MP_AP_RESET_STATE=1 discarded saved progress for %s\n",
                 runtime.config.slot.c_str());
    state = Protocol::State();
  }
  state.slot = runtime.config.slot;
  runtime.session = std::make_unique<Session>(runtime.config, state);
  runtime.enabled = true;
  runtime.stateLabel = "connecting";
  runtime.worker = std::thread(Worker, std::ref(runtime));
}

} // namespace

void EnsureLoaded() {
  try {
    EnsureLoadedImpl(GetRuntime());
  } catch (const std::exception& error) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.attempted)
      runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed: %s\n", error.what());
  } catch (...) {
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    runtime.attempted = true;
    runtime.enabled = false;
    runtime.stateLabel = "off";
    PortLog::Write( "archipelago: initialization failed\n");
  }
}

bool Enabled() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.enabled;
}

bool Connected() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.connected;
}

const char* StatusText() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string text;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled) {
      text = "ap: off";
    } else if (runtime.connected) {
      text = "ap: connected, " + std::to_string(runtime.itemCount) + " items, " +
             std::to_string(runtime.checkCount) + " checks";
    } else if (runtime.stateLabel == "error" && !runtime.lastError.empty()) {
      text = "ap: " + runtime.lastError;
    } else {
      text = "ap: connecting";
    }
  }
  constexpr size_t kOverlayLimit = 48;
  if (text.size() > kOverlayLimit)
    text.resize(kOverlayLimit);
  return text.c_str();
}

int ItemCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.itemCount;
}

std::vector< TrackedItem > TrackedItems() {
  EnsureLoaded();
  std::vector< TrackedItem > result;
  Runtime& runtime = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (runtime.session == nullptr)
      return result;
    const std::vector< Protocol::TrackedItem >& tracked = runtime.session->Tracked();
    result.reserve(tracked.size());
    for (const Protocol::TrackedItem& item : tracked) {
      TrackedItem entry;
      entry.name = item.name;
      entry.from = item.from;
      entry.step = static_cast< int >(item.step);
      entry.total = static_cast< int >(item.total);
      result.push_back(std::move(entry));
    }
  }
  return result;
}

int CheckCount() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.checkCount;
}

const char* LastMessage() {
  EnsureLoaded();
  Runtime& runtime = GetRuntime();
  thread_local std::string message;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    message = runtime.lastMessage;
  }
  return message.c_str();
}

bool TakeNotification(std::string& text) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.notifications.empty()) {
      text = std::move(runtime.notifications.front());
      runtime.notifications.pop_front();
      return true;
    }
    return runtime.session != nullptr && runtime.session->TakeNotification(text);
  } catch (...) {
    text.clear();
    return false;
  }
}

const char* SeedName() {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    thread_local std::string seed;
    std::lock_guard<std::mutex> lock(runtime.mutex);
    seed = runtime.session != nullptr ? runtime.session->SeedName() : std::string();
    return seed.c_str();
  } catch (...) {
    return "";
  }
}

void QueueCheck(const char* locationKey) {
  if (locationKey == nullptr || locationKey[0] == '\0')
    return;
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.enabled || runtime.session == nullptr)
      return;
    int64_t id = 0;
    if (!runtime.session->MarkLocationChecked(locationKey, id)) {
      // Two things come back false here and only one of them is a problem. A key
      // the table knows but has already recorded is the ordinary case. A key the
      // table has never heard of means this location will never be reported, and
      // that fails silently: the session plays fine, items arrive, and the server
      // just never records a check. For a multiworld that is the worst shape a
      // failure can take, so name it once instead of dropping it quietly. The
      // usual cause is a location table whose keys are not the world:area:entity
      // form the game produces - see the note in docs/ARCHIPELAGO.md.
      if (!runtime.session->KnowsLocation(locationKey)) {
        if (runtime.unmappedCount == 0) {
          PortLog::Write(
              "archipelago: location '%s' has no id in the location table, so it was not "
              "reported. The session still works and the server will simply never record "
              "this check; if none ever arrives, the world's location keys are probably "
              "not world:area:entity (see docs/ARCHIPELAGO.md).\n",
              locationKey);
        }
        ++runtime.unmappedCount;
      }
      return;
    }
    runtime.SaveStateLocked();
    runtime.queuedChecks.push_back(id);
  } catch (...) {
    // This is called from game pickup handling; AP must never disrupt gameplay.
  }
}

void Poll(CStateManager& mgr) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    CPlayerState* player = mgr.PlayerState();
    if (player == nullptr)
      return;

    static unsigned int notificationTicks = 0;
    std::deque<ItemGrant> grants;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      grants.swap(runtime.grants);
      const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                           runtime.itemCount));
      runtime.itemCount += static_cast<int>(std::min(grants.size(), room));
    }
    for (const ItemGrant& grant : grants) {
      if (grant.itemType < 0)
        continue;
      const auto type = static_cast<CPlayerState::EItemType>(grant.itemType);
      player->InitializePowerUp(type, grant.capacity);
      player->IncrPickUp(type, grant.amount);
      if (type == CPlayerState::kIT_EnergyTanks)
        player->HealthInfo()->SetHP(player->CalculateHealth());
    }

    // DeathLink, inbound. A bounce the server sent is applied by clearing the
    // alive flag, which is what the world's own client does and what drives
    // the whole death sequence here. Bounces that arrive while the game is not
    // running are left pending by the session and picked up on a later tick,
    // so one sent during a load is not lost.
    int deathsOwed = 0;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (runtime.enabled && runtime.session != nullptr) {
        deathsOwed = runtime.session->TakeDeathPending();
        // Outbound: a death is announced once. The flag stays cleared for
        // several seconds of the death animation, so this is latched rather
        // than sent on every tick, and cleared when the player is alive again.
        const bool alive = player->IsAlive();
        if (alive) {
          runtime.deathAnnounced = false;
        } else if (!runtime.deathAnnounced) {
          runtime.deathAnnounced = true;
          runtime.pendingBounce = "death";
        }
      }
    }
    for (int i = 0; i < deathsOwed; ++i)
      player->SetPlayerAlive(false);

    if (mgr.GetGameState() != CStateManager::kGS_Running)
      return;
    if (++notificationTicks < 120)
      return;
    notificationTicks = 0;

    std::string notification;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      std::string next;
      while (runtime.session->TakeNotification(next)) {
        runtime.notifications.push_back(std::move(next));
        next.clear();
      }
      while (runtime.notifications.size() > 8)
        runtime.notifications.pop_front();
      if (!runtime.notifications.empty()) {
        notification = std::move(runtime.notifications.front());
        runtime.notifications.pop_front();
      }
    }
    if (!notification.empty()) {
      CSamusHud::DisplayHudMemo(ToHudWide(notification), CHUDMemoParms(5.f, true, false, false));
    }
  } catch (...) {
    // Avoid leaking exceptions into the simulation loop.
  }
}

} // namespace PortAp
