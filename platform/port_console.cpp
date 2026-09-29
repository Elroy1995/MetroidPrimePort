// Debug command console for smoke builds (MP_ENABLE_SMOKE_DRIVER): with
// MP_CONSOLE=<port> (1 = 4777) the game listens on 127.0.0.1 and runs one
// command per line, e.g. `warp 83F6FF6F 492CBF4A`, `objs EyeBall`, `shot`.
// Every reply ends with a line `=> ok` or `=> err: <why>`. tools/mpcon.py is
// the client. Commands that touch the game run inside the state manager's
// tick, where every object pointer is live; the rest run once per frame.
#include "port_debug.h"
#include "port_smoke.h"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/BodyState/CBodyController.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include <dolphin/pad.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#if defined(__GNUC__)
#include <cxxabi.h>
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aurora {
void request_screenshot() noexcept;
}

namespace {

// Frames a game command waits for a state manager tick before giving up.
constexpr unsigned kTickTimeout = 180;
constexpr unsigned kWarpTimeout = 6000;

struct Incoming {
  std::string line;
  unsigned generation;
};

std::mutex sQueueMutex;
std::deque< Incoming > sQueue;
std::mutex sClientMutex;
int sClient = -1;
unsigned sGeneration = 0;
bool sStarted = false;
bool sEnabled = false;

void SendRaw(unsigned generation, const std::string& text) {
#ifndef _WIN32
  std::lock_guard< std::mutex > lock(sClientMutex);
  if (sClient < 0 || generation != sGeneration) {
    return;
  }
  size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = send(sClient, text.data() + done, text.size() - done, MSG_NOSIGNAL);
    if (n <= 0) {
      return;
    }
    done += static_cast< size_t >(n);
  }
#else
  (void)generation;
  (void)text;
#endif
}

#ifndef _WIN32
void ListenThread(int listener) {
  std::string buffer;
  for (;;) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    unsigned generation;
    {
      std::lock_guard< std::mutex > lock(sClientMutex);
      if (sClient >= 0) {
        close(sClient);
      }
      sClient = client;
      generation = ++sGeneration;
    }
    std::fprintf(stderr, "[console] client connected\n");
    buffer.clear();
    char chunk[512];
    for (;;) {
      const ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;
      }
      buffer.append(chunk, static_cast< size_t >(n));
      size_t eol;
      while ((eol = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, eol);
        buffer.erase(0, eol + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        std::lock_guard< std::mutex > lock(sQueueMutex);
        sQueue.push_back({line, generation});
      }
    }
    std::lock_guard< std::mutex > lock(sClientMutex);
    if (sClient == client) {
      close(sClient);
      sClient = -1;
    }
    std::fprintf(stderr, "[console] client disconnected\n");
  }
}
#endif

void Start() {
  sStarted = true;
  const char* value = std::getenv("MP_CONSOLE");
  if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
    return;
  }
#ifdef _WIN32
  std::fputs("[console] MP_CONSOLE is not supported on Windows\n", stderr);
#else
  int port = std::atoi(value);
  if (port <= 1) {
    port = 4777;
  }
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  const int yes = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast< uint16_t >(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (listener < 0 || bind(listener, reinterpret_cast< sockaddr* >(&addr), sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    std::fprintf(stderr, "[console] cannot listen on 127.0.0.1:%d: %s\n", port, std::strerror(errno));
    if (listener >= 0) {
      close(listener);
    }
    return;
  }
  std::fprintf(stderr, "[console] listening on 127.0.0.1:%d\n", port);
  sEnabled = true;
  std::thread(ListenThread, listener).detach();
#endif
}

// ---------------------------------------------------------------------------
// Command state

enum class Where { Frame, Tick };

struct Command {
  std::vector< std::string > args;
  unsigned generation = 0;
  unsigned startFrame = 0;
  std::string out;
  // Continuations: a command that spans frames sets these.
  int phase = 0;
  unsigned untilFrame = 0;
  unsigned ticks = 0;
  uint32_t warpWorld = 0;
  uint32_t warpArea = 0;
  PADStatus pad{};
  std::string shotDir;
  size_t shotCount = 0;
};

bool sHasCommand = false;
Command sCmd;
unsigned sFrame = 0;
bool sQuit = false;

void Out(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sCmd.out += buf;
  sCmd.out += '\n';
}

void Finish(const char* error = nullptr) {
  std::string text = sCmd.out;
  if (error == nullptr) {
    text += "=> ok\n";
  } else {
    text += "=> err: ";
    text += error;
    text += '\n';
  }
  SendRaw(sCmd.generation, text);
  sHasCommand = false;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  }
  return s;
}

bool ParseHex(const std::string& s, uint32_t& value) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (*end != '\0') {
    return false;
  }
  value = static_cast< uint32_t >(v);
  return true;
}

bool ParseFloat(const std::string& s, float& value) {
  char* end = nullptr;
  value = std::strtof(s.c_str(), &end);
  return !s.empty() && *end == '\0';
}

bool ParseUnsigned(const std::string& s, unsigned& value) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0') {
    return false;
  }
  value = static_cast< unsigned >(v);
  return true;
}

std::string ClassName(const CEntity& ent) {
  const char* raw = typeid(ent).name();
#if defined(__GNUC__)
  int status = 0;
  char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string name = demangled;
    std::free(demangled);
    return name;
  }
#endif
  return raw;
}

const char* const kMessageNames[] = {
    "UNKM0", "Activate", "Arrived", "Close", "Deactivate", "Decrement", "Follow",
    "Increment", "Next", "Open", "Reset", "ResetAndStart", "SetToMax", "SetToZero",
    "Start", "Stop", "StopAndReset", "ToggleActive", "UNKM18", "Action", "Play",
    "Alert", "InternalMessage00", "OnFloor", "InternalMessage02", "InternalMessage03",
    "Falling", "OnIceSurface", "OnMudSlowSurface", "OnNormalSurface", "Touched",
    "AddPlatformRider", "LandOnNotFloor", "Registered", "Deleted", "InitializedInArea",
    "WorldInitialized", "AddSplashInhabitant", "UpdateSplashInhabitant",
    "RemoveSplashInhabitant", "Jumped", "Damage", "InvulnDamage", "ProjectileCollide",
    "InSnakeWeed", "AddPhazonPoolInhabitant", "UpdatePhazonPoolInhabitant",
    "RemovePhazonPoolInhabitant", "SuspendedMove",
};
constexpr int kMessageCount = sizeof(kMessageNames) / sizeof(kMessageNames[0]);

const char* const kStateNames[] = {
    "Active", "Arrived", "Closed", "Entered", "Exited", "Inactive", "Inside",
    "MaxReached", "Open", "Zero", "Attack", "CloseIn", "Retreat", "Patrol", "Dead",
    "CameraPath", "CameraTarget", "DeactivateState", "Play", "MassiveDeath",
    "DeathRattle", "AboutToMassivelyDie", "Damage", "InvulnDamage", "MassiveFrozenDeath",
    "Modify", "ScanStart", "ScanProcessing", "ScanDone", "UnFrozen", "Default",
    "ReflectedDamage", "InheritBounds",
};
constexpr int kStateCount = sizeof(kStateNames) / sizeof(kStateNames[0]);

const char* const kItemNames[] = {
    "PowerBeam", "IceBeam", "WaveBeam", "PlasmaBeam", "Missiles", "ScanVisor",
    "MorphBallBombs", "PowerBombs", "Flamethrower", "ThermalVisor", "ChargeBeam",
    "SuperMissile", "GrappleBeam", "XRayVisor", "IceSpreader", "SpaceJumpBoots",
    "MorphBall", "CombatVisor", "BoostBall", "SpiderBall", "PowerSuit", "GravitySuit",
    "VariaSuit", "PhazonSuit", "EnergyTanks", "UnknownItem1", "HealthRefill",
    "UnknownItem2", "Wavebuster", "Truth", "Strength", "Elder", "Wild", "Lifegiver",
    "Warrior", "Chozo", "Nature", "Sun", "World", "Spirit", "Newborn",
};
constexpr int kItemCount = sizeof(kItemNames) / sizeof(kItemNames[0]);

int LookupName(const std::string& arg, const char* const* names, int count) {
  unsigned number;
  if (ParseUnsigned(arg, number)) {
    return number < static_cast< unsigned >(count) ? static_cast< int >(number) : -1;
  }
  const std::string want = Lower(arg);
  for (int i = 0; i < count; ++i) {
    if (Lower(names[i]) == want) {
      return i;
    }
  }
  return -1;
}

const char* NameOr(const char* const* names, int count, int index) {
  return index >= 0 && index < count ? names[index] : "?";
}

std::string WorldName(int index) {
  std::string name;
  const wchar_t* wide = gpMemoryCard->GetMemoryWorlds()[index].second.GetFrontEndName();
  for (const wchar_t* p = wide; p != nullptr && *p != 0; ++p) {
    name.push_back(static_cast< char >(*p));
  }
  return name;
}

// An object by editor id (hex, as in the level data), by unique id (`u` +
// decimal index), or by exact debug name.
CEntity* FindObject(CStateManager& mgr, const std::string& arg) {
  const CObjectList& list = mgr.GetObjectListById(kOL_All);
  const bool byUid = arg.size() > 1 && (arg[0] == 'u' || arg[0] == 'U');
  unsigned uid = 0;
  if (byUid && !ParseUnsigned(arg.substr(1), uid)) {
    return nullptr;
  }
  uint32_t eid = 0;
  const bool byEid = !byUid && ParseHex(arg, eid);
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
    CEntity* ent = const_cast< CEntity* >(list[i]);
    if (ent == nullptr) {
      continue;
    }
    if (byUid ? ent->GetUniqueId().Value() == uid
              : byEid ? ent->GetEditorId().Value() == (eid & 0x3FFFFFF)
                      : std::strcmp(ent->GetDebugName().data(), arg.c_str()) == 0) {
      return ent;
    }
  }
  return nullptr;
}

std::string Describe(CStateManager& mgr, CEntity& ent) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "u%-4u %08X %-9s %-26s %-28s", ent.GetUniqueId().Value(),
                ent.GetEditorId().Value(), ent.GetActive() ? "active" : "inactive",
                ClassName(ent).c_str(), ent.GetDebugName().data());
  std::string line = buf;
  if (const CActor* actor = TCastToConstPtr< CActor >(&ent)) {
    const CVector3f pos = actor->GetTranslation();
    const float dist = (pos - mgr.GetPlayer()->GetTranslation()).Magnitude();
    std::snprintf(buf, sizeof(buf), " pos=(%.1f, %.1f, %.1f) dist=%.1f", pos.GetX(), pos.GetY(),
                  pos.GetZ(), dist);
    line += buf;
  }
  return line;
}

// ---------------------------------------------------------------------------
// Commands

void CmdHelp() {
  Out("status                     world, area, player position, health, visor, beam");
  Out("worlds                     world ids and names");
  Out("areas                      areas of the current world (index, MREA)");
  Out("warp <world> [mrea]        load a world (hex MLVL id or name prefix), optionally an area");
  Out("tp <x> <y> <z>             move the player");
  Out("face <yaw deg> | look <id> turn the player (yaw 0 = +y, 90 = -x)");
  Out("objs [filter]              objects whose class or name contains filter");
  Out("obj <id>                   one object: state, health, animation, connections");
  Out("send <id> <msg>            deliver a script message (name or number; relays fire on SetToZero)");
  Out("give <item> [n]            add an item (name or number, see `items`)");
  Out("items                      the player's inventory");
  Out("heal                       refill health");
  Out("press <a+b+...> [frames]   hold pad buttons (a b x y z l r start up down left right)");
  Out("stick <x> <y> [frames]     hold the main stick (-127..127); cstick for the C stick");
  Out("shot                       take a screenshot and print its path");
  Out("wait <frames>              let frames pass");
  Out("aspect <4:3|16:9|window>   switch the rendering aspect, as the Options row does");
  Out("quit                       exit the game");
  Out("ids: hex editor id (002900A1), u<index> unique id, or an exact debug name");
}

void CmdWorlds() {
  const auto& worlds = gpMemoryCard->GetMemoryWorlds();
  const uint32_t current = gpGameState != nullptr ? gpGameState->CurrentWorldAssetId() : 0;
  for (int i = 0; i < worlds.size(); ++i) {
    const uint32_t id = static_cast< uint32_t >(worlds[i].first);
    Out("%08X %s%s", id, WorldName(i).c_str(), id == current ? "  <- current" : "");
  }
}

void CmdStatus(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  const CPlayer& player = *mgr.GetPlayer();
  CPlayerState& ps = *mgr.GetPlayerState();
  Out("frame %u, game state %d", sFrame, static_cast< int >(mgr.GetGameState()));
  if (world != nullptr) {
    const TAreaId area = world->GetCurrentAreaId();
    Out("world %08X area %d (MREA %08X)", static_cast< uint32_t >(world->IGetWorldAssetId()),
        area.Value(),
        static_cast< uint32_t >(world->GetAreaAlways(area).GetAreaAssetId()));
  }
  const CVector3f pos = player.GetTranslation();
  const CVector3f vel = player.GetVelocityWR();
  const CVector3f fwd = player.GetTransform().GetForward();
  const float yaw = std::atan2(-fwd.GetX(), fwd.GetY()) * 57.29578f;
  Out("player u%u pos=(%.2f, %.2f, %.2f) vel=(%.2f, %.2f, %.2f) yaw=%.1f",
      player.GetUniqueId().Value(), pos.GetX(), pos.GetY(), pos.GetZ(), vel.GetX(), vel.GetY(),
      vel.GetZ(), yaw);
  static const char* const morph[] = {"unmorphed", "morphed", "morphing", "unmorphing"};
  static const char* const visors[] = {"combat", "xray", "scan", "thermal"};
  static const char* const beams[] = {"power", "ice", "wave", "plasma", "phazon"};
  const int m = static_cast< int >(player.GetMorphballTransitionState());
  const int v = static_cast< int >(ps.GetCurrentVisor());
  const int b = static_cast< int >(ps.GetCurrentBeam());
  Out("hp %.0f/%.0f, %s, visor %s, beam %s, missiles %d/%d", ps.GetHealthInfo().GetHP(),
      ps.CalculateHealth(), m >= 0 && m < 4 ? morph[m] : "?", v >= 0 && v < 4 ? visors[v] : "?",
      b >= 0 && b < 5 ? beams[b] : "?", ps.GetItemAmount(CPlayerState::kIT_Missiles),
      ps.GetItemCapacity(CPlayerState::kIT_Missiles));
  const CGameCamera& cam = mgr.GetCameraManager()->GetCurrentCamera(mgr);
  Out("camera u%u fov %.1f aspect %.3f (viewport %.3f)", cam.GetUniqueId().Value(), cam.GetFov(),
      cam.GetAspectRatio(), CCameraManager::GetDefaultAspectRatio());
  if (const CEntity* target = mgr.GetObjectById(player.GetOrbitTargetId())) {
    Out("orbit target u%u %08X %s", target->GetUniqueId().Value(), target->GetEditorId().Value(),
        target->GetDebugName().data());
  }
}

void CmdAreas(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr) {
    return Finish("no world");
  }
  const int current = world->GetCurrentAreaId().Value();
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    const CGameArea& area = world->GetAreaAlways(TAreaId(i));
    Out("%3d %08X%s%s", i, static_cast< uint32_t >(area.GetAreaAssetId()),
        area.IsPostConstructed() ? " loaded" : "", i == current ? "  <- current" : "");
  }
  Finish();
}

void CmdObjs(CStateManager& mgr) {
  const std::string filter = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : std::string();
  const CObjectList& list = mgr.GetObjectListById(kOL_All);
  int count = 0;
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
    CEntity* ent = const_cast< CEntity* >(list[i]);
    if (ent == nullptr) {
      continue;
    }
    if (!filter.empty() && Lower(ClassName(*ent)).find(filter) == std::string::npos &&
        Lower(ent->GetDebugName().data()).find(filter) == std::string::npos) {
      continue;
    }
    Out("%s", Describe(mgr, *ent).c_str());
    ++count;
  }
  Out("%d objects", count);
  Finish();
}

void CmdObj(CStateManager& mgr) {
  if (sCmd.args.size() < 2) {
    return Finish("usage: obj <id>");
  }
  CEntity* ent = FindObject(mgr, sCmd.args[1]);
  if (ent == nullptr) {
    return Finish("no such object");
  }
  Out("%s", Describe(mgr, *ent).c_str());
  Out("area %d", ent->GetAreaId().Value());
  if (CActor* actor = TCastToPtr< CActor >(ent)) {
    const CVector3f fwd = actor->GetTransform().GetForward();
    Out("forward (%.3f, %.3f, %.3f)", fwd.GetX(), fwd.GetY(), fwd.GetZ());
    if (actor->HasModelData()) {
      Out("%s", actor->GetPreRenderClipped() ? "outside the view frustum (not animated or drawn)"
                                             : "inside the view frustum");
    }
    if (const CHealthInfo* health = actor->GetHealthInfo(mgr)) {
      Out("hp %.2f", health->GetHP());
    }
  }
  if (const CPhysicsActor* physics = TCastToConstPtr< CPhysicsActor >(ent)) {
    const CVector3f vel = physics->GetVelocityWR();
    Out("velocity (%.2f, %.2f, %.2f)", vel.GetX(), vel.GetY(), vel.GetZ());
  }
  if (CPatterned* patterned = TCastToPtr< CPatterned >(ent)) {
    const CAiState* state = patterned->GetStateMachineState().GetActorState();
    Out("ai state %s for %.2fs", state != nullptr ? state->GetName() : "(none)",
        patterned->GetStateMachineTime());
    if (const CBodyController* body = patterned->GetBodyCtrl()) {
      Out("body state %d, anim %d", static_cast< int >(body->GetCurrentStateId()),
          body->GetCurrentAnimId());
    }
  }
  const rstl::vector< SConnection >& conns = ent->GetConnectionList();
  for (int i = 0; i < conns.size(); ++i) {
    const SConnection& c = conns[i];
    const CEntity* target = mgr.GetObjectById(mgr.GetIdForScript(c.x8_objId));
    Out("on %s send %s to %08X %s", NameOr(kStateNames, kStateCount, c.x0_state),
        NameOr(kMessageNames, kMessageCount, c.x4_msg), c.x8_objId.Value(),
        target != nullptr ? target->GetDebugName().data() : "(not loaded)");
  }
  Finish();
}

void CmdSend(CStateManager& mgr) {
  if (sCmd.args.size() < 3) {
    return Finish("usage: send <id> <msg>");
  }
  CEntity* ent = FindObject(mgr, sCmd.args[1]);
  if (ent == nullptr) {
    return Finish("no such object");
  }
  const int msg = LookupName(sCmd.args[2], kMessageNames, kMessageCount);
  if (msg < 0) {
    return Finish("unknown message");
  }
  mgr.SendScriptMsgAlways(ent->GetUniqueId(), kInvalidUniqueId,
                          static_cast< EScriptObjectMessage >(msg));
  Out("sent %s to u%u %08X", kMessageNames[msg], ent->GetUniqueId().Value(),
      ent->GetEditorId().Value());
  Finish();
}

void CmdGive(CStateManager& mgr) {
  if (sCmd.args.size() < 2) {
    return Finish("usage: give <item> [n]");
  }
  const int item = LookupName(sCmd.args[1], kItemNames, kItemCount);
  unsigned amount = 1;
  if (item < 0 || (sCmd.args.size() > 2 && !ParseUnsigned(sCmd.args[2], amount))) {
    return Finish("unknown item or amount");
  }
  CPlayerState& ps = *mgr.PlayerState();
  const CPlayerState::EItemType type = static_cast< CPlayerState::EItemType >(item);
  ps.InitializePowerUp(type, static_cast< int >(amount));
  ps.IncrPickUp(type, static_cast< int >(amount));
  if (type == CPlayerState::kIT_EnergyTanks) {
    ps.HealthInfo()->SetHP(ps.CalculateHealth());
  }
  Out("%s: %d/%d", kItemNames[item], ps.GetItemAmount(type), ps.GetItemCapacity(type));
  Finish();
}

void CmdItems(CStateManager& mgr) {
  CPlayerState& ps = *mgr.PlayerState();
  for (int i = 0; i < kItemCount; ++i) {
    const CPlayerState::EItemType type = static_cast< CPlayerState::EItemType >(i);
    if (ps.GetItemCapacity(type) > 0) {
      Out("%2d %-16s %d/%d", i, kItemNames[i], ps.GetItemAmount(type), ps.GetItemCapacity(type));
    }
  }
  Finish();
}

void TeleportPlayer(CStateManager& mgr, const CVector3f& pos, const CVector3f& look) {
  CPlayer& player = *mgr.Player();
  CVector3f dir = look;
  dir.SetZ(0.f);
  if (!dir.CanBeNormalized()) {
    dir = player.GetTransform().GetForward();
    dir.SetZ(0.f);
  }
  player.Teleport(CTransform4f::LookAt(pos, pos + dir, CVector3f::Up()), mgr, true);
  player.SetVelocityWR(CVector3f(0.f, 0.f, 0.f));
}

void CmdTp(CStateManager& mgr) {
  float x, y, z;
  if (sCmd.args.size() < 4 || !ParseFloat(sCmd.args[1], x) || !ParseFloat(sCmd.args[2], y) ||
      !ParseFloat(sCmd.args[3], z)) {
    return Finish("usage: tp <x> <y> <z>");
  }
  TeleportPlayer(mgr, CVector3f(x, y, z), mgr.GetPlayer()->GetTransform().GetForward());
  Finish();
}

void CmdFace(CStateManager& mgr) {
  float yaw;
  if (sCmd.args.size() < 2 || !ParseFloat(sCmd.args[1], yaw)) {
    return Finish("usage: face <yaw degrees>");
  }
  const float rad = yaw / 57.29578f;
  TeleportPlayer(mgr, mgr.GetPlayer()->GetTranslation(),
                 CVector3f(-std::sin(rad), std::cos(rad), 0.f));
  Finish();
}

void CmdLook(CStateManager& mgr) {
  CEntity* ent = sCmd.args.size() > 1 ? FindObject(mgr, sCmd.args[1]) : nullptr;
  const CActor* actor = TCastToConstPtr< CActor >(ent);
  if (actor == nullptr) {
    return Finish("no such actor");
  }
  const CVector3f from = mgr.GetPlayer()->GetTranslation();
  TeleportPlayer(mgr, from, actor->GetTranslation() - from);
  Finish();
}

void CmdHeal(CStateManager& mgr) {
  CPlayerState& ps = *mgr.PlayerState();
  ps.HealthInfo()->SetHP(ps.CalculateHealth());
  Finish();
}

bool ResolveWorld(const std::string& arg, uint32_t& id) {
  const auto& worlds = gpMemoryCard->GetMemoryWorlds();
  const std::string want = Lower(arg);
  for (int i = 0; i < worlds.size(); ++i) {
    if (Lower(WorldName(i)).rfind(want, 0) == 0) {
      id = static_cast< uint32_t >(worlds[i].first);
      return true;
    }
  }
  return ParseHex(arg, id);
}

void CmdWarp(CStateManager& mgr) {
  if (sCmd.phase == 0) {
    if (sCmd.args.size() < 2 || !ResolveWorld(sCmd.args[1], sCmd.warpWorld) ||
        (sCmd.args.size() > 2 && !ParseHex(sCmd.args[2], sCmd.warpArea))) {
      return Finish("usage: warp <world id or name> [mrea]");
    }
    PortDebug::RequestWorldTeleport(sCmd.warpWorld, sCmd.warpArea);
    sCmd.phase = 1;
    sCmd.untilFrame = sFrame + kWarpTimeout;
    return;
  }
  // The request is consumed later in the tick that made it and quits that
  // state manager, so any later tick from a manager that is not quitting
  // belongs to the new world; it is done once that one has run a while.
  const CWorld* world = mgr.GetWorld();
  if (sCmd.phase == 1 && world != nullptr && !mgr.GetWantsToQuit() &&
      static_cast< uint32_t >(world->IGetWorldAssetId()) == sCmd.warpWorld) {
    sCmd.phase = 2;
  }
  if (sCmd.phase == 2 && mgr.GetGameState() == CStateManager::kGS_Running && ++sCmd.ticks >= 30) {
    Out("in world %08X area %d (MREA %08X)", sCmd.warpWorld, world->GetCurrentAreaId().Value(),
        static_cast< uint32_t >(world->GetAreaAlways(world->GetCurrentAreaId()).GetAreaAssetId()));
    Finish();
  }
}

bool IsTickCommand(const std::string& name) {
  static const char* const names[] = {"status", "areas", "objs", "obj", "send", "give",
                                      "items", "heal", "tp", "face", "look", "warp"};
  for (const char* n : names) {
    if (name == n) {
      return true;
    }
  }
  return false;
}

void RunTick(CStateManager& mgr) {
  const std::string& name = sCmd.args[0];
  if (name == "status") {
    CmdStatus(mgr);
    Finish();
  } else if (name == "areas") {
    CmdAreas(mgr);
  } else if (name == "objs") {
    CmdObjs(mgr);
  } else if (name == "obj") {
    CmdObj(mgr);
  } else if (name == "send") {
    CmdSend(mgr);
  } else if (name == "give") {
    CmdGive(mgr);
  } else if (name == "items") {
    CmdItems(mgr);
  } else if (name == "heal") {
    CmdHeal(mgr);
  } else if (name == "tp") {
    CmdTp(mgr);
  } else if (name == "face") {
    CmdFace(mgr);
  } else if (name == "look") {
    CmdLook(mgr);
  } else if (name == "warp") {
    CmdWarp(mgr);
  }
}

bool ParseButtons(const std::string& spec, u16& buttons, u8& left, u8& right) {
  size_t start = 0;
  while (start <= spec.size()) {
    size_t end = spec.find_first_of("+,", start);
    if (end == std::string::npos) {
      end = spec.size();
    }
    const std::string b = Lower(spec.substr(start, end - start));
    if (b == "a") buttons |= PAD_BUTTON_A;
    else if (b == "b") buttons |= PAD_BUTTON_B;
    else if (b == "x") buttons |= PAD_BUTTON_X;
    else if (b == "y") buttons |= PAD_BUTTON_Y;
    else if (b == "z") buttons |= PAD_TRIGGER_Z;
    else if (b == "l") { buttons |= PAD_TRIGGER_L; left = 255; }
    else if (b == "r") { buttons |= PAD_TRIGGER_R; right = 255; }
    else if (b == "start") buttons |= PAD_BUTTON_START;
    else if (b == "up") buttons |= PAD_BUTTON_UP;
    else if (b == "down") buttons |= PAD_BUTTON_DOWN;
    else if (b == "left") buttons |= PAD_BUTTON_LEFT;
    else if (b == "right") buttons |= PAD_BUTTON_RIGHT;
    else return false;
    start = end + 1;
  }
  return true;
}

// Frame-level commands, and unknown ones.
void RunFrame() {
  const std::string& name = sCmd.args[0];
  if (name == "help") {
    CmdHelp();
    Finish();
  } else if (name == "worlds") {
    if (gpMemoryCard == nullptr || gpMemoryCard->GetMemoryWorlds().empty()) {
      return Finish("world list not loaded yet");
    }
    CmdWorlds();
    Finish();
  } else if (name == "quit") {
    Finish();
    sQuit = true;
  } else if (name == "aspect") {
    const std::string mode = sCmd.args.size() > 1 ? Lower(sCmd.args[1]) : "";
    if (mode == "4:3") {
      PortDebug::SetAspectMode(PortDebug::kAspect_4_3);
    } else if (mode == "16:9") {
      PortDebug::SetAspectMode(PortDebug::kAspect_16_9);
    } else if (mode == "window") {
      PortDebug::SetAspectMode(PortDebug::kAspect_Window);
    } else {
      return Finish("usage: aspect <4:3|16:9|window>");
    }
    Finish();
  } else if (name == "wait") {
    unsigned frames = 0;
    if (sCmd.phase == 0) {
      if (sCmd.args.size() < 2 || !ParseUnsigned(sCmd.args[1], frames)) {
        return Finish("usage: wait <frames>");
      }
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + frames;
    }
    if (sFrame >= sCmd.untilFrame) {
      Finish();
    }
  } else if (name == "press" || name == "stick" || name == "cstick") {
    if (sCmd.phase == 0) {
      unsigned frames = 6;
      PADStatus& pad = sCmd.pad;
      pad = PADStatus{};
      pad.err = PAD_ERR_NONE;
      size_t framesArg;
      if (name == "press") {
        if (sCmd.args.size() < 2 || !ParseButtons(sCmd.args[1], pad.button, pad.triggerLeft,
                                                   pad.triggerRight)) {
          return Finish("usage: press <a+b+...> [frames]");
        }
        framesArg = 2;
      } else {
        float x, y;
        if (sCmd.args.size() < 3 || !ParseFloat(sCmd.args[1], x) || !ParseFloat(sCmd.args[2], y)) {
          return Finish("usage: stick <x> <y> [frames]");
        }
        const s8 sx = static_cast< s8 >(std::clamp(x, -127.f, 127.f));
        const s8 sy = static_cast< s8 >(std::clamp(y, -127.f, 127.f));
        (name == "stick" ? pad.stickX : pad.substickX) = sx;
        (name == "stick" ? pad.stickY : pad.substickY) = sy;
        framesArg = 3;
      }
      if (sCmd.args.size() > framesArg && !ParseUnsigned(sCmd.args[framesArg], frames)) {
        return Finish("bad frame count");
      }
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + std::max(frames, 1u);
    }
    if (sFrame < sCmd.untilFrame) {
      PADSetVirtualStatus(0, &sCmd.pad);
    } else {
      // The virtual status is sticky; release so the next press is an edge.
      PADStatus released{};
      released.err = PAD_ERR_NONE;
      PADSetVirtualStatus(0, &released);
      Finish();
    }
  } else if (name == "shot") {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::current_path(ec) / "screenshots";
    const auto count = [&dir] {
      std::error_code e;
      size_t n = 0;
      for (fs::directory_iterator it(dir, e), end; !e && it != end; it.increment(e)) {
        ++n;
      }
      return n;
    };
    if (sCmd.phase == 0) {
      sCmd.shotCount = count();
      aurora::request_screenshot();
      sCmd.phase = 1;
      sCmd.untilFrame = sFrame + 120;
      return;
    }
    if (count() > sCmd.shotCount) {
      fs::path newest;
      fs::file_time_type newestTime{};
      for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_time_type t = it->last_write_time(ec);
        if (newest.empty() || t > newestTime) {
          newest = it->path();
          newestTime = t;
        }
      }
      Out("%s", newest.string().c_str());
      Finish();
    } else if (sFrame >= sCmd.untilFrame) {
      Finish("no screenshot appeared");
    }
  } else {
    Finish("unknown command (try help)");
  }
}

void Tokenize(const std::string& line, std::vector< std::string >& args) {
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast< unsigned char >(line[i]))) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    size_t j = i;
    while (j < line.size() && !std::isspace(static_cast< unsigned char >(line[j]))) {
      ++j;
    }
    args.push_back(line.substr(i, j - i));
    i = j;
  }
}

} // namespace

bool PortConsoleFrame(unsigned frame) {
  if (!sStarted) {
    Start();
  }
  if (!sEnabled) {
    return false;
  }
  sFrame = frame;
  while (!sHasCommand) {
    Incoming next;
    {
      std::lock_guard< std::mutex > lock(sQueueMutex);
      if (sQueue.empty()) {
        break;
      }
      next = sQueue.front();
      sQueue.pop_front();
    }
    sCmd = Command{};
    Tokenize(next.line, sCmd.args);
    if (sCmd.args.empty()) {
      continue;
    }
    sCmd.args[0] = Lower(sCmd.args[0]);
    sCmd.generation = next.generation;
    sCmd.startFrame = frame;
    sHasCommand = true;
  }
  if (sHasCommand) {
    if (!IsTickCommand(sCmd.args[0])) {
      RunFrame();
    } else if (sCmd.phase == 0 &&
               frame - sCmd.startFrame > (sCmd.args[0] == "warp" ? kWarpTimeout : kTickTimeout)) {
      // A warp sent while the game boots waits for it to reach a world.
      Finish("the game is not ticking (not in a world, or paused)");
    } else if (sCmd.phase != 0 && frame > sCmd.untilFrame) {
      Finish("timed out");
    }
  }
  return sQuit;
}

void PortConsoleTick(CStateManager& mgr) {
  if (!sEnabled || !sHasCommand || !IsTickCommand(sCmd.args[0])) {
    return;
  }
  RunTick(mgr);
}
