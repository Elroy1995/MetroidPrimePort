#!/usr/bin/env python3
"""Generate per-device button-prompt replacements for Metroid Prime.

The game's prompts are individual 32x32 RGB5A3 textures holding a coloured
button icon (GameCube A teal, B red). Each device gets its own icon for the
same two actions, so the in-game prompt matches the pad (or keyboard) in use.

Art is drawn here rather than copied from a pack, so it carries no third-party
licence. The icon occupies the same inset as the game's own art (a ~21px circle
centred in the 32px tile, transparent margin); drawing it full-bleed makes the
prompt read as a solid square, since the glyph is only about 22px on screen.

Output is an uncompressed 32-bit RGBA DDS. DDS rather than PNG because the
game's texture is RGB5A3 and Aurora's DDS path keeps the alpha; the vertical
flip matches how Aurora reads and writes DDS (the dumps come out flipped).
"""
import os
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

SIZE = 32
OUT = sys.argv[1] if len(sys.argv) > 1 else "textures"

# The two prompt textures to replace, as they are named in a dump:
#   32x32 RGB5A3 ("_5"), GameCube A and B.
TARGETS = {
    "a": "tex1_32x32_bb21e8755f36b2f0_5.dds",
    "b": "tex1_32x32_e6dbfd18d4666ee7_5.dds",
}

# Matches the game's own glyph, which is opaque only from (6,5) to (27,26).
CIRCLE = (5, 5, 27, 27)
KEYCAP = (4, 6, 28, 27)


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


def font(size):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def shade(img, box, top, bottom):
    """Vertical gradient clipped to the ellipse in `box`."""
    grad = Image.new("RGBA", (1, SIZE), (0, 0, 0, 0))
    for y in range(SIZE):
        t = y / (SIZE - 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    grad = grad.resize((SIZE, SIZE))
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    img.paste(grad, (0, 0), mask)


def centred_text(d, label, f, fill, cx, cy):
    bb = d.textbbox((0, 0), label, font=f)
    d.text((cx - (bb[2] - bb[0]) / 2 - bb[0], cy - (bb[3] - bb[1]) / 2 - bb[1]),
           label, font=f, fill=fill)


def circle_button(label, top, bottom, text_fill, text_size):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    shade(img, CIRCLE, top, bottom)
    d = ImageDraw.Draw(img)
    d.ellipse(CIRCLE, outline=(255, 255, 255, 110), width=1)
    centred_text(d, label, font(text_size), text_fill, SIZE / 2, SIZE / 2 + 1)
    return img


def symbol_button(shape, top, bottom, fill):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    shade(img, CIRCLE, top, bottom)
    d = ImageDraw.Draw(img)
    d.ellipse(CIRCLE, outline=(255, 255, 255, 110), width=1)
    cx = cy = SIZE / 2
    r = 5
    if shape == "cross":
        d.line((cx - r, cy - r, cx + r, cy + r), fill=fill, width=3)
        d.line((cx - r, cy + r, cx + r, cy - r), fill=fill, width=3)
    else:
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=fill, width=2)
    return img


def keycap(label, text_size):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(KEYCAP, radius=3, fill=(232, 232, 232, 255))
    d.rounded_rectangle((KEYCAP[0], KEYCAP[1], KEYCAP[2], KEYCAP[3] - 2), radius=3,
                        fill=(250, 250, 250, 255))
    d.rounded_rectangle(KEYCAP, radius=3, outline=(110, 110, 110, 255), width=1)
    centred_text(d, label, font(text_size), (40, 40, 40, 255), SIZE / 2, SIZE / 2)
    return img


GREEN = (16, 124, 16, 255)
RED = (211, 47, 47, 255)
BLUE = (46, 107, 230, 255)
DARK = (32, 32, 32, 255)
WHITE = (255, 255, 255, 255)

# Xbox A is the bottom button (green), B the right one (red).
XBOX = {"a": circle_button("A", (150, 205, 150, 255), GREEN, WHITE, 15),
        "b": circle_button("B", (235, 130, 130, 255), RED, WHITE, 15)}
# PlayStation: the A action is Cross (blue), B is Circle (red).
PS = {"a": symbol_button("cross", (140, 190, 250, 255), BLUE, WHITE),
      "b": symbol_button("circle", (245, 140, 130, 255), RED, WHITE)}
# Switch Pro letters; keep them neutral.
SWITCH = {"a": circle_button("A", (225, 225, 225, 255), (150, 150, 150, 255), DARK, 15),
          "b": circle_button("B", (225, 225, 225, 255), (150, 150, 150, 255), DARK, 15)}
# Keyboard: the port's defaults bind the A action to X and B to Z.
KEYBOARD = {"a": keycap("X", 13), "b": keycap("Z", 13)}

SETS = {"xbox": XBOX, "playstation": PS, "switch": SWITCH, "keyboard": KEYBOARD}


def main():
    for device, icons in SETS.items():
        d = os.path.join(OUT, device)
        os.makedirs(d, exist_ok=True)
        for action, name in TARGETS.items():
            write_dds(icons[action], os.path.join(d, name))
        print(f"{device}: {len(icons)} icons -> {d}")


if __name__ == "__main__":
    main()
