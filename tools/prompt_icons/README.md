# Prompt icons

Vendored icons used by `tools/make_prompt_glyphs.py` to build the per-device
button-prompt replacements in `textures/`.

They come from Kenney's **Input Prompts** pack:
https://kenney.nl/assets/input-prompts

License: Creative Commons CC0 1.0 (public domain). The pack may be used in any
project, commercial or not, with no attribution required; credit is given here
anyway. The pack ships a `License.txt` alongside the download.

Only the icons the port needs are copied here:

| File | Used for |
| --- | --- |
| `xbox_button_color_a.png`, `xbox_button_color_b.png` | Xbox A / B prompts |
| `playstation_button_color_cross.png`, `playstation_button_color_circle.png` | PlayStation Cross / Circle prompts |
| `switch_button_a.png`, `switch_button_b.png` | Switch A / B prompts |
| `keyboard_x.png`, `keyboard_z.png` | keyboard prompts, matching the port's default A / B keys |

The prompt texture names they are written as (`tex1_32x32_..._5.dds`) identify
the game's own GameCube A and B glyph textures, which is why these two actions
are the ones covered so far.
