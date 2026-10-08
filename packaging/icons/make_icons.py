#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Oscilline contributors
#
# Writes the app icon as SVG, PNG sizes, Windows .ico, and macOS .icns from one
# geometry, so every format matches. Needs Pillow. Run from any directory:
#   python3 packaging/icons/make_icons.py

from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
APP_ID = "io.github.marioaddict3.Oscilline"

# Geometry on a 256 x 256 canvas.
SIZE = 256
BACKGROUND = "#000000"
TILE = (16, 16, 240, 240)
TILE_RADIUS = 48
DIAMOND = [(128, 56), (200, 128), (128, 200), (56, 128)]
DIAMOND_COLOR = "#ffffff"
DIAMOND_WIDTH = 12
# The ribbon: flat, a short oscillation inside the diamond, then flat again.
RIBBON = [(44, 128), (86, 128), (100, 104), (114, 152), (128, 104), (142, 152), (156, 104),
          (170, 128), (212, 128)]
RIBBON_COLOR = "#f58e27"
RIBBON_WIDTH = 10

PNG_SIZES = [16, 24, 32, 48, 64, 128, 256, 512]


def svg() -> str:
    def points(xy):
        return " ".join(f"{x},{y}" for x, y in xy)

    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">
  <rect x="{TILE[0]}" y="{TILE[1]}" width="{TILE[2] - TILE[0]}" height="{TILE[3] - TILE[1]}" rx="{TILE_RADIUS}" fill="{BACKGROUND}"/>
  <polygon points="{points(DIAMOND)}" fill="none" stroke="{DIAMOND_COLOR}" stroke-width="{DIAMOND_WIDTH}" stroke-linejoin="round"/>
  <polyline points="{points(RIBBON)}" fill="none" stroke="{RIBBON_COLOR}" stroke-width="{RIBBON_WIDTH}" stroke-linejoin="round" stroke-linecap="round"/>
</svg>
"""


def render(size: int) -> Image.Image:
    # Draw at four times the size, then downsample for smooth edges.
    scale = size * 4 / SIZE
    image = Image.new("RGBA", (size * 4, size * 4), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def sc(xy):
        return [(x * scale, y * scale) for x, y in xy]

    draw.rounded_rectangle([v * scale for v in TILE], radius=TILE_RADIUS * scale, fill=BACKGROUND)
    width = round(DIAMOND_WIDTH * scale)
    draw.line(sc(DIAMOND + DIAMOND[:1]), fill=DIAMOND_COLOR, width=width, joint="curve")
    for x, y in sc(DIAMOND):
        draw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=DIAMOND_COLOR)
    width = round(RIBBON_WIDTH * scale)
    draw.line(sc(RIBBON), fill=RIBBON_COLOR, width=width, joint="curve")
    for x, y in sc(RIBBON[:1] + RIBBON[-1:]):
        draw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=RIBBON_COLOR)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    (HERE / f"{APP_ID}.svg").write_text(svg(), encoding="utf-8")
    images = {size: render(size) for size in PNG_SIZES}
    png_dir = HERE / "png"
    png_dir.mkdir(exist_ok=True)
    for size, image in images.items():
        image.save(png_dir / f"{size}.png", optimize=True)
    images[256].save(HERE / "oscilline.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    images[512].save(HERE / "oscilline.icns")


if __name__ == "__main__":
    main()
