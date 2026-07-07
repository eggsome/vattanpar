# crateblast

Top-down 2D shooter for Linux/Wayland. No engine, no toolkit, no sprites —
a raw `wayland-client` connection, software rendering into shared-memory
buffers, and vector shapes all the way down.

Drive around a 5760x3240 map (3x a 1080p screen in each axis) and blast
barrels. Barrels take three hits, get knocked around by bullets and by you,
bounce off walls, and burst into fragments. Score is top-left, minimap is
top-right.

## Controls

| Input       | Action                    |
|-------------|---------------------------|
| W A S D     | Move (with a bit of momentum) |
| SPACE       | Jump                      |
| Mouse       | Aim                       |
| Left button | Shoot (hold for auto-fire) |
| Esc         | Quit                      |

## Wall heights & jumping

Walls come in three heights — **low**, **medium**, **high** — drawn lighter
the taller they are. Jumping (SPACE) lets you climb exactly one step:
ground → low → medium → high. Anything more than one step above you stays
solid even mid-jump. You can jump down any distance, but walking off an
edge is not possible — changing height always takes a jump. While airborne
a small black shadow circle marks your position on the ground and shrinks
the higher you are.

Bullets fly at the height you fired from and pass over lower walls, so the
high ground is worth taking.

## Terrain

- **Grass** — baseline speed and friction
- **Sand** — slow, grippy
- **Water** — slowest
- **Ice** — fast and very slippery; barrels slide forever on it

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
    wall    <height> <x> <y> <w> <h>          axis-aligned rectangle wall
    terrain <type> <x> <y> <w> <h>            rectangle terrain patch
    poly    <height|type> <x1> <y1> <x2> <y2> ...  arbitrary polygon (3-32 pts)
    crate   <x> <y>                           a barrel (center)

`<height>` is `low`, `medium`, or `high`; `<type>` is `grass`, `sand`,
`water`, or `ice`. Legacy `wall <x> <y> <w> <h>` and `poly wall ...` lines
(no height keyword) load as `high`. Later terrain lines paint over earlier
ones. Pass a different map as `./crateblast mymap.txt`.

## Map editor

`make edit` (or `python3 editor.py [map.txt]`) opens a GTK4 editor
(needs `python3-gi`, preinstalled on Ubuntu GNOME).

- Pick a wall height (**Low**/**Medium**/**High**) or a terrain type, then
  left-click to drop vertices; finish the polygon with right-click, Enter,
  or double-click (needs at least 3 points); Esc cancels.
- **Barrel** / **Spawn** place on click.
- **Select**: click a shape to select it, drag to move it, drag a corner
  handle to reshape; Delete removes it. Click empty space to deselect.
  The status bar shows the height of a selected wall.
- Middle-drag pans, scroll zooms around the cursor, **Snap** toggles the
  20 px grid snap.
- Ctrl+S / **Save** writes the file (axis-aligned rectangles are saved in
  the compact `wall`/`terrain` syntax, everything else as `poly`).
  Ctrl+Z / **Undo** steps back; unsaved changes show a `*` in the title.
  Comments in a hand-written map file are not preserved on save.
