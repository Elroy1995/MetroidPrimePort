# Prompt icons

Vendored icons used by `tools/make_prompt_glyphs.py` to build the per-device
button-prompt replacements in `textures/`.

They come from Kenney's **Input Prompts** pack:
https://kenney.nl/assets/input-prompts

License: Creative Commons CC0 1.0 (public domain). The pack may be used in any
project, commercial or not, with no attribution required; credit is given here
anyway. The pack ships a `License.txt` alongside the download.

Only the icons the port needs are copied here.

The device icons label the A, B, L and R prompts for each controller family.
The keyboard set doubles as the fallback used when a binding has no icon of its
own, so it labels the port's default keys:

| File | Used for |
| --- | --- |
| `xbox_button_color_a.png`, `xbox_button_color_b.png` | Xbox A / B prompts |
| `xbox_lb.png`, `xbox_rb.png` | Xbox LB / RB shoulder prompts |
| `playstation_button_color_cross.png`, `playstation_button_color_circle.png` | PlayStation Cross / Circle prompts |
| `playstation_trigger_l1.png`, `playstation_trigger_r1.png` | PlayStation L1 / R1 shoulder prompts |
| `switch_button_a.png`, `switch_button_b.png` | Switch A / B prompts |
| `switch_button_l.png`, `switch_button_r.png` | Switch L / R shoulder prompts |
| `keyboard_x.png`, `keyboard_z.png` | keyboard A / B, matching the default keys |
| `keyboard_q.png`, `keyboard_e.png` | keyboard L / R, matching the default keys |
| `keyboard_f.png` | keyboard Z, matching the default key |
| `keyboard_arrows.png` | keyboard stick prompts (the sticks are bound to key sets) |
| `xbox_stick_r.png`, `playstation_stick_r.png`, `switch_stick_r.png` | pad stick prompts |

Which game textures they are written as comes from the prompt table in
`platform/port_prompts.cpp`, which `tools/make_prompt_glyphs.py` parses, so a
newly identified prompt texture only has to be added there.

The rest are keyboard and mouse icons for the inputs that can be bound. They are
written to `<textures>/bindings/<stem>.dds` and served per binding, so the prompt
shows whatever key or mouse button is actually bound to the action. Stems match
the tables in `platform/port_prompts.cpp`: `keyboard_a`–`keyboard_z`,
`keyboard_0`–`keyboard_9`, the arrows, `keyboard_f1`–`keyboard_f12`, the named
keys (space, enter, escape, tab, backspace, delete, insert, home, end, page up,
page down, shift, ctrl, alt) and `mouse_left` / `mouse_right`.

The prompt texture names these are written as (`tex1_32x32_..._5.dds`) identify
the game's own GameCube A and B glyph textures, which is why these two actions
are the ones covered so far.
