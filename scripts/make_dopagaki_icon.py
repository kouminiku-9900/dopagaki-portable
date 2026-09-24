#!/usr/bin/env python3
"""Draw the dopagaki-portable XMB icon (ICON0.PNG, 144x80) into assets/.

A vertical card with a play mark (a Short, held upright) beside the name.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
W, H = 144 * SCALE, 80 * SCALE
FONTS = [
    '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf',
    '/System/Library/Fonts/Supplemental/Arial Bold.ttf',
    '/Library/Fonts/Arial Bold.ttf',
]


def font(size):
    for path in FONTS:
        if Path(path).is_file():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main():
    icon = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(icon)
    s = SCALE

    # Background card: deep magenta to orange, left to right.
    card = Image.new('RGBA', (W, H))
    px = card.load()
    for x in range(W):
        t = x / (W - 1)
        c = (round(170 + 70 * t), round(20 + 90 * t), round(110 - 70 * t), 255)
        for y in range(H):
            px[x, y] = c
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (3 * s, 3 * s, W - 3 * s, H - 3 * s), radius=12 * s, fill=255)
    icon.paste(card, (0, 0), mask)

    # The upright "short": a tall white card with a play triangle.
    x0, y0, x1, y1 = 12 * s, 10 * s, 46 * s, 70 * s
    draw.rounded_rectangle((x0, y0, x1, y1), radius=6 * s,
                           fill=(255, 255, 255, 255))
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    draw.polygon([(cx - 7 * s, cy - 10 * s), (cx - 7 * s, cy + 10 * s),
                  (cx + 11 * s, cy)], fill=(220, 40, 90, 255))
    # Swipe hint: two chevrons under the card's right edge.
    for dy in (0, 7 * s):
        draw.line([(52 * s, 40 * s + dy), (56 * s, 44 * s + dy),
                   (60 * s, 40 * s + dy)], fill=(255, 255, 255, 230),
                  width=2 * s)

    # The name, two lines.
    big = font(21 * s)
    small = font(13 * s)
    draw.text((66 * s, 16 * s), 'dopa', font=big, fill=(255, 255, 255, 255))
    draw.text((66 * s, 38 * s), 'gaki', font=big, fill=(255, 255, 255, 255))
    draw.text((67 * s, 60 * s), 'portable', font=small,
              fill=(255, 235, 210, 255))

    out = icon.resize((144, 80), Image.LANCZOS)
    path = ROOT / 'assets/ICON0-dopagaki.png'
    out.save(path)
    print(path)


if __name__ == '__main__':
    main()
