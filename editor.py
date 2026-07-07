#!/usr/bin/env python3
"""crateblast map editor.

GUI editor for the crateblast map format. Draw polygonal walls (three
heights: low/medium/high) and terrain, place barrels and the spawn point,
save back to the same plain-text format the game reads.

Usage:  python3 editor.py [map.txt]

Controls:
  Tool buttons     select / draw wall (by height) / draw terrain / barrel / spawn
  Left click       add polygon vertex (draw tools), select or drag (select)
  Right click / Enter / double click   finish the polygon (needs >= 3 points)
  Esc              cancel polygon in progress, or clear selection
  Delete           delete the selected shape or crate
  Middle drag      pan
  Scroll           zoom (around the cursor)
  Ctrl+S           save
  Ctrl+Z           undo
"""
import sys
import copy
import math
import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk  # noqa: E402

TERRAIN_KINDS = ["grass", "sand", "water", "ice"]
WALL_KINDS = ["low", "medium", "high"]
KIND_COLORS = {  # matches src/draw.c
    "grass":  (0x3B / 255, 0x5E / 255, 0x3B / 255),
    "sand":   (0xC0 / 255, 0xA4 / 255, 0x5E / 255),
    "water":  (0x2A / 255, 0x5E / 255, 0x86 / 255),
    "ice":    (0xB9 / 255, 0xD4 / 255, 0xDE / 255),
    "low":    (0x2E / 255, 0x32 / 255, 0x3C / 255),
    "medium": (0x3D / 255, 0x43 / 255, 0x51 / 255),
    "high":   (0x4E / 255, 0x55 / 255, 0x68 / 255),
}
WALL_EDGE_COLORS = {
    "low":    (0x44 / 255, 0x4A / 255, 0x58 / 255),
    "medium": (0x56 / 255, 0x5E / 255, 0x70 / 255),
    "high":   (0x6A / 255, 0x72 / 255, 0x88 / 255),
}
BARREL_COLOR = (0xB4 / 255, 0x71 / 255, 0x2F / 255)
GRID = 160
SNAP = 20
BARREL_R = 21
PLAYER_R = 18
MAX_POLY_PTS = 32
MAX_UNDO = 100

# ------------------------------------------------------------------ model
# map dict: size (w,h), spawn (x,y), terrains [[kind, [(x,y),...]], ...],
#           walls [[kind, [(x,y),...]], ...] with kind low/medium/high,
#           crates [(x,y), ...] (barrels; the file keyword stays "crate")


def new_map():
    return {"size": (5760.0, 3240.0), "spawn": (2880.0, 1620.0),
            "terrains": [], "walls": [], "crates": []}


def rect_pts(x, y, w, h):
    return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]


def as_axis_rect(pts):
    """Return (x, y, w, h) if pts is an axis-aligned rectangle, else None."""
    if len(pts) != 4:
        return None
    xs, ys = {p[0] for p in pts}, {p[1] for p in pts}
    if len(xs) != 2 or len(ys) != 2:
        return None
    for i in range(4):
        (x1, y1), (x2, y2) = pts[i], pts[(i + 1) % 4]
        if x1 != x2 and y1 != y2:
            return None
    return (min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))


def load_map(path):
    m = new_map()
    with open(path) as f:
        for line in f:
            tok = line.split()
            if not tok or tok[0].startswith("#"):
                continue
            try:
                if tok[0] == "size":
                    m["size"] = (float(tok[1]), float(tok[2]))
                elif tok[0] == "spawn":
                    m["spawn"] = (float(tok[1]), float(tok[2]))
                elif tok[0] == "wall":
                    if tok[1] in WALL_KINDS:
                        m["walls"].append(
                            [tok[1], rect_pts(*map(float, tok[2:6]))])
                    else:  # legacy: no height keyword
                        m["walls"].append(
                            ["high", rect_pts(*map(float, tok[1:5]))])
                elif tok[0] == "terrain":
                    m["terrains"].append(
                        [tok[1], rect_pts(*map(float, tok[2:6]))])
                elif tok[0] == "poly":
                    vals = list(map(float, tok[2:]))
                    pts = list(zip(vals[0::2], vals[1::2]))
                    if len(pts) >= 3:
                        if tok[1] in WALL_KINDS:
                            m["walls"].append([tok[1], pts])
                        elif tok[1] == "wall":  # legacy
                            m["walls"].append(["high", pts])
                        else:
                            m["terrains"].append([tok[1], pts])
                elif tok[0] in ("crate", "barrel"):
                    m["crates"].append((float(tok[1]), float(tok[2])))
            except (ValueError, IndexError):
                print(f"editor: skipping bad line: {line.rstrip()}",
                      file=sys.stderr)
    return m


def fmt(v):
    return f"{v:g}"


def shape_line(kind, pts):
    r = as_axis_rect(pts)
    if r is not None:
        head = f"wall {kind}" if kind in WALL_KINDS else f"terrain {kind}"
        return f"{head} {' '.join(fmt(v) for v in r)}"
    coords = " ".join(f"{fmt(x)} {fmt(y)}" for x, y in pts)
    return f"poly {kind} {coords}"


def save_map(path, m):
    out = ["# crateblast map (written by editor.py)",
           f"size {fmt(m['size'][0])} {fmt(m['size'][1])}",
           f"spawn {fmt(m['spawn'][0])} {fmt(m['spawn'][1])}", ""]
    out += [shape_line(kind, pts) for kind, pts in m["terrains"]]
    out += [shape_line(kind, pts) for kind, pts in m["walls"]]
    out.append("")
    out += [f"crate {fmt(x)} {fmt(y)}" for x, y in m["crates"]]
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def point_in_poly(pts, x, y):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < xi + (y - yi) / (yj - yi) * (xj - xi):
            inside = not inside
        j = i
    return inside


# ----------------------------------------------------------------- editor


class EditorWindow(Gtk.ApplicationWindow):
    def __init__(self, app, path):
        super().__init__(application=app)
        self.path = path
        try:
            self.map = load_map(path)
        except FileNotFoundError:
            self.map = new_map()
        self.tool = "select"
        self.zoom = 0.25
        self.view_x = self.view_y = 0.0
        self.fitted = False
        self.draft = []          # in-progress polygon, world coords
        self.sel = None          # ("wall"|"terrain"|"crate", index)
        self.drag = None
        self.button1_down = False
        self.mouse = (0.0, 0.0)  # screen coords
        self.snap_on = True
        self.undo_stack = []
        self.modified = False

        self.set_default_size(1500, 950)
        self.build_ui()
        self.update_title()

    # ------------------------------------------------------------- UI

    def build_ui(self):
        hb = Gtk.HeaderBar()
        self.set_titlebar(hb)

        first = None
        self.tool_buttons = {}
        for name in (["select"] + WALL_KINDS + TERRAIN_KINDS
                     + ["crate", "spawn"]):
            label = "Barrel" if name == "crate" else name.capitalize()
            b = Gtk.ToggleButton(label=label)
            if first is None:
                first = b
                b.set_active(True)
            else:
                b.set_group(first)
            b.connect("toggled", self.on_tool, name)
            self.tool_buttons[name] = b
            hb.pack_start(b)

        save = Gtk.Button(label="Save")
        save.connect("clicked", lambda *_: self.save())
        hb.pack_end(save)
        undo = Gtk.Button(label="Undo")
        undo.connect("clicked", lambda *_: self.undo())
        hb.pack_end(undo)
        delete = Gtk.Button(label="Delete")
        delete.connect("clicked", lambda *_: self.delete_selection())
        hb.pack_end(delete)
        snap = Gtk.ToggleButton(label="Snap")
        snap.set_active(True)
        snap.connect("toggled",
                     lambda b: setattr(self, "snap_on", b.get_active()))
        hb.pack_end(snap)

        self.da = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.da.set_draw_func(self.on_draw)

        self.status = Gtk.Label(xalign=0, margin_start=8, margin_top=2,
                                margin_bottom=2)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.append(self.da)
        box.append(self.status)
        self.set_child(box)

        click = Gtk.GestureClick(button=1)
        click.connect("pressed", self.on_press)
        click.connect("released", self.on_release)
        self.da.add_controller(click)

        rclick = Gtk.GestureClick(button=3)
        rclick.connect("pressed", self.on_right_press)
        self.da.add_controller(rclick)

        pan = Gtk.GestureDrag(button=2)
        pan.connect("drag-begin", self.on_pan_begin)
        pan.connect("drag-update", self.on_pan_update)
        self.da.add_controller(pan)

        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self.on_motion)
        self.da.add_controller(motion)

        scroll = Gtk.EventControllerScroll(
            flags=Gtk.EventControllerScrollFlags.VERTICAL)
        scroll.connect("scroll", self.on_scroll)
        self.da.add_controller(scroll)

        keys = Gtk.EventControllerKey()
        keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        keys.connect("key-pressed", self.on_key)
        self.add_controller(keys)

    def update_title(self):
        star = "*" if self.modified else ""
        self.set_title(f"{star}{self.path} — crateblast editor")

    def update_status(self):
        wx, wy = self.s2w(*self.mouse)
        m = self.map
        extra = ""
        if self.draft:
            extra = f" | drawing {self.tool}: {len(self.draft)} pts" \
                    " (right-click/Enter to finish, Esc to cancel)"
        elif self.sel:
            kind, i = self.sel
            detail = f" ({m['walls'][i][0]})" if kind == "wall" else ""
            extra = f" | selected {kind} #{i}{detail}"
        self.status.set_text(
            f"{self.tool} | {wx:.0f},{wy:.0f} | zoom {self.zoom * 100:.0f}%"
            f" | map {m['size'][0]:g}x{m['size'][1]:g}"
            f" | {len(m['walls'])} walls, {len(m['terrains'])} terrain,"
            f" {len(m['crates'])} barrels{extra}")

    # ------------------------------------------------------- transforms

    def w2s(self, x, y):
        return ((x - self.view_x) * self.zoom, (y - self.view_y) * self.zoom)

    def s2w(self, sx, sy):
        return (sx / self.zoom + self.view_x, sy / self.zoom + self.view_y)

    def snap(self, x, y):
        if self.snap_on:
            return (round(x / SNAP) * SNAP, round(y / SNAP) * SNAP)
        return (x, y)

    def fit_view(self, w, h):
        mw, mh = self.map["size"]
        self.zoom = min(w / mw, h / mh) * 0.95
        self.view_x = mw / 2 - w / self.zoom / 2
        self.view_y = mh / 2 - h / self.zoom / 2

    # ------------------------------------------------------------ edits

    def push_undo(self):
        self.undo_stack.append(copy.deepcopy(self.map))
        del self.undo_stack[:-MAX_UNDO]

    def undo(self):
        if self.undo_stack:
            self.map = self.undo_stack.pop()
            self.sel = None
            self.drag = None
            self.modified = True
            self.update_title()
            self.da.queue_draw()

    def save(self):
        save_map(self.path, self.map)
        self.modified = False
        self.update_title()

    def touch(self):
        self.modified = True
        self.update_title()

    def delete_selection(self):
        if not self.sel:
            return
        kind, i = self.sel
        self.push_undo()
        if kind == "wall":
            del self.map["walls"][i]
        elif kind == "terrain":
            del self.map["terrains"][i]
        elif kind == "crate":
            del self.map["crates"][i]
        self.sel = None
        self.touch()
        self.da.queue_draw()

    def finish_draft(self):
        if len(self.draft) >= 3:
            self.push_undo()
            if self.tool in WALL_KINDS:
                self.map["walls"].append([self.tool, self.draft])
                self.sel = ("wall", len(self.map["walls"]) - 1)
            else:
                self.map["terrains"].append([self.tool, self.draft])
                self.sel = ("terrain", len(self.map["terrains"]) - 1)
            self.touch()
        self.draft = []
        self.da.queue_draw()

    def sel_pts(self):
        """Points list of the selected polygon, or None."""
        if not self.sel:
            return None
        kind, i = self.sel
        if kind == "wall":
            return self.map["walls"][i][1]
        if kind == "terrain":
            return self.map["terrains"][i][1]
        return None

    def set_sel_pts(self, pts):
        kind, i = self.sel
        if kind == "wall":
            self.map["walls"][i][1] = pts
        else:
            self.map["terrains"][i][1] = pts

    # ------------------------------------------------------------ input

    def on_tool(self, button, name):
        if button.get_active():
            self.tool = name
            self.draft = []
            if name != "select":
                self.sel = None
            cursor = "default" if name == "select" else "crosshair"
            self.da.set_cursor(Gdk.Cursor.new_from_name(cursor))
            self.update_status()
            self.da.queue_draw()

    def on_press(self, gesture, n_press, x, y):
        self.button1_down = True
        self.mouse = (x, y)
        if self.tool == "select":
            self.select_press(x, y)
        elif self.tool in ("crate", "spawn"):
            self.push_undo()
            wx, wy = self.snap(*self.s2w(x, y))
            if self.tool == "crate":
                self.map["crates"].append((wx, wy))
            else:
                self.map["spawn"] = (wx, wy)
            self.touch()
        else:  # polygon draw tools
            if n_press >= 2:
                self.finish_draft()
            elif len(self.draft) >= MAX_POLY_PTS:
                self.finish_draft()
            else:
                self.draft.append(self.snap(*self.s2w(x, y)))
        self.update_status()
        self.da.queue_draw()

    def on_release(self, gesture, n_press, x, y):
        self.button1_down = False
        if self.drag:
            if not self.drag["moved"]:
                self.undo_stack.pop()  # selection click, nothing moved
            self.drag = None

    def on_right_press(self, gesture, n_press, x, y):
        if self.draft:
            self.finish_draft()
        else:
            self.sel = None
            self.da.queue_draw()
        self.update_status()

    def select_press(self, x, y):
        wx, wy = self.s2w(x, y)
        handle = 8 / self.zoom

        # vertex handle of the selected polygon?
        pts = self.sel_pts()
        if pts:
            for vi, (vx, vy) in enumerate(pts):
                if abs(vx - wx) < handle and abs(vy - wy) < handle:
                    self.push_undo()
                    self.drag = {"what": "vertex", "vi": vi,
                                 "start": (wx, wy), "orig": list(pts),
                                 "moved": False}
                    return

        def start_move(sel, orig):
            self.sel = sel
            self.push_undo()
            self.drag = {"what": "move", "start": (wx, wy), "orig": orig,
                         "moved": False}

        for i in reversed(range(len(self.map["crates"]))):
            cx, cy = self.map["crates"][i]
            if (cx - wx) ** 2 + (cy - wy) ** 2 < BARREL_R ** 2:
                start_move(("crate", i), self.map["crates"][i])
                return
        sx, sy = self.map["spawn"]
        if (sx - wx) ** 2 + (sy - wy) ** 2 < PLAYER_R ** 2:
            start_move(("spawn", 0), self.map["spawn"])
            return
        for i in reversed(range(len(self.map["walls"]))):
            if point_in_poly(self.map["walls"][i][1], wx, wy):
                start_move(("wall", i), list(self.map["walls"][i][1]))
                return
        for i in reversed(range(len(self.map["terrains"]))):
            if point_in_poly(self.map["terrains"][i][1], wx, wy):
                start_move(("terrain", i), list(self.map["terrains"][i][1]))
                return
        self.sel = None

    def on_motion(self, ctrl, x, y):
        self.mouse = (x, y)
        if self.drag and self.button1_down:
            wx, wy = self.s2w(x, y)
            d = self.drag
            d["moved"] = True
            if d["what"] == "vertex":
                pts = list(d["orig"])
                pts[d["vi"]] = self.snap(wx, wy)
                self.set_sel_pts(pts)
            else:
                dx, dy = wx - d["start"][0], wy - d["start"][1]
                if self.snap_on:
                    dx = round(dx / SNAP) * SNAP
                    dy = round(dy / SNAP) * SNAP
                kind, i = self.sel
                if kind == "crate":
                    ox, oy = d["orig"]
                    self.map["crates"][i] = (ox + dx, oy + dy)
                elif kind == "spawn":
                    ox, oy = d["orig"]
                    self.map["spawn"] = (ox + dx, oy + dy)
                else:
                    self.set_sel_pts([(px + dx, py + dy)
                                      for px, py in d["orig"]])
            self.touch()
        self.update_status()
        self.da.queue_draw()

    def on_pan_begin(self, gesture, x, y):
        self.pan_origin = (self.view_x, self.view_y)

    def on_pan_update(self, gesture, dx, dy):
        self.view_x = self.pan_origin[0] - dx / self.zoom
        self.view_y = self.pan_origin[1] - dy / self.zoom
        self.da.queue_draw()

    def on_scroll(self, ctrl, dx, dy):
        wx, wy = self.s2w(*self.mouse)
        self.zoom = max(0.02, min(8.0, self.zoom * (1.2 ** -dy)))
        self.view_x = wx - self.mouse[0] / self.zoom
        self.view_y = wy - self.mouse[1] / self.zoom
        self.update_status()
        self.da.queue_draw()
        return True

    def on_key(self, ctrl, keyval, keycode, state):
        ctrl_held = state & Gdk.ModifierType.CONTROL_MASK
        if ctrl_held and keyval == Gdk.KEY_s:
            self.save()
        elif ctrl_held and keyval == Gdk.KEY_z:
            self.undo()
        elif keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            self.finish_draft()
        elif keyval == Gdk.KEY_Escape:
            if self.draft:
                self.draft = []
            else:
                self.sel = None
            self.da.queue_draw()
        elif keyval in (Gdk.KEY_Delete, Gdk.KEY_BackSpace):
            self.delete_selection()
        else:
            return False
        self.update_status()
        return True

    # ------------------------------------------------------------- draw

    def poly_path(self, cr, pts):
        cr.move_to(*self.w2s(*pts[0]))
        for p in pts[1:]:
            cr.line_to(*self.w2s(*p))
        cr.close_path()

    def on_draw(self, area, cr, w, h):
        if not self.fitted:
            self.fit_view(w, h)
            self.fitted = True
            self.update_status()

        m = self.map
        mw, mh = m["size"]
        cr.set_source_rgb(0.05, 0.05, 0.07)
        cr.paint()

        ox, oy = self.w2s(0, 0)
        cr.rectangle(ox, oy, mw * self.zoom, mh * self.zoom)
        cr.set_source_rgb(*KIND_COLORS["grass"])
        cr.fill()

        for kind, pts in m["terrains"]:
            self.poly_path(cr, pts)
            cr.set_source_rgb(*KIND_COLORS[kind])
            cr.fill()

        if self.zoom * GRID >= 14:
            cr.set_source_rgba(0, 0, 0, 0.15)
            cr.set_line_width(1)
            gx = 0
            while gx <= mw:
                cr.move_to(*self.w2s(gx, 0))
                cr.line_to(*self.w2s(gx, mh))
                gx += GRID
            gy = 0
            while gy <= mh:
                cr.move_to(*self.w2s(0, gy))
                cr.line_to(*self.w2s(mw, gy))
                gy += GRID
            cr.stroke()

        # lowest walls first so taller ones read as being on top
        for kind in WALL_KINDS:
            for wkind, pts in m["walls"]:
                if wkind != kind:
                    continue
                self.poly_path(cr, pts)
                cr.set_source_rgb(*KIND_COLORS[kind])
                cr.fill_preserve()
                cr.set_source_rgb(*WALL_EDGE_COLORS[kind])
                cr.set_line_width(2)
                cr.stroke()

        br = BARREL_R * self.zoom
        for cx, cy in m["crates"]:
            sx, sy = self.w2s(cx, cy)
            cr.arc(sx, sy, br, 0, 2 * math.pi)
            cr.set_source_rgb(*BARREL_COLOR)
            cr.fill_preserve()
            cr.set_source_rgb(0.36, 0.23, 0.09)
            cr.set_line_width(2)
            cr.stroke()
            cr.arc(sx, sy, br * 0.5, 0, 2 * math.pi)
            cr.set_source_rgb(0.36, 0.23, 0.09)
            cr.set_line_width(1.5)
            cr.stroke()

        sx, sy = self.w2s(*m["spawn"])
        ph = PLAYER_R * self.zoom
        cr.arc(sx, sy, ph, 0, 2 * math.pi)
        cr.set_source_rgb(0.93, 0.94, 0.96)
        cr.fill()
        cr.set_source_rgb(0.05, 0.05, 0.07)
        cr.select_font_face("sans")
        cr.set_font_size(max(10, ph))
        cr.move_to(sx - ph * 0.35, sy + ph * 0.4)
        cr.show_text("S")

        if self.draft:
            color = KIND_COLORS.get(self.tool, (1, 1, 0))
            cr.set_source_rgba(1, 1, 0.4, 0.9)
            cr.set_line_width(2)
            cr.set_dash([6, 4])
            cr.move_to(*self.w2s(*self.draft[0]))
            for p in self.draft[1:]:
                cr.line_to(*self.w2s(*p))
            cr.line_to(*self.w2s(*self.snap(*self.s2w(*self.mouse))))
            cr.stroke()
            cr.set_dash([])
            cr.set_source_rgb(*color)
            for px, py in self.draft:
                dx, dy = self.w2s(px, py)
                cr.rectangle(dx - 3, dy - 3, 6, 6)
            cr.fill()

        pts = self.sel_pts()
        if self.sel and self.sel[0] == "crate":
            cx, cy = m["crates"][self.sel[1]]
            dx, dy = self.w2s(cx, cy)
            cr.arc(dx, dy, br + 4, 0, 2 * math.pi)
            cr.set_source_rgb(1, 1, 1)
            cr.set_line_width(2)
            cr.set_dash([4, 3])
            cr.stroke()
            cr.set_dash([])
        elif pts:
            self.poly_path(cr, pts)
            cr.set_source_rgb(1, 1, 1)
            cr.set_line_width(2)
            cr.set_dash([4, 3])
            cr.stroke()
            cr.set_dash([])
            for px, py in pts:
                dx, dy = self.w2s(px, py)
                cr.rectangle(dx - 4, dy - 4, 8, 8)
                cr.set_source_rgb(1, 1, 1)
                cr.fill_preserve()
                cr.set_source_rgb(0, 0, 0)
                cr.set_line_width(1)
                cr.stroke()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "map.txt"
    app = Gtk.Application(application_id="dev.crateblast.editor")

    def activate(app):
        EditorWindow(app, path).present()

    app.connect("activate", activate)
    app.run(None)


if __name__ == "__main__":
    main()
