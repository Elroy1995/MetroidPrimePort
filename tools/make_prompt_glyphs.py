#!/usr/bin/env python3
"""Generate per-device button-prompt replacements for Metroid Prime.

The game's prompts are individual 32x32 RGB5A3 textures holding a coloured
button icon (GameCube A teal, B red). Each device gets its own icon for the
same two actions, so the in-game prompt matches the pad (or keyboard) in use.
The keyboard icons label the keys the port binds by default (A action is X,
B action is Z).

Icons come from Kenney's "Input Prompts" pack, which is CC0; the ones used are
vendored in tools/prompt_icons/ (see the README there). Run with an output
directory, usually the `textures` folder beside the executable:

    tools/make_prompt_glyphs.py textures

Output is an uncompressed 32-bit RGBA DDS. DDS rather than PNG because the
game's texture is RGB5A3 and Aurora's DDS path keeps the alpha where the PNG
path does not; the vertical flip matches how Aurora reads and writes DDS (the
dumps come out flipped). The icon is inset to match the game's own art, which
is opaque only from (6,5) to (27,26): the glyph is about 22px on screen, so a
full-bleed icon reads as a solid square.
"""
import os
import re
import struct
import sys
from PIL import Image

SIZE = 32
ICON_SIZE = 22
INSET = (SIZE - ICON_SIZE) // 2
ICONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_icons")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT_PROMPTS = os.path.join(ROOT, "platform", "port_prompts.cpp")
OUT = sys.argv[1] if len(sys.argv) > 1 else "textures"

# Icon per device and action. The keyboard set is the fallback used when a
# binding has no icon of its own, so it labels the port's default keys.
DEVICE_ICONS = {
    "xbox": {"a": "xbox_button_color_a.png", "b": "xbox_button_color_b.png",
             "l": "xbox_lt.png", "r": "xbox_rt.png", "stick": "xbox_stick_r.png"},
    "playstation": {"a": "playstation_button_color_cross.png",
                    "b": "playstation_button_color_circle.png",
                    "l": "playstation_trigger_l2.png", "r": "playstation_trigger_r2.png",
                    "stick": "playstation_stick_r.png"},
    "switch": {"a": "switch_button_a.png", "b": "switch_button_b.png",
               "l": "switch_button_zl.png", "r": "switch_button_zr.png",
               "stick": "switch_stick_r.png"},
    "keyboard": {"a": "keyboard_x.png", "b": "keyboard_z.png",
                 "l": "keyboard_q.png", "r": "keyboard_e.png",
                 "z": "keyboard_f.png", "stick": "keyboard_arrows.png"},
}

# Per-device icons for the pad's own buttons, keyed by the SDL gamepad button
# the port reports in the controller's mapping. The port builds the icon name as
# "<device>_<key>", so a remapped pad button follows the new button.
PAD_ICONS = {
    "xbox": {
        "south": "xbox_button_color_a.png", "east": "xbox_button_color_b.png",
        "west": "xbox_button_color_x.png", "north": "xbox_button_color_y.png",
        "start": "xbox_button_start.png", "back": "xbox_button_view.png",
        "leftshoulder": "xbox_lb.png", "rightshoulder": "xbox_rb.png",
        "leftstick": "xbox_stick_l_press.png", "rightstick": "xbox_stick_r_press.png",
        "lt": "xbox_lt.png", "rt": "xbox_rt.png",
        "stick": "xbox_stick_r.png",
    },
    "playstation": {
        "south": "playstation_button_color_cross.png",
        "east": "playstation_button_color_circle.png",
        "west": "playstation_button_color_square.png",
        "north": "playstation_button_color_triangle.png",
        "start": "playstation3_button_start.png", "back": "playstation3_button_select.png",
        "leftshoulder": "playstation_trigger_l1.png", "rightshoulder": "playstation_trigger_r1.png",
        "leftstick": "playstation_stick_l_press.png", "rightstick": "playstation_stick_r_press.png",
        "lt": "playstation_trigger_l2.png", "rt": "playstation_trigger_r2.png",
        "stick": "playstation_stick_r.png",
    },
    "switch": {
        "south": "switch_button_b.png", "east": "switch_button_a.png",
        "west": "switch_button_y.png", "north": "switch_button_x.png",
        "start": "switch_button_plus.png", "back": "switch_button_minus.png",
        "leftshoulder": "switch_button_l.png", "rightshoulder": "switch_button_r.png",
        "leftstick": "switch_stick_l_press.png", "rightstick": "switch_stick_r_press.png",
        "lt": "switch_button_zl.png", "rt": "switch_button_zr.png",
        "stick": "switch_stick_r.png",
    },
}
PAD_ICONS["standard"] = PAD_ICONS["xbox"]

# PAD_BUTTON_* / PAD_TRIGGER_* to the action name used above.
ACTION_FOR_BUTTON = {
    "PAD_BUTTON_A": "a",
    "PAD_BUTTON_B": "b",
    "PAD_TRIGGER_L": "l",
    "PAD_TRIGGER_R": "r",
    "PAD_TRIGGER_Z": "z",
    "PAD_AXIS_CSTICK": "stick",
}


def read_prompt_keys():
    """Parse the prompt texture table out of platform/port_prompts.cpp.

    Keeping the table in one place means a texture only has to be identified
    once, in the C++ where it is used.
    """
    pattern = re.compile(
        r"\{\s*(PAD_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*0x([0-9a-fA-F]+)ull\s*,\s*\"(\w+)\"\s*\}")
    with open(PORT_PROMPTS) as f:
        keys = pattern.findall(f.read())
    if not keys:
        raise SystemExit(f"no prompt keys found in {PORT_PROMPTS}")
    missing = sorted({k[0] for k in keys} - set(ACTION_FOR_BUTTON))
    if missing:
        raise SystemExit(f"no icon mapping for: {', '.join(missing)}")
    return [(ACTION_FOR_BUTTON[button], int(w), int(h), hsh, fmt) for button, w, h, hsh, fmt in keys]


def texture_name(w, h, hsh, fmt):
    return f"tex1_{w}x{h}_{hsh}_{fmt}.dds"

# One icon per input that can be bound, written to <out>/bindings/<stem>.dds.
# The port registers the icon for whichever input is bound to a prompt's
# action, so a rebound key or mouse button is reflected in game. Stems match
# the tables in platform/port_prompts.cpp.
BINDING_ICONS = [
    *[f"keyboard_{c}" for c in "abcdefghijklmnopqrstuvwxyz"],
    *[f"keyboard_{d}" for d in "0123456789"],
    "keyboard_arrow_up", "keyboard_arrow_down", "keyboard_arrow_left", "keyboard_arrow_right",
    *[f"keyboard_f{i}" for i in range(1, 13)],
    "keyboard_space", "keyboard_enter", "keyboard_escape", "keyboard_tab",
    "keyboard_backspace", "keyboard_delete", "keyboard_insert", "keyboard_home",
    "keyboard_end", "keyboard_page_up", "keyboard_page_down",
    "keyboard_shift", "keyboard_ctrl", "keyboard_alt",
    "mouse_left", "mouse_right",
    "keyboard_arrows",
]


def write_dds(img, path):
    img = img.convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
    w, h = img.size
    header = b"DDS " + struct.pack(
        "<7I44x",
        124,                        # dwSize
        0x1 | 0x2 | 0x4 | 0x1000,   # CAPS|HEIGHT|WIDTH|PIXELFORMAT
        h, w,
        0,                          # pitch (unused)
        0,                          # depth
        1,                          # mip count
    )
    pf = struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    caps = struct.pack("<5I", 0x1000, 0, 0, 0, 0)  # CAPS, CAPS2-4, reserved2
    with open(path, "wb") as f:
        f.write(header + pf + caps + img.tobytes())


def make_icon(name, width=SIZE, height=SIZE):
    """Scale the icon to the game's inset for a texture of this size."""
    scale = min(width, height) / SIZE
    size = max(1, int(round(ICON_SIZE * scale)))
    icon = Image.open(os.path.join(ICONS_DIR, name)).convert("RGBA")
    icon = icon.resize((size, size), Image.LANCZOS)
    tile = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    tile.alpha_composite(icon, ((width - size) // 2, (height - size) // 2))
    return tile


def main():
    keys = read_prompt_keys()
    for device, icons in DEVICE_ICONS.items():
        d = os.path.join(OUT, device)
        os.makedirs(d, exist_ok=True)
        for action, w, h, hsh, fmt in keys:
            if action not in icons:
                continue  # this device has no icon for that action
            write_dds(make_icon(icons[action], w, h), os.path.join(d, texture_name(w, h, hsh, fmt)))
        print(f"{device}: {len(keys)} textures -> {d}")

    bindings = os.path.join(OUT, "bindings")
    os.makedirs(bindings, exist_ok=True)
    padCount = 0
    for device, icons in PAD_ICONS.items():
        for key, icon in icons.items():
            write_dds(make_icon(icon), os.path.join(bindings, f"{device}_{key}.dds"))
            padCount += 1
    missing = []
    for stem in BINDING_ICONS:
        src = os.path.join(ICONS_DIR, stem + ".png")
        if not os.path.exists(src):
            missing.append(stem)
            continue
        write_dds(make_icon(stem + ".png"), os.path.join(bindings, stem + ".dds"))
    print(f"bindings: {len(BINDING_ICONS) - len(missing)} key icons, {padCount} pad icons -> {bindings}")
    if missing:
        print("  missing from tools/prompt_icons: " + ", ".join(missing))


if __name__ == "__main__":
    main()
