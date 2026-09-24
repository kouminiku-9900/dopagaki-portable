#!/usr/bin/env python3
"""Draw the komi-tube XMB icon (ICON0.PNG, 144x80) into assets/."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

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


def gradient(size, top, bottom):
    w, h = size
    img = Image.new('RGBA', size)
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
        for x in range(w):
            px[x, y] = c
    return img


def main():
    icon = Image.new('RGBA', (W, H), (0, 0, 0, 0))

    # Rounded card with a teal-to-indigo gradient and a soft shadow.
    card = (6 * SCALE, 6 * SCALE, W - 6 * SCALE, H - 8 * SCALE)
    radius = 14 * SCALE
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (card[0], card[1] + 3 * SCALE, card[2], card[3] + 3 * SCALE),
        radius, fill=(0, 0, 0, 110))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(3 * SCALE)))
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(card, radius, fill=255)
    icon.paste(gradient((W, H), (32, 201, 170), (58, 64, 180)), (0, 0), mask)

    draw = ImageDraw.Draw(icon)
    # Play button: a cream disc with a coral triangle.
    cx, cy, r = 36 * SCALE, 37 * SCALE, 19 * SCALE
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 246, 228, 255))
    t = 9 * SCALE
    draw.polygon(
        [(cx - t * 0.7, cy - t), (cx - t * 0.7, cy + t), (cx + t * 1.1, cy)],
        fill=(255, 111, 97, 255))

    # Wordmark.
    draw.text((62 * SCALE, 25 * SCALE), 'komi', font=font(21 * SCALE),
              fill=(255, 255, 255, 255))
    draw.text((63 * SCALE, 47 * SCALE), 'tube', font=font(15 * SCALE),
              fill=(255, 236, 190, 255))

    out = ROOT / 'assets'
    out.mkdir(exist_ok=True)
    icon.save(out / 'icon-master.png')
    icon.resize((144, 80), Image.LANCZOS).save(out / 'ICON0.PNG', optimize=True)
    print(out / 'ICON0.PNG')


if __name__ == '__main__':
    main()
