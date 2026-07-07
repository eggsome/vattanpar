# crateblast

Top-down 2D shooter for Linux/Wayland. No engine, no toolkit, no sprites —
a raw `wayland-client` connection, software rendering into shared-memory
buffers, and vector boxes all the way down.

Drive around a 5760x3240 map (3x a 1080p screen in each axis) and blast
crates. Crates take three hits, get knocked around by bullets and by you,
bounce off walls, and burst into fragments. Score is top-left, minimap is
top-right.

## Controls

| Input       | Action                    |
|-------------|---------------------------|
| W A S D     | Move (with a bit of momentum) |
| Mouse       | Aim                       |
| Left button | Shoot (hold for auto-fire) |
| Esc         | Quit                      |

## Terrain

- **Grass** — baseline speed and friction
- **Sand** — slow, grippy
- **Water** — slowest
- **Ice** — fast and very slippery; crates slide forever on it

## Build & run

Needs `wayland-client` dev headers, `wayland-protocols`, and
`wayland-scanner` (Debian/Ubuntu: `libwayland-dev wayland-protocols`).

    make run

The window is a fixed 1920x1080 buffer (v1 targets 1080p only).

## Map format

`map.txt` is a plain-text vector format; see the comments at the top of the
file. Lines:

    size    <w> <h>
    spawn   <x> <y>
    wall    <x> <y> <w> <h>                    axis-aligned rectangle wall
    terrain <type> <x> <y> <w> <h>             rectangle terrain patch
    poly    <wall|type> <x1> <y1> <x2> <y2> ...   arbitrary polygon (3-32 pts)
    crate   <x> <y>

`<type>` is one of `grass`, `sand`, `water`, `ice`. Later terrain lines paint
over earlier ones. Pass a different map as `./crateblast mymap.txt`.

## Map editor

`make edit` (or `python3 editor.py [map.txt]`) opens a GTK4 editor
(needs `python3-gi`, preinstalled on Ubuntu GNOME).

- Pick **Wall** or a terrain type, then left-click to drop vertices;
  finish the polygon with right-click, Enter, or double-click (needs at
  least 3 points); Esc cancels.
- **Crate** / **Spawn** place on click.
- **Select**: click a shape to select it, drag to move it, drag a corner
  handle to reshape; Delete removes it. Click empty space to deselect.
- Middle-drag pans, scroll zooms around the cursor, **Snap** toggles the
  20 px grid snap.
- Ctrl+S / **Save** writes the file (axis-aligned rectangles are saved in
  the compact `wall`/`terrain` syntax, everything else as `poly`).
  Ctrl+Z / **Undo** steps back; unsaved changes show a `*` in the title.
  Comments in a hand-written map file are not preserved on save.
