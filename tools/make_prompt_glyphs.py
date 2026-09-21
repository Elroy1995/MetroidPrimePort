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
import struct
import sys
from PIL import Image

SIZE = 32
ICON_SIZE = 22
INSET = (SIZE - ICON_SIZE) // 2
ICONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompt_icons")
OUT = sys.argv[1] if len(sys.argv) > 1 else "textures"

# The two prompt textures to replace, as they are named in a dump:
#   32x32 RGB5A3 ("_5"), GameCube A and B.
TARGETS = {
    "a": "tex1_32x32_bb21e8755f36b2f0_5.dds",
    "b": "tex1_32x32_e6dbfd18d4666ee7_5.dds",
}

SETS = {
    "xbox": {"a": "xbox_button_color_a.png", "b": "xbox_button_color_b.png"},
    "playstation": {"a": "playstation_button_color_cross.png",
                    "b": "playstation_button_color_circle.png"},
    "switch": {"a": "switch_button_a.png", "b": "switch_button_b.png"},
    "keyboard": {"a": "keyboard_x.png", "b": "keyboard_z.png"},
}

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


def make_icon(name):
    icon = Image.open(os.path.join(ICONS_DIR, name)).convert("RGBA")
    icon = icon.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    tile = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    tile.alpha_composite(icon, (INSET, INSET))
    return tile


def main():
    for device, icons in SETS.items():
        d = os.path.join(OUT, device)
        os.makedirs(d, exist_ok=True)
        for action, name in TARGETS.items():
            write_dds(make_icon(icons[action]), os.path.join(d, name))
        print(f"{device}: {len(icons)} icons -> {d}")

    bindings = os.path.join(OUT, "bindings")
    os.makedirs(bindings, exist_ok=True)
    missing = []
    for stem in BINDING_ICONS:
        src = os.path.join(ICONS_DIR, stem + ".png")
        if not os.path.exists(src):
            missing.append(stem)
            continue
        write_dds(make_icon(stem + ".png"), os.path.join(bindings, stem + ".dds"))
    print(f"bindings: {len(BINDING_ICONS) - len(missing)} icons -> {bindings}")
    if missing:
        print("  missing from tools/prompt_icons: " + ", ".join(missing))


if __name__ == "__main__":
    main()
