// Binding-aware button prompt icons; see port_prompts.h.

#include "port_prompts.h"

#include "port_textures.h"

#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <aurora/texture.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
// Stands in for the C-stick, which is an axis rather than a button, so the
// table can name the prompts that show it.
constexpr PADButton PAD_AXIS_CSTICK = 0;

// The prompt textures the port can re-icon. Each names the game action it
// stands for, and the hash identifies the game's own texture as it appears in
// a dump. One action usually has several, since each screen draws its own art.
struct PromptKey {
  PADButton button;
  uint32_t width;
  uint32_t height;
  uint64_t hash;
  const char* format;
};
constexpr PromptKey kKeys[] = {
    // Front end.
    {PAD_BUTTON_A, 32, 32, 0xbb21e8755f36b2f0ull, "5"},
    {PAD_BUTTON_B, 32, 32, 0xe6dbfd18d4666ee7ull, "5"},
    // Pause / inventory. The B prompt reuses the front end's texture; the two
    // shoulder prompts have their own, one per side.
    {PAD_BUTTON_A, 32, 32, 0x281ae5aa517797edull, "5"},
    {PAD_TRIGGER_L, 32, 32, 0x3f419d4a7ba3cff3ull, "5"},
    {PAD_TRIGGER_R, 32, 32, 0x178b7311fda3f949ull, "5"},
    // Map screen.
    {PAD_TRIGGER_L, 32, 32, 0x06ad76760dcad506ull, "5"},
    {PAD_TRIGGER_R, 32, 32, 0x45ccec4d3cda3f1bull, "5"},
    {PAD_TRIGGER_Z, 64, 32, 0x0f4cb495c960bcfaull, "14"},
    // HUD hint memos.
    {PAD_TRIGGER_R, 32, 32, 0xc39b2f9c2eac777bull, "5"},
    // Stick prompts. Not a button, so there is no binding to follow; the icon
    // is the device's own stick (or the direction keys for a keyboard).
    //
    // 0x1ff9d2b310c0b706 at 32x32 is not a stick prompt: it is the pause menu's
    // Exit sphere, and listing it here once wrote the arrow glyph over it. It
    // was also the only 32x32 entry with format 14 while the others are format
    // 5, which is the tell.
    //
    // 0x2d26352b420db007 is the map screen's Rotate prompt. It was missing, so
    // the map showed no stick icon at all. Found by MP_DUMP_TEXTURES=1, which
    // dumps only the textures that have no replacement, and then LOOKING at
    // them - Rotate turned out to be a grey spiral emblem.
    //
    // 0xe14dc493b5513d14 dumps as a yellow "C" badge: a C-stick prompt, just
    // not the map's.
    {PAD_AXIS_CSTICK, 64, 32, 0x2d26352b420db007ull, "5"},
    {PAD_AXIS_CSTICK, 64, 32, 0xe14dc493b5513d14ull, "5"},
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

// The actions to resolve bindings for, resolved once per action rather than
// once per texture.
struct PromptAction {
  PADButton button;
  const char* label;
};
constexpr PromptAction kActions[] = {
    {PAD_BUTTON_A, "A"},
    {PAD_BUTTON_B, "B"},
    {PAD_TRIGGER_L, "L"},
    {PAD_TRIGGER_R, "R"},
    {PAD_TRIGGER_Z, "Z"},
    {PAD_AXIS_CSTICK, "stick"},
};
constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

// Scancodes mapped to icon stems. Aurora reports mouse buttons as negative
// codes; the rest are SDL scancodes. Stems match the files generated into
// <textures>/bindings/ by tools/make_prompt_glyphs.py.
struct KeyIcon {
  int scancode;
  const char* stem;
};
constexpr KeyIcon kKeyIcons[] = {
    {SDL_SCANCODE_A, "keyboard_a"}, {SDL_SCANCODE_B, "keyboard_b"},
    {SDL_SCANCODE_C, "keyboard_c"}, {SDL_SCANCODE_D, "keyboard_d"},
    {SDL_SCANCODE_E, "keyboard_e"}, {SDL_SCANCODE_F, "keyboard_f"},
    {SDL_SCANCODE_G, "keyboard_g"}, {SDL_SCANCODE_H, "keyboard_h"},
    {SDL_SCANCODE_I, "keyboard_i"}, {SDL_SCANCODE_J, "keyboard_j"},
    {SDL_SCANCODE_K, "keyboard_k"}, {SDL_SCANCODE_L, "keyboard_l"},
    {SDL_SCANCODE_M, "keyboard_m"}, {SDL_SCANCODE_N, "keyboard_n"},
    {SDL_SCANCODE_O, "keyboard_o"}, {SDL_SCANCODE_P, "keyboard_p"},
    {SDL_SCANCODE_Q, "keyboard_q"}, {SDL_SCANCODE_R, "keyboard_r"},
    {SDL_SCANCODE_S, "keyboard_s"}, {SDL_SCANCODE_T, "keyboard_t"},
    {SDL_SCANCODE_U, "keyboard_u"}, {SDL_SCANCODE_V, "keyboard_v"},
    {SDL_SCANCODE_W, "keyboard_w"}, {SDL_SCANCODE_X, "keyboard_x"},
    {SDL_SCANCODE_Y, "keyboard_y"}, {SDL_SCANCODE_Z, "keyboard_z"},
    {SDL_SCANCODE_0, "keyboard_0"}, {SDL_SCANCODE_1, "keyboard_1"},
    {SDL_SCANCODE_2, "keyboard_2"}, {SDL_SCANCODE_3, "keyboard_3"},
    {SDL_SCANCODE_4, "keyboard_4"}, {SDL_SCANCODE_5, "keyboard_5"},
    {SDL_SCANCODE_6, "keyboard_6"}, {SDL_SCANCODE_7, "keyboard_7"},
    {SDL_SCANCODE_8, "keyboard_8"}, {SDL_SCANCODE_9, "keyboard_9"},
    {SDL_SCANCODE_UP, "keyboard_arrow_up"}, {SDL_SCANCODE_DOWN, "keyboard_arrow_down"},
    {SDL_SCANCODE_LEFT, "keyboard_arrow_left"}, {SDL_SCANCODE_RIGHT, "keyboard_arrow_right"},
    {SDL_SCANCODE_F1, "keyboard_f1"}, {SDL_SCANCODE_F2, "keyboard_f2"},
    {SDL_SCANCODE_F3, "keyboard_f3"}, {SDL_SCANCODE_F4, "keyboard_f4"},
    {SDL_SCANCODE_F5, "keyboard_f5"}, {SDL_SCANCODE_F6, "keyboard_f6"},
    {SDL_SCANCODE_F7, "keyboard_f7"}, {SDL_SCANCODE_F8, "keyboard_f8"},
    {SDL_SCANCODE_F9, "keyboard_f9"}, {SDL_SCANCODE_F10, "keyboard_f10"},
    {SDL_SCANCODE_F11, "keyboard_f11"}, {SDL_SCANCODE_F12, "keyboard_f12"},
    {SDL_SCANCODE_SPACE, "keyboard_space"}, {SDL_SCANCODE_RETURN, "keyboard_enter"},
    {SDL_SCANCODE_ESCAPE, "keyboard_escape"}, {SDL_SCANCODE_TAB, "keyboard_tab"},
    {SDL_SCANCODE_BACKSPACE, "keyboard_backspace"}, {SDL_SCANCODE_DELETE, "keyboard_delete"},
    {SDL_SCANCODE_INSERT, "keyboard_insert"}, {SDL_SCANCODE_HOME, "keyboard_home"},
    {SDL_SCANCODE_END, "keyboard_end"}, {SDL_SCANCODE_PAGEUP, "keyboard_page_up"},
    {SDL_SCANCODE_PAGEDOWN, "keyboard_page_down"},
    {SDL_SCANCODE_LSHIFT, "keyboard_shift"}, {SDL_SCANCODE_RSHIFT, "keyboard_shift"},
    {SDL_SCANCODE_LCTRL, "keyboard_ctrl"}, {SDL_SCANCODE_RCTRL, "keyboard_ctrl"},
    {SDL_SCANCODE_LALT, "keyboard_alt"}, {SDL_SCANCODE_RALT, "keyboard_alt"},
    {SDL_SCANCODE_LGUI, "keyboard_win"}, {SDL_SCANCODE_RGUI, "keyboard_win"},
    {SDL_SCANCODE_MINUS, "keyboard_minus"}, {SDL_SCANCODE_EQUALS, "keyboard_equals"},
    {SDL_SCANCODE_LEFTBRACKET, "keyboard_bracket_open"},
    {SDL_SCANCODE_RIGHTBRACKET, "keyboard_bracket_close"},
    {SDL_SCANCODE_SEMICOLON, "keyboard_semicolon"}, {SDL_SCANCODE_APOSTROPHE, "keyboard_apostrophe"},
    {SDL_SCANCODE_COMMA, "keyboard_comma"}, {SDL_SCANCODE_PERIOD, "keyboard_period"},
    {SDL_SCANCODE_SLASH, "keyboard_slash_forward"}, {SDL_SCANCODE_BACKSLASH, "keyboard_slash_back"},
    {SDL_SCANCODE_NONUSBACKSLASH, "keyboard_slash_back"}, {SDL_SCANCODE_GRAVE, "keyboard_tilde"},
    {SDL_SCANCODE_CAPSLOCK, "keyboard_capslock"}, {SDL_SCANCODE_PRINTSCREEN, "keyboard_printscreen"},
    {SDL_SCANCODE_SCROLLLOCK, "keyboard_scroll_lock"}, {SDL_SCANCODE_PAUSE, "keyboard_pause"},
    // The keypad. The pack has no keypad digits, so those share the main row's.
    {SDL_SCANCODE_KP_0, "keyboard_0"}, {SDL_SCANCODE_KP_1, "keyboard_1"},
    {SDL_SCANCODE_KP_2, "keyboard_2"}, {SDL_SCANCODE_KP_3, "keyboard_3"},
    {SDL_SCANCODE_KP_4, "keyboard_4"}, {SDL_SCANCODE_KP_5, "keyboard_5"},
    {SDL_SCANCODE_KP_6, "keyboard_6"}, {SDL_SCANCODE_KP_7, "keyboard_7"},
    {SDL_SCANCODE_KP_8, "keyboard_8"}, {SDL_SCANCODE_KP_9, "keyboard_9"},
    {SDL_SCANCODE_KP_ENTER, "keyboard_numpad_enter"}, {SDL_SCANCODE_KP_PLUS, "keyboard_numpad_plus"},
    {SDL_SCANCODE_KP_MINUS, "keyboard_minus"}, {SDL_SCANCODE_KP_MULTIPLY, "keyboard_asterisk"},
    {SDL_SCANCODE_KP_DIVIDE, "keyboard_slash_forward"}, {SDL_SCANCODE_KP_PERIOD, "keyboard_period"},
    {SDL_SCANCODE_NUMLOCKCLEAR, "keyboard_numlock"},
    {PAD_KEY_MOUSE_LEFT, "mouse_left"}, {PAD_KEY_MOUSE_RIGHT, "mouse_right"},
    {PAD_KEY_MOUSE_MIDDLE, "mouse_scroll"},
    {PAD_KEY_MOUSE_X1, "mouse_side_back"}, {PAD_KEY_MOUSE_X2, "mouse_side_forward"},
};

// A GameCube pad's buttons carry GameCube labels, which SDL's positional names
// don't give; Aurora's default GC mappings (lib/dolphin/pad/pad.cpp) pin each
// SDL button to the GC button of the same name, so they double as the labels.
// Kept in step with g_defaultButtonsGamecube / g_defaultButtonsNSOGamecube.
struct GameCubeButton {
  int sdlButton;
  PADButton action;
  const char* stem;
};
constexpr GameCubeButton kGameCubeButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A, "gamecube_a"},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_B, "gamecube_b"},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X, "gamecube_x"},
    {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y, "gamecube_y"},
    {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START, "gamecube_start"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z, "gamecube_z"},
    {SDL_GAMEPAD_BUTTON_MISC3, PAD_TRIGGER_L, "gamecube_l"},
    {SDL_GAMEPAD_BUTTON_MISC4, PAD_TRIGGER_R, "gamecube_r"},
    {PAD_NATIVE_BUTTON_TRIGGER_LEFT, PAD_TRIGGER_L, "gamecube_l"},
    {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_TRIGGER_R, "gamecube_r"},
};
constexpr GameCubeButton kNsoGameCubeButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A, "gamecube_a"},
    {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_B, "gamecube_b"},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X, "gamecube_x"},
    {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y, "gamecube_y"},
    {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START, "gamecube_start"},
    {SDL_GAMEPAD_BUTTON_BACK, PAD_TRIGGER_Z, "gamecube_z"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_L, "gamecube_l"},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_R, "gamecube_r"},
    {PAD_NATIVE_BUTTON_TRIGGER_LEFT, PAD_TRIGGER_L, "gamecube_l"},
    {PAD_NATIVE_BUTTON_TRIGGER_RIGHT, PAD_TRIGGER_R, "gamecube_r"},
};

// The icon for `button` on a GameCube pad, or empty to leave the game's own art,
// which is already right for a GC pad on its default mapping. So only an action
// moved to another button gets an icon. The GameCube touch layout also resolves
// to "gamecube", hence the check that a GC pad really is in port 0.
std::string GameCubeStemForButton(PADButton button) {
  const PADControllerType type = PADGetControllerType(PAD_CHAN0);
  const GameCubeButton* table = nullptr;
  size_t tableSize = 0;
  if (type == PAD_TYPE_GAMECUBE) {
    table = kGameCubeButtons;
    tableSize = sizeof(kGameCubeButtons) / sizeof(kGameCubeButtons[0]);
  } else if (type == PAD_TYPE_NSO_GAMECUBE) {
    table = kNsoGameCubeButtons;
    tableSize = sizeof(kNsoGameCubeButtons) / sizeof(kNsoGameCubeButtons[0]);
  } else {
    return {};
  }
  u32 count = 0;
  PADButtonMapping* mappings = PADGetButtonMappings(PAD_CHAN0, &count);
  for (u32 i = 0; mappings != nullptr && i < count; ++i) {
    if (mappings[i].padButton != button) {
      continue;
    }
    // Unbound means the analog trigger for L and R, which is the default too.
    const int native = static_cast<int>(mappings[i].nativeButton);
    for (size_t j = 0; j < tableSize; ++j) {
      if (table[j].sdlButton == native) {
        return table[j].action == button ? std::string() : std::string(table[j].stem);
      }
    }
    break;
  }
  return {};
}

// The SDL button a mapping points at, named the way the generated pad icons
// are (tools/make_prompt_glyphs.py writes "<device>_<suffix>").
const char* SuffixForSdlButton(int button) {
  switch (button) {
  case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
  case SDL_GAMEPAD_BUTTON_EAST: return "east";
  case SDL_GAMEPAD_BUTTON_WEST: return "west";
  case SDL_GAMEPAD_BUTTON_NORTH: return "north";
  case SDL_GAMEPAD_BUTTON_START: return "start";
  case SDL_GAMEPAD_BUTTON_BACK: return "back";
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "leftshoulder";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "rightshoulder";
  case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "leftstick";
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "rightstick";
  case PAD_NATIVE_BUTTON_TRIGGER_LEFT: return "lt";
  case PAD_NATIVE_BUTTON_TRIGGER_RIGHT: return "rt";
  default: return nullptr;
  }
}

struct Registration {
  std::string iconPath;  // stable storage for the read callback
  std::string activeStem;
  aurora::texture::ReplacementRegistration handle{};
  bool registered = false;
};
Registration sRegistrations[kKeyCount];
std::string sBindingsDir;
bool sEnabled = false;

// Which input the player last used. The prompts, and the static texture set
// (PortTextures asks ActiveDevice too), follow that rather than whatever happens
// to be plugged in, so switching between a keyboard, a pad and the touch overlay
// swaps the icons over. Pad is the starting state: with no pad connected the pad
// device resolves to "keyboard" anyway.
enum class ActiveInput { Pad, Keyboard, TouchXbox, TouchGameCube };
std::atomic< ActiveInput > sActiveInput{ActiveInput::Pad};

// Keys sent by the phone or the OS rather than a keyboard in the player's hands.
bool IsSystemKey(SDL_Scancode scancode) {
  switch (scancode) {
  case SDL_SCANCODE_AC_BACK:
  case SDL_SCANCODE_VOLUMEUP:
  case SDL_SCANCODE_VOLUMEDOWN:
  case SDL_SCANCODE_MUTE:
  case SDL_SCANCODE_MEDIA_PLAY:
  case SDL_SCANCODE_MEDIA_PAUSE:
  case SDL_SCANCODE_MEDIA_PLAY_PAUSE:
  case SDL_SCANCODE_MEDIA_NEXT_TRACK:
  case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:
  case SDL_SCANCODE_MEDIA_STOP:
  case SDL_SCANCODE_POWER:
    return true;
  default:
    return false;
  }
}

bool IsTouchMouse(SDL_MouseID which) { return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID; }

// Only deliberate input switches the device. The touch overlay is itself a
// virtual pad, whose events would undo NoteTouchInput on the next pump; a touch
// or pen also arrives as a mouse; and a stick resting a little off centre would
// flip back to the pad every frame while the keyboard is in use, and each flip
// re-registers textures and clears Aurora's texture cache.
bool SDLCALL active_input_watch(void*, SDL_Event* event) {
  switch (event->type) {
  case SDL_EVENT_MOUSE_MOTION:
    if (!IsTouchMouse(event->motion.which)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (!IsTouchMouse(event->button.which)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    if (!IsTouchMouse(event->wheel.which)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_KEY_DOWN:
    // Not TEXT_INPUT: on Android that is the soft keyboard typing into the
    // overlay, not a physical keyboard.
    if (!event->key.repeat && !IsSystemKey(event->key.scancode)) {
      sActiveInput.store(ActiveInput::Keyboard, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    if (!SDL_IsJoystickVirtual(event->gbutton.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    // The same threshold the touch overlay uses to decide a pad is in use.
    if (std::abs(static_cast< int >(event->gaxis.value)) > 16000 &&
        !SDL_IsJoystickVirtual(event->gaxis.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  case SDL_EVENT_GAMEPAD_ADDED:
    if (!SDL_IsJoystickVirtual(event->gdevice.which)) {
      sActiveInput.store(ActiveInput::Pad, std::memory_order_relaxed);
    }
    break;
  default:
    break;
  }
  return true;
}

// The width and height a DDS header declares, or false if the file is too short
// to hold one. A DDS opens with the "DDS " magic then a 124-byte header, of
// which the height and width are the two little-endian uint32 at offset 12.
bool IconDimensions(const std::string& path, uint32_t& width, uint32_t& height) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  char magic[4] = {};
  if (!in.read(magic, 4) || std::strncmp(magic, "DDS ", 4) != 0) {
    return false;
  }
  in.seekg(12, std::ios::beg);
  uint32_t both[2] = {};
  if (!in.read(reinterpret_cast<char*>(both), sizeof(both))) {
    return false;
  }
  height = both[0];
  width = both[1];
  return true;
}

// Serves one generated icon as if it were a replacement file. Called from
// Aurora worker threads, so it only touches the filesystem.
bool ReadIconBytes(void* userData, const char* path, std::vector<uint8_t>& out) {
  // The prompts are 32x32 with a single mip, so any mip sidecar Aurora probes
  // for is a miss rather than the same image again.
  if (path != nullptr && std::strstr(path, "_mip") != nullptr) {
    return false;
  }
  const auto* filePath = static_cast<const std::string*>(userData);
  std::ifstream in(*filePath, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out.data()), size);
  return !in.fail();
}

// The icon stem for whatever is bound to `button` on `device`, or empty when
// the port has no icon for it (in which case the static set stays in place).
// Keyboard and mouse come from the key bindings; a pad follows its own button
// mapping, so a remapped button shows the button it was remapped to.
std::string IconStemForButton(PADButton button, const char* device) {
  const bool keyboard = std::strcmp(device, "keyboard") == 0;
  if (std::strcmp(device, "gamecube") == 0) {
    // The game's C-stick art is the GameCube stick already.
    return button == PAD_AXIS_CSTICK ? std::string() : GameCubeStemForButton(button);
  }
  if (button == PAD_AXIS_CSTICK) {
    // A stick is bound to several keys at once, so the icon says "direction
    // keys" rather than naming one; a pad shows its own stick.
    return keyboard ? std::string("keyboard_arrows") : std::string(device) + "_stick";
  }

  u32 count = 0;
  if (keyboard) {
    // The main key's icon, else the alt key's.
    for (u32 slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
      PADKeyButtonBinding* bindings = PADGetKeyButtonBindingsSlot(PAD_CHAN0, slot, &count);
      for (u32 i = 0; bindings != nullptr && i < count; ++i) {
        if (bindings[i].padButton != button) {
          continue;
        }
        for (const KeyIcon& icon : kKeyIcons) {
          if (icon.scancode == bindings[i].scancode) {
            return icon.stem;
          }
        }
      }
    }
    return {};
  }

  // A pad's L and R are analog triggers unless remapped to a button (a preset
  // puts R on a stick click); the generated set carries the triggers under one
  // name per action.
  const bool trigger = button == PAD_TRIGGER_L || button == PAD_TRIGGER_R;
  const std::string triggerStem = std::string(device) + (button == PAD_TRIGGER_L ? "_lt" : "_rt");
  PADButtonMapping* mappings = PADGetButtonMappings(PAD_CHAN0, &count);
  for (u32 i = 0; mappings != nullptr && i < count; ++i) {
    if (mappings[i].padButton != button) {
      continue;
    }
    const char* suffix = SuffixForSdlButton(static_cast<int>(mappings[i].nativeButton));
    if (suffix != nullptr) {
      return std::string(device) + "_" + suffix;
    }
    break;
  }
  return trigger ? triggerStem : std::string();
}

void Apply(size_t index, const std::string& stem) {
  Registration& reg = sRegistrations[index];
  if (reg.registered) {
    aurora::texture::unregister_replacement(reg.handle);
    reg.handle = {};
    reg.registered = false;
  }
  reg.activeStem = stem;
  if (stem.empty()) {
    return;
  }

  const PromptKey& key = kKeys[index];
  char keyName[80];
  std::snprintf(keyName, sizeof(keyName), "tex1_%ux%u_%016llx_%s.dds", key.width, key.height,
                static_cast<unsigned long long>(key.hash), key.format);
  // Prefer the binding generated at exactly this texture's size, then the
  // unsuffixed one. A single stem can serve textures of different sizes - the
  // C-stick prompts here are one 32x32 and one 64x32 - and a mismatch is not
  // scaled by the game: it reads 64x32 of pixels out of 32x32 of data and draws
  // the right-hand half as noise. tools/make_prompt_glyphs.py writes the sized
  // variants for exactly this reason.
  char sized[160];
  std::snprintf(sized, sizeof(sized), "%s_%ux%u.dds", stem.c_str(), key.width, key.height);
  const std::filesystem::path base(sBindingsDir);
  std::error_code existsError;
  if (std::filesystem::is_regular_file(base / sized, existsError)) {
    reg.iconPath = (base / sized).string();
  } else if (std::filesystem::is_regular_file(base / (stem + ".dds"), existsError)) {
    reg.iconPath = (base / (stem + ".dds")).string();
  } else {
    // Nothing generated for this action, so nothing is registered. Note that
    // what stays on screen is not necessarily "the game's own art": the static
    // per-device set in <textures>/<device>/ is registered separately, by
    // filename, so if it has a file for this texture then that is what shows -
    // art for the default bindings. Saying "the game's own art" here would be
    // wrong in the common case.
    //
    // activeStem is deliberately NOT cleared. Poll() skips a key whose stem
    // already matches, so clearing it made Poll re-run Apply on this key every
    // single frame: a stat, a DDS open and a stderr line, 60 times a second,
    // for the rest of the session. Two runs of the map screen logged 4200 of
    // them. Leaving the stem recorded means one attempt and then silence.
    return;
  }
  // Last line of defence, in case a sized file is ever wrong: refuse art whose
  // dimensions do not match the texture it is standing in for. As above,
  // activeStem is left recorded so Poll does not retry, and as above what
  // remains on screen is the static device set if it has one, not necessarily
  // the game's own art.
  uint32_t iconWidth = 0;
  uint32_t iconHeight = 0;
  if (!IconDimensions(reg.iconPath, iconWidth, iconHeight) ||
      (iconWidth != key.width || iconHeight != key.height)) {
    return;
  }
  // Above the static device set, which registers at the default priority. At
  // equal priority the newest registration wins, so a reload of that set (a
  // pad unplugged, say) buried the binding icons, and Poll does not re-apply a
  // stem that has not changed.
  reg.handle = aurora::texture::register_virtual_replacement(
      keyName, aurora::texture::VirtualFileSource{&ReadIconBytes, &reg.iconPath},
      aurora::texture::ReplacementOptions{.priority = 1});
  reg.registered = reg.handle.id != 0;
}

const char* LabelForButton(PADButton button) {
  for (const PromptAction& action : kActions) {
    if (action.button == button) {
      return action.label;
    }
  }
  return "?";
}
} // namespace

namespace PortPrompts {
// "gamecube" has no static set, which is deliberate: the game's own prompt art
// already is the GameCube set, so only a remapped GC pad button gets an icon
// (GameCubeStemForButton).
const char* ActiveDevice() {
  const char* env = std::getenv("MP_TEXTURE_DEVICE");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }
  switch (sActiveInput.load(std::memory_order_relaxed)) {
  case ActiveInput::Keyboard:
    return "keyboard";
  case ActiveInput::TouchXbox:
    return "xbox";
  case ActiveInput::TouchGameCube:
    return "gamecube";
  case ActiveInput::Pad:
  default:
    return PortTextures::PadDeviceName();
  }
}

void Initialize(const char* textureRoot) {
  if (textureRoot == nullptr || textureRoot[0] == '\0') {
    return;
  }
  // Tracked even without generated icons, since the static set follows it too.
  SDL_AddEventWatch(active_input_watch, nullptr);
  const std::filesystem::path bindingsDir = std::filesystem::path(textureRoot) / "bindings";
  std::error_code ec;
  if (!std::filesystem::is_directory(bindingsDir, ec)) {
    return;
  }
  sBindingsDir = bindingsDir.string();
  sEnabled = true;
  Poll();
}

void Poll() {
  if (!sEnabled) {
    return;
  }
  const char* device = ActiveDevice();
  for (const PromptAction& action : kActions) {
    const std::string stem = IconStemForButton(action.button, device);
    size_t applied = 0;
    for (size_t i = 0; i < kKeyCount; ++i) {
      if (kKeys[i].button != action.button) {
        continue;
      }
      const std::string& current = sRegistrations[i].activeStem;
      if (stem.empty() && current.empty()) {
        continue;
      }
      if (!stem.empty() && stem == current) {
        continue;
      }
      Apply(i, stem);
      ++applied;
    }
    if (applied != 0) {
      std::fprintf(stderr, "metroid_prime_port: prompt %s %s\n", LabelForButton(action.button),
                   stem.empty() ? "(back to the static icon)" : stem.c_str());
    }
  }
}
void NoteTouchInput(bool xboxLayout) {
  sActiveInput.store(xboxLayout ? ActiveInput::TouchXbox : ActiveInput::TouchGameCube,
                     std::memory_order_relaxed);
}
} // namespace PortPrompts
