#!/usr/bin/env python3
"""Author the editor's icon atlas inside GIMP (run through the GIMP MCP PyGObject console).

Why a script instead of hand-drawn PNGs: every glyph here is geometry, so the *source of truth* for the asset is this
file. A hand-painted atlas cannot be reviewed, re-rendered at a new size, or fixed without opening an image editor —
and "which icons exist, in which cell" would then live only in someone's memory. This produces three things:

  assets/editor/icons/hp_editor_icons.png   the RGBA8 atlas the editor uploads to the GPU (flattened composite)
  assets/editor/icons/hp_editor_icons.xcf   the layered source — one layer per glyph, editable in GIMP
  assets/editor/icons/icon_manifest.json    name -> cell, which tests compare against the C++ table

The last file is what makes the asset and the code unable to drift silently: EditorIcons' lookup table is asserted
against the manifest, so renaming or moving a glyph fails a test instead of drawing the wrong picture.

Run from the repo root with GIMP's MCP server up (see the gimp-mcp-drive skill):

    python3 scripts/run_gimp_script.py scripts/make_editor_icons.py

or paste into the console. The module is also importable: `icon_table()` returns the manifest as data, which
TestEditorIcons reads directly, so the atlas can be checked without GIMP installed.
"""

import json
import math
import os

# --- Atlas geometry -------------------------------------------------------------------------------
#
# 32 px cells on a 512 px canvas: glyphs are authored at 2x the size they are mostly drawn at (16 px in panel rows,
# 24 px in headers), so downscaling is always a reduction. A power-of-two square keeps mip generation sane and lets
# the lookup be arithmetic instead of a per-icon rect table.
CELL = 32
COLS = 16
ROWS = 16
ATLAS = COLS * CELL

OUT_DIR = "assets/editor/icons"
PNG_PATH = os.path.join(OUT_DIR, "hp_editor_icons.png")
XCF_PATH = os.path.join(OUT_DIR, "hp_editor_icons.xcf")
MANIFEST_PATH = os.path.join(OUT_DIR, "icon_manifest.json")

# --- Palette --------------------------------------------------------------------------------------
#
# The editor chrome is a narrow band of neutral greys (see EditorStyle.cpp), so glyphs are a single light value that
# reads against every panel background in that band. Accents are used only where the glyph *is* the information —
# a warning, an error, a play state — never for decoration.
GLYPH = "#C9CDD3"        # primary line colour on dark chrome
DIM = "#8A9099"          # secondary detail, disabled affordances
FILL = "#3E434B"         # body fill: gives a glyph mass without leaving the neutral band
ACCENT = "#57A6E0"       # interaction blue, matches EditorStyle's kAccent family
WARN = "#E0B252"         # lit/sun and warnings
ERROR = "#D05B5B"        # errors, stop
OK = "#6FB98F"           # success-ish (visibility on)

# --- Icon table -----------------------------------------------------------------------------------
#
# Order defines the cells: index i -> column i % COLS, row i // COLS. EditorIcons' C++ table must match this list
# exactly; TestEditorIcons compares it against the emitted manifest, so an insertion in one file and not the other is
# a test failure rather than a mislabelled button.
ICON_ORDER = [
    # Toolbar: file actions
    "new_level",
    "open",
    "save",
    "save_all",
    "save_as",
    "cut",
    "copy",
    "paste",
    "undo",
    "redo",
    "search",
    "settings",
    # Toolbar: play state
    "play",
    "pause",
    "stop",
    "step_frame",
    # Panel headers (UE's names: World Outliner / Details / Content Browser / Output Log)
    "panel_outliner",
    "panel_details",
    "panel_content",
    "panel_output_log",
    "panel_placer",
    # Outliner row types
    "actor_empty",
    "actor_cube",
    "actor_light",
    "actor_camera",
    "actor_script",
    "group_folder",
    # Visibility affordances
    "eye_open",
    "eye_closed",
    # Content browser asset types
    "asset_scene",
    "asset_script",
    "asset_mesh",
    "asset_texture",
    "asset_generic",
    # Viewport toolbar
    "view_lit",
    "view_unlit",
    "view_wireframe",
    "view_show",
    "camera_speed",
    "view_game",
    "view_immersive",
    "view_bookmark",
    # Window / misc
    "window_icon",
    "chevron_down",
    "chevron_right",
    "close_x",
    "reset_arrow",
    "link_broken",
]


def _glyphs():
    """The drawing program for every glyph, keyed by name.

    Primitives are data (`("rect", x, y, w, h, colour)`, `("poly", [pts], colour)`, ...) rather than live calls so the
    same table can drive GIMP and be introspected by a test with no GIMP present. Coordinates are inside one 32x32
    cell; the driver offsets them into the atlas.
    """
    g = {}

    # --- File actions -----------------------------------------------------------------------------
    doc = [(6, 3), (19, 3), (25, 9), (25, 29), (6, 29)]
    fold = [(19, 3), (19, 9), (25, 9)]

    g["new_level"] = [("poly", doc, FILL), ("poly", fold, DIM), ("lines", [(9, 14, 22, 14), (9, 19, 22, 19), (9, 24, 17, 24)], GLYPH, 1.6)]
    g["open"] = [
        ("poly", [(3, 12), (11, 12), (13, 15), (28, 15), (28, 26), (3, 26)], FILL),
        ("poly", [(3, 9), (10, 9), (12, 12), (3, 12)], DIM),
        ("poly", [(6, 18), (25, 18), (23, 26), (3, 26)], GLYPH),
    ]
    g["save"] = [("poly", [(4, 4), (22, 4), (28, 10), (28, 28), (4, 28)], FILL),
                 ("rect", 9, 4, 12, 7, GLYPH), ("rect", 11, 16, 12, 8, DIM), ("rect", 13, 18, 8, 4, GLYPH)]
    g["save_all"] = [("rect", 3, 3, 15, 15, FILL), ("lines", [(6, 6, 14, 6)], GLYPH, 1.4),
                     ("poly", [(9, 9), (27, 9), (27, 27), (9, 27)], FILL),
                     ("rect", 13, 9, 8, 5, GLYPH), ("rect", 14, 18, 9, 6, DIM)]
    g["save_as"] = [("poly", [(4, 4), (22, 4), (28, 10), (28, 28), (4, 28)], FILL),
                    ("rect", 9, 4, 12, 7, GLYPH), ("lines", [(11, 19, 21, 19), (11, 23, 18, 23)], GLYPH, 1.6)]
    g["cut"] = [("ring", 9, 24, 4.0, 2.0, GLYPH), ("ring", 23, 24, 4.0, 2.0, GLYPH),
                ("lines", [(11, 21, 24, 6), (21, 21, 8, 6)], GLYPH, 2.0)]
    g["copy"] = [("rect", 5, 3, 14, 17, DIM), ("rect", 10, 8, 15, 19, FILL),
                 ("lines", [(13, 13, 22, 13), (13, 17, 22, 17), (13, 21, 19, 21)], GLYPH, 1.5)]
    g["paste"] = [("poly", [(6, 6), (26, 6), (26, 29), (6, 29)], FILL),
                  ("rect", 11, 3, 10, 6, DIM), ("rect", 13, 4, 6, 3, GLYPH),
                  ("lines", [(10, 15, 22, 15), (10, 20, 22, 20), (10, 25, 17, 25)], GLYPH, 1.6)]
    g["undo"] = [("arc", 16, 18, 9.5, 200.0, 320.0, 3.2, GLYPH), ("poly", [(7, 10), (14, 12), (9, 18)], GLYPH)]
    g["redo"] = [("arc", 16, 18, 9.5, 220.0, -40.0, 3.2, GLYPH), ("poly", [(25, 10), (18, 12), (23, 18)], GLYPH)]
    g["search"] = [("ring", 14, 14, 8.0, 2.6, GLYPH), ("lines", [(20, 20, 27, 27)], GLYPH, 3.0)]
    g["settings"] = [("gear", 16, 16, 12.0, 8.5, 4.4, GLYPH), ("ring", 16, 16, 4.0, 2.2, FILL)]

    # --- Play state -------------------------------------------------------------------------------
    g["play"] = [("poly", [(9, 5), (27, 16), (9, 27)], GLYPH)]
    g["pause"] = [("rect", 8, 5, 5.5, 22, GLYPH), ("rect", 18, 5, 5.5, 22, GLYPH)]
    g["stop"] = [("rect", 7, 7, 18, 18, ERROR)]
    g["step_frame"] = [("poly", [(6, 6), (20, 16), (6, 26)], GLYPH), ("rect", 23, 6, 3.5, 20, GLYPH)]

    # --- Panel headers ----------------------------------------------------------------------------
    # World Outliner: a hierarchy of nodes — how UE labels the panel and what the tree actually is.
    g["panel_outliner"] = [("lines", [(4, 8, 12, 8), (4, 16, 12, 16), (4, 24, 12, 24)], DIM, 1.6),
                           ("rect", 12, 5, 8, 6, GLYPH), ("rect", 12, 13, 8, 6, GLYPH), ("rect", 12, 21, 8, 6, GLYPH)]
    # Details: the slider stack UE uses for properties.
    g["panel_details"] = [("lines", [(5, 9, 27, 9), (5, 16, 27, 16), (5, 23, 27, 23)], DIM, 2.0),
                          ("ring", 11, 9, 3.4, 1.8, GLYPH), ("ring", 21, 16, 3.4, 1.8, GLYPH), ("ring", 14, 23, 3.4, 1.8, GLYPH)]
    g["panel_content"] = [("poly", [(3, 10), (11, 10), (13, 13), (29, 13), (29, 26), (3, 26)], FILL),
                          ("poly", [(3, 7), (10, 7), (12, 10), (3, 10)], DIM),
                          ("rect", 8, 17, 5, 5, GLYPH), ("rect", 16, 17, 5, 5, GLYPH)]
    g["panel_output_log"] = [("lines", [(5, 8, 26, 8)], DIM, 1.6), ("lines", [(5, 13, 20, 13)], GLYPH, 1.6),
                             ("lines", [(5, 18, 24, 18)], GLYPH, 1.6), ("lines", [(5, 23, 15, 23)], DIM, 1.6),
                             ("dot", 24, 23, 3.0, OK)]
    g["panel_placer"] = [("ring", 16, 16, 11.0, 2.2, GLYPH), ("lines", [(16, 9, 16, 23), (9, 16, 23, 16)], GLYPH, 2.4)]

    # --- Outliner row types -----------------------------------------------------------------------
    g["actor_empty"] = [("ring", 16, 16, 8.0, 1.8, DIM), ("dot", 16, 16, 2.2, DIM)]
    g["actor_cube"] = _cube(GLYPH, FILL)
    # Point light: the bulb + rays UE shows for a Light component.
    g["actor_light"] = [("ring", 15, 17, 6.0, 2.2, WARN), ("rect", 12, 23, 6, 4, DIM),
                        ("lines", [(15, 4, 15, 8), (5, 17, 9, 17), (21, 17, 25, 17), (8, 10, 11, 13), (22, 10, 19, 13)], WARN, 1.8)]
    g["actor_camera"] = [("rect", 3, 11, 16, 13, FILL), ("poly", [(19, 15), (27, 11), (27, 24), (19, 20)], GLYPH),
                         ("dot", 8, 14, 2.0, DIM)]
    g["actor_script"] = [("poly", [(7, 4), (25, 4), (25, 28), (7, 28)], FILL),
                         ("lines", [(11, 11, 21, 11), (11, 16, 18, 16), (11, 21, 20, 21)], ACCENT, 1.8)]
    g["group_folder"] = [("poly", [(3, 10), (11, 10), (13, 13), (29, 13), (29, 26), (3, 26)], GLYPH),
                         ("poly", [(3, 7), (10, 7), (12, 10), (3, 10)], GLYPH)]

    # --- Visibility -------------------------------------------------------------------------------
    g["eye_open"] = [("eye", 16, 16, 11.0, 7.0, GLYPH), ("dot", 16, 16, 3.4, FILL), ("dot", 16, 16, 1.6, GLYPH)]
    g["eye_closed"] = [("eye", 16, 16, 11.0, 4.0, DIM), ("lines", [(6, 25, 26, 7)], DIM, 2.2)]

    # --- Content browser asset types --------------------------------------------------------------
    g["asset_scene"] = _cube(GLYPH, FILL) + [("rect", 4, 24, 24, 3, DIM)]
    g["asset_script"] = g["actor_script"]
    g["asset_mesh"] = [("poly", [(16, 4), (27, 11), (27, 22), (16, 29), (5, 22), (5, 11)], FILL),
                       ("lines", [(16, 4, 16, 29), (5, 11, 27, 22), (27, 11, 5, 22)], GLYPH, 1.4)]
    g["asset_texture"] = [("rect", 4, 6, 24, 20, FILL), ("lines", [(4, 6, 16, 16), (28, 6, 16, 16)], GLYPH, 1.4),
                          ("dot", 11, 12, 2.6, WARN)]
    g["asset_generic"] = [("poly", [(7, 4), (21, 4), (25, 8), (25, 28), (7, 28)], FILL), ("poly", [(21, 4), (21, 8), (25, 8)], DIM)]

    # --- Viewport toolbar -------------------------------------------------------------------------
    g["view_lit"] = [("dot", 16, 16, 5.5, WARN)] + _rays(16, 16, 9.0, 13.0, 2.0, WARN)
    g["view_unlit"] = [("bulb", 16, 14, 7.0, GLYPH), ("rect", 13, 22, 6, 5, DIM), ("lines", [(13, 24, 19, 24)], FILL, 1.2)]
    g["view_wireframe"] = [("poly", [(16, 4), (27, 11), (27, 22), (16, 29), (5, 22), (5, 11)], None),
                           ("lines", [(16, 4, 27, 11), (27, 11, 27, 22), (27, 22, 16, 29), (16, 29, 5, 22),
                                      (5, 22, 5, 11), (5, 11, 16, 4), (16, 4, 16, 29), (5, 11, 27, 22),
                                      (27, 11, 5, 22)], GLYPH, 1.4)]
    g["view_show"] = g["eye_open"]
    g["camera_speed"] = [("ring", 16, 16, 10.0, 2.2, GLYPH), ("lines", [(16, 16, 16, 9)], GLYPH, 2.0),
                         ("lines", [(16, 16, 21, 18)], ACCENT, 2.0)]
    g["view_game"] = [("round_rect", 4, 11, 24, 12, 6.0, FILL), ("dot", 10, 17, 2.4, GLYPH),
                      ("dot", 21, 15, 1.8, GLYPH), ("dot", 24, 18, 1.8, GLYPH)]
    g["view_immersive"] = [("poly", [(3, 16), (10, 9), (10, 13), (22, 13), (22, 9), (29, 16),
                                     (22, 23), (22, 19), (10, 19), (10, 23)], GLYPH)]
    g["view_bookmark"] = [("poly", [(8, 4), (24, 4), (24, 28), (16, 22), (8, 28)], FILL),
                          ("lines", [(8, 4, 24, 4)], GLYPH, 1.6)]

    # --- Window / misc ----------------------------------------------------------------------------
    # The window/taskbar icon: the engine's mark — a hollow cube with an accent edge, legible at 16 px.
    g["window_icon"] = _cube("#E6E9ED", "#2B3038") + [("lines", [(16, 4, 27, 11)], ACCENT, 2.0)]
    g["chevron_down"] = [("poly", [(6, 12), (16, 22), (26, 12), (26, 9), (16, 19), (6, 9)], GLYPH)]
    g["chevron_right"] = [("poly", [(12, 6), (22, 16), (12, 26), (9, 26), (19, 16), (9, 6)], GLYPH)]
    g["close_x"] = [("lines", [(8, 8, 24, 24), (24, 8, 8, 24)], GLYPH, 3.0)]
    g["reset_arrow"] = [("arc", 16, 16, 9.5, 60.0, 300.0, 3.0, GLYPH), ("poly", [(16, 2), (23, 7), (15, 11)], GLYPH)]
    g["link_broken"] = [("lines", [(4, 20, 11, 13), (21, 11, 28, 4)], DIM, 3.0),
                        ("ring", 12, 12, 4.5, 2.0, GLYPH), ("dot", 24, 24, 2.6, ERROR)]

    missing = [name for name in ICON_ORDER if name not in g]
    if missing:
        raise RuntimeError("icon table is missing glyphs: " + ", ".join(missing))
    return g


def _cube(edge, face):
    """An isometric cube: top/left/right faces, edges in `edge`. Used for actors and scene assets."""
    top = [(16, 4), (27, 10), (16, 16), (5, 10)]
    left = [(5, 10), (16, 16), (16, 28), (5, 22)]
    right = [(27, 10), (27, 22), (16, 28), (16, 16)]
    return [("poly", top, edge), ("poly", left, face), ("poly", right, DIM),
            ("lines", [(5, 10, 16, 16), (27, 10, 16, 16), (16, 16, 16, 28)], edge, 1.2)]


def _rays(cx, cy, r0, r1, width, colour):
    """Eight radial ticks — the sun's rays."""
    out = []
    for i in range(8):
        a = math.radians(i * 45.0)
        out.append(("line", cx + math.cos(a) * r0, cy + math.sin(a) * r0,
                    cx + math.cos(a) * r1, cy + math.sin(a) * r1, width, colour))
    return out


def icon_table():
    """Manifest as data: [{name, index, col, row, x, y, size}, ...] — what the C++ table is checked against."""
    return [{
        "name": name,
        "index": i,
        "col": i % COLS,
        "row": i // COLS,
        "x": (i % COLS) * CELL,
        "y": (i // COLS) * CELL,
        "size": CELL,
    } for i, name in enumerate(ICON_ORDER)]


def atlas_size():
    return {"width": ATLAS, "height": ROWS * CELL, "cell": CELL, "cols": COLS, "rows": ROWS}


# --- GIMP driver ----------------------------------------------------------------------------------

def _ops_for(name, glyph):
    """Translate one glyph's primitives into GIMP selection ops + fills.

    Everything is built as a selection then filled with the context colour: GIMP's own polygon/ellipse selection is
    anti-aliased and handles sub-pixel geometry, which hand-computed pixel writes do not. `None` colours mean
    "geometry only" (used by the wireframe glyph, whose faces must stay transparent).
    """
    from gi.repository import Gimp, Gegl

    ops = []
    for prim in glyph:
        kind = prim[0]

        if kind == "rect":
            _, x, y, w, h, colour = prim
            ops.append(("path", [("rect", x, y, w, h)], colour))
        elif kind == "poly":
            _, pts, colour = prim
            ops.append(("path", [("poly", pts)], colour))
        elif kind in ("dot", "ring", "bulb", "eye"):
            # The primitive's own name is the selection kind; its last element is the fill colour (a ring's args are
            # centre/radius/thickness, so splitting the tail off keeps the geometry unpacks exact).
            ops.append((kind, prim[1:-1], prim[-1]))
        elif kind == "lines":
            _, segs, colour, width = prim
            ops.append(("path", [("line", s[0], s[1], s[2], s[3], width) for s in segs], colour))
        elif kind == "line":
            _, x1, y1, x2, y2, width, colour = prim
            ops.append(("path", [("line", x1, y1, x2, y2, width)], colour))
        elif kind == "arc":
            _, cx, cy, r, a0, a1, width, colour = prim
            ops.append(("arc", (cx, cy, r, a0, a1, width), colour))
        elif kind == "round_rect":
            _, x, y, w, h, radius, colour = prim
            ops.append(("round_rect", (x, y, w, h, radius), colour))
        elif kind == "gear":
            _, cx, cy, r_out, r_in, tooth, colour = prim
            ops.append(("gear", (cx, cy, r_out, r_in, tooth), colour))
        else:
            raise RuntimeError("unknown primitive: " + kind)
    return ops


def _selection_is_empty(image):
    from gi.repository import Gimp

    proc = Gimp.get_pdb().lookup_procedure("gimp-selection-is-empty")
    cfg = proc.create_config()
    cfg.set_property("image", image)
    result = proc.run(cfg)
    return bool(result.index(1))


def draw_atlas(image=None):
    """Draw every glyph into one layer per icon of `image` (created when omitted). Returns the image."""
    from gi.repository import Gimp, Gegl

    glyphs = _glyphs()
    if image is None:
        # Gimp.Image.new takes a *base type* (RGB / GRAYSCALE / INDEXED), not a GimpImageType. Passing
        # Gimp.ImageType.RGBA_IMAGE — whose numeric value is 1, the same as GRAYSCALE — silently builds a greyscale
        # image, which then exports every coloured glyph as its luminance: an atlas of grey blobs and no error.
        image = Gimp.Image.new(ATLAS, ROWS * CELL, Gimp.ImageBaseType.RGB)

    def colour_of(value):
        return Gegl.Color.new(value) if isinstance(value, str) else None

    for entry in icon_table():
        name = entry["name"]
        ox = float(entry["x"])
        oy = float(entry["y"])

        layer = Gimp.Layer.new(image, "icon_" + name, ATLAS, ROWS * CELL,
                               Gimp.ImageType.RGBA_IMAGE, 100, Gimp.LayerMode.NORMAL)
        image.insert_layer(layer, None, 0)
        if not layer.has_alpha():
            layer.add_alpha()
        layer.fill(Gimp.FillType.TRANSPARENT)

        for path_kind, payload, colour in _ops_for(name, glyphs[name]):
            _select(image, path_kind, payload, ox, oy)
            if colour is None:
                Gimp.Selection.none(image)
                continue
            # Fill only a non-empty selection. GIMP's edit_fill with an empty selection paints the ENTIRE layer — no
            # error, no warning — so one degenerate primitive (a polygon that flattened to two points) silently floods
            # the atlas with solid colour and every glyph loses its transparency. Failing here is the only way this is
            # caught before someone notices in the editor months later.
            if _selection_is_empty(image):
                raise RuntimeError("glyph '%s' produced an empty selection for a %s primitive" % (name, path_kind))
            Gimp.context_set_foreground(colour_of(colour))
            Gimp.Drawable.edit_fill(layer, Gimp.FillType.FOREGROUND)
            Gimp.Selection.none(image)

        # Keep the layer named exactly after the glyph so the XCF is self-documenting.
        layer.set_name(name)

    Gimp.displays_flush()
    return image


def _select(image, kind, payload, ox, oy):
    """Build a selection for one primitive, in atlas coordinates (cell origin added here)."""
    from gi.repository import Gimp

    if kind == "path":
        for op in payload:
            _select_one(image, op[0], op[1:], ox, oy)
        return
    _select_one(image, kind, payload, ox, oy)


def _select_one(image, kind, args, ox, oy):
    from gi.repository import Gimp

    if kind == "rect":
        x, y, w, h = args
        image.select_rectangle(Gimp.ChannelOps.ADD, int(round(ox + x)), int(round(oy + y)),
                               max(1, int(round(w))), max(1, int(round(h))))
    elif kind == "poly":
        pts = args[0]
        # Flatten a list of (x, y) pairs. Iterating it with an index step of 2 (the shape you write for a *flat* list)
        # silently drops every other vertex: a triangle becomes a segment, GIMP reports an empty selection, and
        # edit_fill on an empty selection paints the whole layer — which is how one bad glyph can flood an atlas.
        flat = []
        for px, py in pts:
            flat.extend([ox + px, oy + py])
        image.select_polygon(Gimp.ChannelOps.ADD, flat)
    elif kind == "line":
        x1, y1, x2, y2, width = args
        _select_thick_line(image, ox + x1, oy + y1, ox + x2, oy + y2, width)
    elif kind == "dot":
        cx, cy, r = args
        image.select_ellipse(Gimp.ChannelOps.ADD, int(round(ox + cx - r)), int(round(oy + cy - r)),
                             max(1, int(round(2 * r))), max(1, int(round(2 * r))))
    elif kind == "ring":
        cx, cy, r, t = args
        image.select_ellipse(Gimp.ChannelOps.ADD, int(round(ox + cx - r)), int(round(oy + cy - r)),
                             max(1, int(round(2 * r))), max(1, int(round(2 * r))))
        inner = max(0.5, r - t)
        image.select_ellipse(Gimp.ChannelOps.SUBTRACT, int(round(ox + cx - inner)), int(round(oy + cy - inner)),
                             max(1, int(round(2 * inner))), max(1, int(round(2 * inner))))
    elif kind == "eye":
        # Almond: two arcs would be exact, but an ellipse squashed to the lid height reads as an eye at 32 px once the
        # pupil is added — and the closed variant is the same primitive with a flatter squash.
        cx, cy, rx, ry = args
        image.select_ellipse(Gimp.ChannelOps.ADD, int(round(ox + cx - rx)), int(round(oy + cy - ry)),
                             max(1, int(round(2 * rx))), max(1, int(round(2 * ry))))
    elif kind == "bulb":
        cx, cy, r = args
        image.select_ellipse(Gimp.ChannelOps.ADD, int(round(ox + cx - r)), int(round(oy + cy - r)),
                             max(1, int(round(2 * r))), max(1, int(round(2 * r))))
    elif kind == "round_rect":
        x, y, w, h, radius = args
        if hasattr(image, "select_round_rectangle"):
            image.select_round_rectangle(Gimp.ChannelOps.ADD, int(round(ox + x)), int(round(oy + y)),
                                         max(1, int(round(w))), max(1, int(round(h))), float(radius), float(radius))
        else:
            image.select_rectangle(Gimp.ChannelOps.ADD, int(round(ox + x)), int(round(oy + y)),
                                   max(1, int(round(w))), max(1, int(round(h))))
    elif kind == "arc":
        cx, cy, r, a0, a1, width = args
        _select_arc(image, ox + cx, oy + cy, r, a0, a1, width)
    elif kind == "gear":
        cx, cy, r_out, r_in, tooth = args
        pts = []
        steps = 8
        for i in range(steps * 2):
            a = math.pi * i / steps
            r = r_out if (i % 2 == 0) else r_in
            pts.append([ox + cx + math.cos(a) * r, oy + cy + math.sin(a) * r])
        image.select_polygon(Gimp.ChannelOps.ADD, [v for p in pts for v in p])
        inner = max(1.0, r_in - tooth)
        image.select_ellipse(Gimp.ChannelOps.SUBTRACT, int(round(ox + cx - inner)), int(round(oy + cy - inner)),
                             max(1, int(round(2 * inner))), max(1, int(round(2 * inner))))
    else:
        raise RuntimeError("unknown selection kind: " + kind)


def _select_thick_line(image, x1, y1, x2, y2, width):
    """A stroked line as a quad polygon, so line caps/joins stay anti-aliased like the rest of the glyph."""
    from gi.repository import Gimp

    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    if length < 1e-6:
        image.select_ellipse(Gimp.ChannelOps.ADD, int(round(x1 - width / 2)), int(round(y1 - width / 2)),
                             max(1, int(round(width))), max(1, int(round(width))))
        return
    nx, ny = -dy / length * width / 2.0, dx / length * width / 2.0
    image.select_polygon(Gimp.ChannelOps.ADD, [x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny])


def _select_arc(image, cx, cy, r, deg0, deg1, width):
    """An arc band as one annular-sector polygon (outer sweep forward, inner sweep back).

    The obvious implementation — outer circle minus inner circle, intersected with a wedge — is wrong in practice:
    GIMP's INTERSECT against a large wedge clipped the band into pie slices and left gaps in the middle of undo/redo
    arrows, visible immediately in the contact sheet. Tracing both radii as a single polygon has no boolean at all, so
    the band is exact at any sweep and its ends are square caps, which is what these glyphs want anyway.
    """
    from gi.repository import Gimp

    inner = max(0.5, r - width)
    steps = max(6, int(abs(deg1 - deg0) / 6.0))
    pts = []
    for i in range(steps + 1):
        a = math.radians(deg0 + (deg1 - deg0) * i / steps)
        pts.append([cx + math.cos(a) * r, cy + math.sin(a) * r])
    for i in range(steps, -1, -1):
        a = math.radians(deg0 + (deg1 - deg0) * i / steps)
        pts.append([cx + math.cos(a) * inner, cy + math.sin(a) * inner])

    image.select_polygon(Gimp.ChannelOps.REPLACE, [v for p in pts for v in p])


def _status(result):
    """The first value of a PDB return array, as its enum name.

    Gimp.ValueArray exposes neither `[]` nor `get_index` in these bindings — the accessor is `.index(n)`, and finding
    that out by guessing costs a confusing TypeError at a call site that looks obviously right. Compare the enum's
    *nick* ("success") rather than str(), which varies between bindings versions.
    """
    value = result.index(0)
    return getattr(value, "value_nick", None) or str(value)


def export(image, out_dir=OUT_DIR):
    """Write the layered XCF, then the flattened RGBA8 PNG composite. Returns the paths written."""
    from gi.repository import Gimp, Gio

    os.makedirs(out_dir, exist_ok=True)
    png = os.path.join(out_dir, "hp_editor_icons.png")
    xcf = os.path.join(out_dir, "hp_editor_icons.xcf")

    # XCF first: flattening is destructive to the in-memory image, and the layered file is the asset someone edits.
    # The procedure's `file` property is a GimpFile, not a path string — passing a str fails with "does not have
    # property", which reads like a missing argument and is neither.
    proc = Gimp.get_pdb().lookup_procedure("gimp-xcf-save")
    cfg = proc.create_config()
    cfg.set_property("run-mode", Gimp.RunMode.NONINTERACTIVE)
    cfg.set_property("image", image)
    cfg.set_property("file", Gio.File.new_for_path(xcf))
    if _status(proc.run(cfg)) != "success":
        raise RuntimeError("xcf save failed: " + str(Gimp.get_pdb().get_last_error()))

    # file-png-export takes an *image* and writes its visible composite, alpha preserved — unlike the MCP
    # export_image(flatten=False) path, which exports a single drawable and loses every layer under it.
    proc = Gimp.get_pdb().lookup_procedure("file-png-export")
    cfg = proc.create_config()
    cfg.set_property("image", image)
    cfg.set_property("file", Gio.File.new_for_path(png))
    if _status(proc.run(cfg)) != "success":
        raise RuntimeError("png export failed: " + str(Gimp.get_pdb().get_last_error()))

    with open(os.path.join(out_dir, "icon_manifest.json"), "w") as handle:
        json.dump({"atlas": atlas_size(), "cell": CELL, "icons": icon_table()}, handle, indent=2)
        handle.write("\n")

    print("wrote", png, xcf)
    return [png, xcf, os.path.join(out_dir, "icon_manifest.json")]


def cpp_table():
    """The C++ name table (`EditorIconTable.inl`), generated so the layout cannot drift from the atlas.

    One generator for the manifest and this file means a rename or an insertion is a single edit in ICON_ORDER; the
    remaining risk — someone editing the PNG by hand, or forgetting to re-run this script — is what TestEditorIcons
    catches by checking that every named cell actually has ink and that no unnamed cell does.
    """
    lines = [
        "// Generated by scripts/make_editor_icons.py — do not edit.",
        "// Regenerate with:  python3 scripts/run_gimp_script.py scripts/make_editor_icons.py",
        "//",
        "// Cell order is the atlas' own: index i sits at column i % kAtlasColumns, row i / kAtlasColumns. The test in",
        "// tests/Editor/TestEditorIcons.cpp asserts this list against icon_manifest.json AND against ink in the PNG,",
        "// so a hand-edited asset or a forgotten regeneration fails the build rather than mislabelling a button.",
        "",
    ]
    for name in ICON_ORDER:
        lines.append('    "%s",' % name)
    body = "\n".join(lines) + "\n"
    return body


def write_cpp_table(path="engine/Editor/Src/EditorIconTable.inl"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(cpp_table())
    return path


def run():
    """Full authoring pass: draw, export, and report what is in the atlas."""
    image = draw_atlas()
    paths = export(image)
    paths.append(write_cpp_table())
    print("icons:", len(ICON_ORDER), "atlas", ATLAS, "x", ROWS * CELL)
    return paths


if __name__ == "__main__":
    run()
