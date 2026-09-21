#pragma once

// Binding-aware button prompt icons.
//
// The game draws each prompt from a fixed texture, so a shipped replacement can
// only ever show one device's icon. This module instead registers, at runtime,
// the icon for the input actually bound to each prompt's action, so rebinding a
// key or mouse button is reflected in game. The icons are pre-rendered per
// input by tools/make_prompt_glyphs.py into <textures>/bindings/.
//
// Only the keyboard/mouse set follows the bindings; with a pad the static
// per-device icons already match the pad's own labels.
namespace PortPrompts {
// Call once after Aurora is up, with the same texture root the replacements
// came from (null or empty disables the module).
void Initialize(const char* textureRoot);

// Re-registers an action's icon when its binding changed. Call once per frame.
void Poll();
} // namespace PortPrompts
