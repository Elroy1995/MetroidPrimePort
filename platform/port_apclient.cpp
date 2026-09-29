#include "port_apclient.h"
#include "port_log.h"

#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"
#include "port_ws.h"

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
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
#include <exception>
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
  // Called once at exit. A worker blocked in a DNS lookup or a connect cannot
  // be cut short, so it gets a moment to notice `stop` and is otherwise left
  // behind (the Runtime is never freed) rather than holding up the exit.
  void Shutdown() {
    stop.store(true, std::memory_order_release);
    wake.notify_all();
    if (worker.joinable()) {
      bool done;
      {
        std::unique_lock<std::mutex> lock(mutex);
        done = wake.wait_for(lock, std::chrono::seconds(2), [this] { return workerDone; });
      }
      if (done)
        worker.join();
      else
        worker.detach();
    }
    FlushState(); // whatever the worker had not written yet
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop.load(std::memory_order_acquire)) {
      if (!wake.wait_until(lock, deadline, [this] {
            return stop.load(std::memory_order_acquire) || stateDirty;
          }))
        break; // the backoff ran out
      if (stateDirty) {
        lock.unlock();
        FlushState(); // checks collected while offline are still recorded
        lock.lock();
      }
    }
    return !stop.load(std::memory_order_acquire);
  }

  // The state file is written by the socket thread (or at shutdown, once it
  // has stopped), never by the game thread: a pickup must not wait on disk.
  void MarkStateDirtyLocked() {
    stateDirty = true;
    wake.notify_all();
  }

  // Writes the state file if it has changed. Only one thread calls this at a
  // time, so the snapshot taken under the lock is written in order.
  void FlushState() {
    Protocol::State snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!stateDirty || session == nullptr)
        return;
      stateDirty = false;
      snapshot = session->GetState();
    }
    const bool written = Protocol::SaveStateFile(statePath, snapshot);
    if (!written && !stateWriteFailed) // said once, not on every pickup
      PortLog::Write("archipelago: could not write %s\n", statePath.c_str());
    stateWriteFailed = !written;
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stop{false};
  std::thread worker;
  bool workerDone = false; // under mutex; set as the worker returns
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
  // A loaded game rewound the session, so the socket thread owes the server a
  // Sync to get the full inventory replayed.
  bool syncWanted = false;
  // The session state has changed since the state file was last written.
  bool stateDirty = false;
  bool stateWriteFailed = false; // only touched by the thread that flushes
  std::deque<std::string> notifications;
  int itemCount = 0;
  int checkCount = 0;
  // Locations collected in game that the table has no id for. Counted and
  // reported once, because dropping them is otherwise invisible.
  int unmappedCount = 0;
  // DeathLink: whether this client's death has already been announced, so a
  // death is sent once rather than on every tick the flag stays clear for.
  bool deathAnnounced = false;
  // The game reached the end-of-game world: the goal is owed to the server
  // (`goalWanted`) until the socket thread has sent it once (`goalSent`).
  bool goalWanted = false;
  bool goalSent = false;
};

Runtime& GetRuntime() {
  // Leaked on purpose, so a worker Shutdown() leaves behind never touches
  // freed memory; the stopper runs Shutdown() with the other static
  // destructors.
  static Runtime* runtime = new Runtime;
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

// Names the session a save's received items came from: FNV-1a over the seed
// and slot, never 0, which marks a game no session has given items to.
uint32_t SessionIdentity(const std::string& seed, const std::string& slot) {
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const std::string& text) {
    for (const char c : text) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
  };
  mix(seed);
  mix(std::string(1, '\0'));
  mix(slot);
  return hash != 0 ? hash : 1;
}

// Lines the session up with the loaded game. The state file's item index says
// what this client has received, but not what the game holds: quitting without
// saving, or loading an older save, drops items the index has moved past. So
// the save records how many it holds, and on load the session rewinds to that
// many and has the server replay the rest.
void ReconcileLocked(Runtime& runtime, CGameState::ApProgress& progress) {
  const Protocol::State& state = runtime.session->GetState();
  if (state.seed.empty())
    return; // no session yet to compare with; nothing can have been granted
  const uint32_t identity = SessionIdentity(state.seed, runtime.config.slot);
  if (progress.reconciled) {
    // The server changed seeds under a running game. The session has already
    // dropped the old seed's progress, and none of the new one's is held yet.
    if (progress.identity != identity) {
      progress.identity = identity;
      progress.appliedIndex = 0;
    }
    return;
  }
  progress.reconciled = true;
  // Where the game's items end on the session's side: grants still queued
  // for the game are about to be applied, so they count as not held yet.
  const int64_t firstPending =
      runtime.grants.empty() ? state.nextItemIndex : runtime.grants.front().index;
  if (!progress.recorded) {
    // A save from before the record existed: take it to hold what the state
    // file says was received, which is what the client assumed until now.
    progress.recorded = true;
    progress.identity = identity;
    progress.appliedIndex = static_cast<uint>(
        std::clamp<int64_t>(firstPending, 0, std::numeric_limits<uint32_t>::max()));
    return;
  }
  // A new game, or a save from another seed or slot, holds none of these items.
  const int64_t held = progress.identity == identity ? progress.appliedIndex : 0;
  progress.identity = identity;
  progress.appliedIndex = static_cast<uint>(held);
  if (held == firstPending)
    return;
  PortLog::Write("archipelago: the loaded game holds %lld of %lld received items; "
                 "asking the server for the rest\n",
                 static_cast<long long>(held), static_cast<long long>(state.nextItemIndex));
  runtime.session->RewindTo(held);
  runtime.MarkStateDirtyLocked();
  runtime.grants.clear();
  runtime.itemCount = static_cast<int>(std::min<int64_t>(held, std::numeric_limits<int>::max()));
  runtime.syncWanted = true;
}

// With the built-in tables the game is a plain disc, so what the AP ISO patches
// in is done here each tick instead: unlimited ammo, the Artifact Temple totems
// following the artifacts held (the retail pickup scripts light the totem of the
// artifact that used to be at a location, not the one received), and the goal.
void ApplyBuiltinWorld(Runtime& runtime, CStateManager& mgr, CPlayerState& player) {
  namespace Prime = MetroidPrime;
  bool unlimitedMissiles = false;
  bool unlimitedPowerBombs = false;
  {
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.config.builtin || runtime.session == nullptr)
      return;
    unlimitedMissiles =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedMissiles) > 0;
    unlimitedPowerBombs =
        runtime.session->ReceivedCount(Prime::kItemBase + Prime::kUnlimitedPowerBombs) > 0;
    if (!runtime.goalWanted && gpGameState->CurrentWorldAssetId() == Prime::kEndOfGameWorld) {
      runtime.goalWanted = true;
      runtime.wake.notify_all();
    }
  }
  const auto topOff = [&player](CPlayerState::EItemType type) {
    const int missing = player.GetItemCapacity(type) - player.GetItemAmount(type);
    if (missing > 0)
      player.IncrPickUp(type, missing);
  };
  if (unlimitedMissiles)
    topOff(CPlayerState::kIT_Missiles);
  if (unlimitedPowerBombs)
    topOff(CPlayerState::kIT_PowerBombs);

  // Tallon's layer state is shared with the running world while in Tallon, so
  // writing it through the game state covers both cases.
  CScriptLayerManager* layers =
      gpGameState->StateForWorld(Prime::kTallonWorld).GetLayerState().GetPtr();
  TAreaId temple = Prime::kArtifactTempleIndex;
  const CWorld* world = mgr.GetWorld();
  if (world != nullptr && world->IGetWorldAssetId() == Prime::kTallonWorld)
    temple = world->IGetAreaId(Prime::kArtifactTempleArea);
  if (layers == nullptr || temple.Value() < 0 ||
      static_cast<size_t>(temple.Value()) >= layers->GetAreaLayers().size())
    return;
  for (int id = CPlayerState::kIT_Truth; id <= CPlayerState::kIT_Newborn; ++id) {
    const bool held = player.GetItemAmount(static_cast<CPlayerState::EItemType>(id)) > 0;
    // Truth's totem is the first thing in the room, so it has its own layer.
    const TLayerId layer(id == CPlayerState::kIT_Truth ? 23 : id - 28);
    if (layers->IsLayerActive(temple, layer) != held)
      layers->SetLayerActive(temple, layer, held);
  }
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

void WorkerLoop(Runtime& runtime) {
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
    client.SetCancelFlag(&runtime.stop); // quitting must not wait out a timeout
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
      if (!runtime.stop.load(std::memory_order_acquire)) // not a failure when cut short
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
                runtime.MarkStateDirtyLocked();
              runtime.grants.insert(runtime.grants.end(), newGrants.begin(), newGrants.end());
              runtime.lastMessage = runtime.session->LastMessage();
              const std::string& packetError = runtime.session->LastError();
              if (cmd != "ConnectionRefused" && !packetError.empty() && packetError != oldError &&
                  packetError != runtime.lastError) {
                runtime.lastError = packetError;
                runtime.LogStateLocked(packetError);
              }
              if (cmd == "Connected")
                runtime.syncWanted = false; // the handshake replays everything anyway
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
                for (const std::string& warning : runtime.session->GetSlotData().warnings)
                  PortLog::Write("archipelago: seed option %s; the game will not match the "
                                 "seed's logic\n",
                                 warning.c_str());
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
        bool syncWanted = false;
        bool goalWanted = false;
        if (apConnected) {
          std::lock_guard<std::mutex> lock(runtime.mutex);
          goalWanted = runtime.goalWanted && !runtime.goalSent;
          runtime.goalSent = runtime.goalSent || goalWanted;
          while (!runtime.queuedChecks.empty()) {
            pendingChecks.push_back(runtime.queuedChecks.front());
            runtime.queuedChecks.pop_front();
          }
          syncWanted = runtime.syncWanted;
          runtime.syncWanted = false;
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
        if (syncWanted && !SendPacket(client, Session::BuildSync(), connectionError)) {
          connectionFailed = true;
          break;
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
        if (goalWanted) {
          if (!SendPacket(client, Session::BuildGoal(), connectionError)) {
            std::lock_guard<std::mutex> lock(runtime.mutex);
            runtime.goalSent = false; // owed again on the next connection
            connectionFailed = true;
            break;
          }
          PortLog::Write("archipelago: goal complete\n");
        }
        runtime.FlushState();

        if (!received && !client.IsOpen()) {
          connectionError = ErrorText(client.Error());
          connectionFailed = true;
          break;
        }
      }
    }

    client.Close();
    runtime.FlushState();
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

// An exception escaping a std::thread terminates the game, and a broken or
// hostile server can cause one (bad_alloc on a huge message, a packet the
// session does not expect). It costs the connection instead, which retries.
void Worker(Runtime& runtime) {
  while (!runtime.stop.load(std::memory_order_acquire)) {
    try {
      WorkerLoop(runtime);
      break;
    } catch (const std::exception& error) {
      PortLog::Write("archipelago: client error: %s\n", error.what());
      runtime.SetError(std::string("internal error: ") + error.what());
    } catch (...) {
      PortLog::Write("archipelago: client error: unknown exception\n");
      runtime.SetError("internal error");
    }
    if (!runtime.WaitBackoff(30))
      break;
  }
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.workerDone = true;
  runtime.wake.notify_all();
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
    runtime.MarkStateDirtyLocked();
    runtime.queuedChecks.push_back(id);
  } catch (...) {
    // This is called from game pickup handling; AP must never disrupt gameplay.
  }
}

void OnInventoryReset() {
  try {
    if (!Enabled() || gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();
    progress.appliedIndex = 0;
    progress.reconciled = false; // reconcile again, now holding nothing
  } catch (...) {
  }
}

void Poll(CStateManager& mgr) {
  try {
    EnsureLoaded();
    Runtime& runtime = GetRuntime();
    CPlayerState* player = mgr.PlayerState();
    if (player == nullptr || gpGameState == nullptr)
      return;
    CGameState::ApProgress& progress = gpGameState->PortApProgress();

    static unsigned int notificationTicks = 0;
    std::deque<ItemGrant> grants;
    {
      std::lock_guard<std::mutex> lock(runtime.mutex);
      if (!runtime.enabled || runtime.session == nullptr)
        return;
      ReconcileLocked(runtime, progress);
      grants.swap(runtime.grants);
      const size_t room = static_cast<size_t>(std::max(0, std::numeric_limits<int>::max() -
                                                           runtime.itemCount));
      runtime.itemCount += static_cast<int>(std::min(grants.size(), room));
    }
    for (const ItemGrant& grant : grants) {
      if (grant.index >= 0 && grant.index < std::numeric_limits<uint32_t>::max())
        progress.appliedIndex =
            std::max(progress.appliedIndex, static_cast<uint>(grant.index + 1));
      if (grant.itemType < 0)
        continue;
      const auto type = static_cast<CPlayerState::EItemType>(grant.itemType);
      player->InitializePowerUp(type, grant.capacity);
      player->IncrPickUp(type, grant.amount);
      if (type == CPlayerState::kIT_EnergyTanks)
        player->HealthInfo()->SetHP(player->CalculateHealth());
    }
    ApplyBuiltinWorld(runtime, mgr, *player);

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
        } else if (!runtime.deathAnnounced && runtime.connected) {
          // Only while connected: a death during an outage would otherwise
          // be announced, stale, on the next connect.
          runtime.deathAnnounced = true;
          runtime.pendingBounce = "death";
        }
        // A death that came in over DeathLink is not announced back: every
        // other client would echo it too, and the deaths would go round the
        // multiworld forever.
        if (deathsOwed > 0)
          runtime.deathAnnounced = true;
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
