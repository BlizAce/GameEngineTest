#!/usr/bin/env python3
"""Inspect a rendered screenshot: colour spread, luminance, sampled pixels.

Usage: png_probe.py FILE [CROP_W CROP_H]

A development aid for the runtime's `--screenshot <path>` output. It answers "did anything render, and is it more than one
flat colour" from a terminal, which is what makes an unattended engine run reviewable over ssh. Deliberately NOT part of the
test suite: the suites assert on texels read back through the engine itself (tests/Runtime/TestScreenshot.cpp), because a test
that shells out to Python would still pass when the capture path is broken.
"""

import sys

from PIL import Image


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    path = sys.argv[1]
    image = Image.open(path)
    print(f"{path}: {image.size[0]}x{image.size[1]} mode={image.mode}")

    rgb = image.convert("RGB")
    if len(sys.argv) > 3:
        width, height = int(sys.argv[2]), int(sys.argv[3])
        rgb = rgb.crop((0, 0, width, height))
        print(f"cropped to {width}x{height}")

    colors = rgb.getcolors(maxcolors=1 << 24)
    if colors is None:
        print("distinct colors: more than the sampling cap")
    else:
        print(f"distinct colors: {len(colors)}")
        for count, color in sorted(colors, key=lambda entry: -entry[0])[:10]:
            print(f"  rgb{color} x{count}")

    # Luma weights from ITU-R BT.709, matching what a display does with the values the tone map emits.
    luma = [sum(pixel * weight for pixel, weight in zip(color, (0.2126, 0.7152, 0.0722))) for color in rgb.getdata()]
    non_black = sum(1 for value in luma if value > 4.0)
    print(f"mean luminance: {sum(luma) / len(luma):.2f} / 255   max={max(luma):.0f}")
    print(f"non-black pixels: {non_black}/{len(luma)} = {100.0 * non_black / len(luma):.1f}%")

    width, height = rgb.size
    for (fx, fy) in [(0.5, 0.5), (0.25, 0.3), (0.75, 0.7), (0.1, 0.9)]:
        x, y = int(width * fx), int(height * fy)
        print(f"  ({x},{y}) -> rgb{rgb.getpixel((x, y))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
