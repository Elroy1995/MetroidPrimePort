// Room geometry at run time: which areas have a file, their models, and the draw. See
// port_room_geo.h.
#include "port_room_geo.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_env.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

namespace PortRoomGeo {
namespace {

struct Model {
  uint32_t id = 0;
  std::unique_ptr< CModelData > data;
  bool loaded = false;
  bool hidden = false; // by the console, to find which model a surface belongs to
  CAABox bounds = CAABox::MakeMaxInvertedBox();
};

struct Placed {
  size_t model;
  CTransform4f xf = CTransform4f::Identity(); // model -> world
  CAABox bounds = CAABox::MakeMaxInvertedBox();
  bool bounded = false;
  std::unique_ptr< CActorLights > lights;
  bool areaLit = false; // `lights` holds the area's lights
};

struct Area {
  bool hasFile = false;
  bool placed = false; // the instances have their world transforms
  std::vector< Instance > instances;
  std::vector< Model > models;
  std::vector< Placed > items;
  size_t loaded = 0;
};

// Areas in memory; one without a file has no instances. Never destroyed: the models' tokens
// must not outlive the game's resource pool, which static destruction would not respect.
std::unordered_map< uint32_t, Area >& Areas() {
  static auto* const areas = new std::unordered_map< uint32_t, Area >();
  return *areas;
}

int sMode = -1;
bool sBuffersReady = false;
bool sWarned = false;
int sDrawn = 0;
int sDrawnLast = 0;

void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomGeoPath(mrea);
  if (path.empty() || gpResourceFactory == nullptr) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector< uint8_t > data((std::istreambuf_iterator< char >(in)),
                                    std::istreambuf_iterator< char >());
  std::string error;
  if (!in || !Parse(data, area.instances, error)) {
    PortLog::Write("room geo: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.instances.clear();
    return;
  }
  // A model the import could not convert leaves its instances behind.
  std::unordered_map< uint32_t, size_t > index;
  size_t missing = 0;
  for (const Instance& instance : area.instances) {
    auto found = index.find(instance.model);
    if (found == index.end()) {
      size_t slot = size_t(-1);
      if (gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(instance.model)) == 'CMDL') {
        slot = area.models.size();
        Model& model = area.models.emplace_back();
        model.id = instance.model;
        model.data.reset(new CModelData(
            CStaticRes(static_cast< CAssetId >(instance.model), CVector3f(1.f, 1.f, 1.f))));
      }
      found = index.emplace(instance.model, slot).first;
    }
    if (found->second == size_t(-1)) {
      ++missing;
      continue;
    }
    Placed& item = area.items.emplace_back();
    item.model = found->second;
    const float* const m = instance.transform;
    item.xf = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  }
  area.instances.clear();
  area.instances.shrink_to_fit();
  area.hasFile = !area.items.empty();
  PortLog::Write("room geo: %08X: %zu instance(s) of %zu model(s), %zu without a model\n", mrea,
                 area.items.size(), area.models.size(), missing);
}

} // namespace

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  auto& areas = Areas();
  if (!sBuffersReady && !sWarned) {
    for (size_t i = 0; i < count; ++i) {
      if (!PortMods::RoomGeoPath(mreas[i]).empty()) {
        PortLog::Write("room geo: not drawn; the game started without room geometry installed. Restart it.\n");
        sWarned = true;
        break;
      }
    }
  }
  if (GetMode() == Mode::Off) {
    areas.clear();
    return;
  }
  for (auto it = areas.begin(); it != areas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      it = areas.erase(it);
    } else {
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (areas.find(mreas[i]) == areas.end()) {
      Load(mreas[i], areas[mreas[i]]);
    }
  }
  sDrawnLast = sDrawn;
  sDrawn = 0;
}

bool Draw(const CStateManager& mgr, const CGameArea& gameArea, const CFrustumPlanes& frustum) {
  if (GetMode() == Mode::Off) {
    return false;
  }
  auto& areas = Areas();
  const auto found = areas.find(gameArea.GetAreaAssetId());
  if (found == areas.end() || !found->second.hasFile) {
    return false;
  }
  Area& area = found->second;
  if (!area.placed) {
    for (Placed& item : area.items) {
      item.xf = gameArea.GetTM() * item.xf;
    }
    area.placed = true;
  }
  if (area.loaded != area.models.size()) {
    area.loaded = 0;
    for (Model& model : area.models) {
      if (!model.loaded) {
        if (!model.data->IsLoaded(0)) {
          model.data->Touch(CModelData::kWM_Normal, 0);
          continue;
        }
        model.bounds = model.data->GetBounds();
        model.loaded = true;
      }
      ++area.loaded;
    }
    // The area's own geometry stays until the last model is in, so no frame has holes.
    if (area.loaded != area.models.size() && GetMode() == Mode::Replace) {
      return false;
    }
  }
  static const bool areaLights = std::getenv("MP_ROOM_GEO_AREA_LIGHTS") != nullptr;
  const bool baked = !areaLights && PortRoomEnv::HasVolume(gameArea.GetAreaAssetId());
  for (Placed& item : area.items) {
    const Model& model = area.models[item.model];
    if (!model.loaded || model.hidden) {
      continue;
    }
    if (!item.bounded) {
      item.bounds = model.bounds.GetTransformedAABox(item.xf);
      item.bounded = true;
    }
    if (!frustum.BoxInFrustumPlanes(item.bounds)) {
      continue;
    }
    if (item.lights == nullptr || item.areaLit == baked) {
      // The baked ambient already holds the area's lights, so that set has room for none:
      // one that expects area lights and has none drops its dynamic lights too. Its
      // ambient is what a material outside PBR is drawn at.
      item.lights.reset(new CActorLights(8, CVector3f(0.f, 0.f, 0.f), 4, baked ? 0 : 4));
      if (baked) {
        item.lights->SetAmbientColor(CColor::White());
      }
      item.areaLit = !baked;
    }
    if (baked) {
      const CVector3f centre = item.bounds.GetCenterPoint();
      const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
      PortRoomEnv::SetVolumeHint(gameArea.GetAreaAssetId(), at);
    } else {
      item.lights->BuildAreaLightList(mgr, gameArea, item.bounds);
    }
    item.lights->BuildDynamicLightList(mgr, item.bounds);
    model.data->Render(CModelData::kWM_Normal, item.xf, item.lights.get(), CModelFlags::Normal());
    ++sDrawn;
  }
  if (baked) {
    PortRoomEnv::ClearVolumeHint();
  }
  return GetMode() == Mode::Replace;
}

bool sReplacingArea = false;

std::string At(const CVector3f& point, float margin) {
  std::string out;
  char line[160];
  for (const auto& [mrea, area] : Areas()) {
    for (const Placed& item : area.items) {
      const Model& model = area.models[item.model];
      if (!item.bounded) {
        continue;
      }
      const CVector3f lo = item.bounds.GetMinPoint();
      const CVector3f hi = item.bounds.GetMaxPoint();
      if (point.GetX() < lo.GetX() - margin || point.GetX() > hi.GetX() + margin ||
          point.GetY() < lo.GetY() - margin || point.GetY() > hi.GetY() + margin ||
          point.GetZ() < lo.GetZ() - margin || point.GetZ() > hi.GetZ() + margin) {
        continue;
      }
      std::snprintf(line, sizeof(line), "%08X in %08X: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)%s\n", model.id,
                    mrea, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ(),
                    model.hidden ? " hidden" : "");
      out += line;
    }
  }
  return out;
}

int SetHidden(uint32_t id, bool hidden) {
  int count = 0;
  for (auto& [mrea, area] : Areas()) {
    for (Model& model : area.models) {
      if (id == 0 || model.id == id) {
        model.hidden = hidden;
        ++count;
      }
    }
  }
  return count;
}

void Reset() { Areas().clear(); }

void SetMode(Mode mode) {
  sMode = int(mode);
  if (mode == Mode::Off) {
    Reset();
  }
}

void SetBuffersReady(bool ready) { sBuffersReady = ready; }

Mode GetMode() {
  if (!sBuffersReady) {
    return Mode::Off;
  }
  if (sMode < 0) {
    const char* const env = std::getenv("MP_ROOM_GEO");
    sMode = int(env == nullptr || env[0] == '\0' ? Mode::Replace
                : env[0] == '0'                  ? Mode::Off
                : env[0] == 'o'                  ? Mode::Overlay
                                                 : Mode::Replace);
  }
  return Mode(sMode);
}

void Stats(int& areaCount, int& instances, int& models, int& loaded, int& drawn) {
  areaCount = instances = models = loaded = 0;
  for (const auto& [mrea, area] : Areas()) {
    if (!area.hasFile) {
      continue;
    }
    ++areaCount;
    instances += int(area.items.size());
    models += int(area.models.size());
    for (const Model& model : area.models) {
      loaded += model.loaded ? 1 : 0;
    }
  }
  drawn = sDrawnLast;
}

} // namespace PortRoomGeo
