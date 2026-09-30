#pragma once

// Debug model viewer: draws one CMDL in front of the first-person camera, lit by
// white ambient, so a model (retail or a mod's replacement) can be inspected and
// captured without finding it in a room. Driven by the console's `viewmodel`.

#include <cstdint>
#include <string>

class CStateManager;

namespace PortViewModel {

// dist <= 0 picks a distance that fits the model's bounds. Angles in degrees.
// Returns false with err set when the id isn't a CMDL.
bool Show(uint32_t id, float dist, float yaw, float pitch, std::string& err);
void Hide();
// While a model is shown the arm cannon is hidden, so it doesn't cover the view.
bool Active();
std::string Status();

// Called from CStateManager::DrawWorld where the arm cannon is drawn.
void Draw(const CStateManager& mgr);

} // namespace PortViewModel
