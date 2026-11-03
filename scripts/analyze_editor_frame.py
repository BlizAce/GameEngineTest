#!/usr/bin/env python3
"""Measure the viewport image's *visible* extent in a composed editor frame.

Usage: analyze_editor_frame.py <png> [--label NAME] [--expect-fits]

Why this exists when the C++ suite already asserts panel geometry: the suite checks that the render target and the
image widget agree (|rendered - displayed| <= one quantisation step), which is the right invariant but is measured
from the engine's own bookkeeping. This script measures the *pixels*, so a disagreement between what ImGui says it
laid out and what actually reached the framebuffer — clipping, scrollbars over content, a panel drawn off-screen —
is caught by an observer that trusts none of it. Before the layout fix this was the only check that could see an
896x640 scene cropped to ~810x584 behind two scrollbars while every structural assertion stayed green.

Method: the scene image is a photographic gradient, unlike flat UI fills, so grow a rectangle outward from a seed
point inside it while rows/cols remain "mostly varying". The seed is the frame centre biased into the central dock
node; --expect-fits fails when scrollbars sit against the image edge.
"""
import sys


def parse_args():
    path = None
    label = ""
    expect_fits = False
    index = 1
    argv = sys.argv
    while index < len(argv):
        if argv[index] == "--label":
            label = argv[index + 1] + " "
            index += 2
        elif argv[index] == "--expect-fits":
            expect_fits = True
            index += 1
        elif path is None:
            path = argv[index]
            index += 1
        else:
            index += 1
    return path, label, expect_fits


def main():
    path, label, expect_fits = parse_args()
    if path is None:
        print("usage: analyze_editor_frame.py <png> [--label NAME] [--expect-fits]")
        return 2

    from PIL import Image

    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()

    # A pixel is "scene-like" if it differs from its neighbour by more than any flat fill or glyph edge does. Scanning
    # outward and requiring a majority of such pixels per row/column makes the boundary robust to a stray text pixel in
    # a neighbouring panel, which a simple colour-threshold flood cannot be.
    def row_varies(y, x_lo, x_hi):
        varying = 0
        previous = None
        for x in range(x_lo, x_hi, 2):
            current = pixels[x, y]
            if previous is not None and sum(abs(a - b) for a, b in zip(current, previous)) > 14:
                varying += 1
            previous = current
        return varying

    def col_varies(x, y_lo, y_hi):
        varying = 0
        previous = None
        for y in range(y_lo, y_hi, 2):
            current = pixels[x, y]
            if previous is not None and sum(abs(a - b) for a, b in zip(current, previous)) > 14:
                varying += 1
            previous = current
        return varying

    # Seed inside the central dock node: horizontally between the side panels, vertically above the bottom dock.
    seed_x = int(width * 0.5)
    seed_y = int(height * 0.35)
    reference = pixels[seed_x, seed_y]

    def same_as_seed(p):
        return sum(abs(a - b) for a, b in zip(p, reference)) < 26

    # Grow from the seed while the row/col is either part of the seed's local gradient or varies photographically.
    x_left = seed_x
    while x_left > 0 and same_as_seed(pixels[x_left - 1, seed_y]):
        x_left -= 1
    x_right = seed_x
    while x_right < width - 1 and same_as_seed(pixels[x_right + 1, seed_y]):
        x_right += 1
    y_top = seed_y
    while y_top > 0 and same_as_seed(pixels[seed_x, y_top - 1]):
        y_top -= 1
    y_bottom = seed_y
    while y_bottom < height - 1 and same_as_seed(pixels[seed_x, y_bottom + 1]):
        y_bottom += 1

    # Widen using the variation test: rows/cols that vary photographically belong to the rendered image.
    probe_rows = [y for y in range(y_top, y_bottom + 1) if row_varies(y, x_left, x_right) > (x_right - x_left) / 2 * 0.25]
    if probe_rows:
        y_top, y_bottom = probe_rows[0], probe_rows[-1]
    probe_cols = [x for x in range(x_left, x_right + 1) if col_varies(x, y_top, y_bottom) > (y_bottom - y_top) / 2 * 0.25]
    if probe_cols:
        x_left, x_right = probe_cols[0], probe_cols[-1]

    view_w = x_right - x_left + 1
    view_h = y_bottom - y_top + 1

    print(f"{label}window={width}x{height}")
    print(f"{label}scene_visible={view_w}x{view_h} at {x_left},{y_top}")
    print(f"{label}scene_area_pct={100.0 * view_w * view_h / (width * height):.1f}")

    # Distinct colours in the menu-bar strip: a stock bitmap font renders it in ~3, an antialiased TTF in far more. The
    # band is detected rather than hardcoded — its height follows the frame height and UI scale, and rows 2..18 measured
    # a *different* element on a differently-sized window, which is how this check briefly reported "3 colours" on a
    # frame whose text was demonstrably antialiased.
    #
    # Reports 0 for a frame rendered with ImGui's *default* palette (the pre-change baseline), because the detection key
    # is the editor theme's own chrome colour and stock ImGui uses another. That is "not measured", not "no
    # anti-aliasing"; compare against the editor theme, or count colours in a fixed row range by hand for a baseline.
    def is_chrome_row(y):
        targets = ((0x17, 0x19, 0x1C), (0x1B, 0x1D, 0x21))
        samples = range(0, width, 7)
        return any(sum(1 for x in samples if all(abs(a - b) <= 8 for a, b in zip(pixels[x, y], target)))
                   > len(samples) * 0.8 for target in targets)

    chrome_rows = [y for y in range(0, min(height, 40)) if is_chrome_row(y)]
    strip = set()
    if chrome_rows:
        top, bottom = chrome_rows[0], chrome_rows[-1] + 1
        strip = {pixels[x, y] for y in range(top, bottom) for x in range(0, min(width, 900))}
    print(f"{label}menubar_distinct_colors={len(strip)}")

    if not expect_fits:
        return 0

    failures = []
    if len(strip) < 5:
        failures.append(f"menu bar strip has {len(strip)} colours (un-antialiased font)")
    if view_w * view_h < 0.15 * width * height:
        failures.append(f"scene covers only {100.0 * view_w * view_h / (width * height):.1f}% of the window")
    for note in failures:
        print("PROBLEM:", note)
    return 1 if failures else 0


sys.exit(main())
