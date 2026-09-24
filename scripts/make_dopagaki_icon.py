#!/usr/bin/env python3
"""Draw the dopagaki-portable XMB icon (ICON0.PNG, 144x80) into assets/.

Loud on purpose: rainbow stripes behind a big 「ドパガキ」 in rainbow letters
with a white-then-black outline, 「ポータブル!!」 underneath, and a few
sparkles. Drawn at 4x and scaled down.
"""
import colorsys
import glob
import math
from pathlib import Path
import unicodedata

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
W, H = 144 * SCALE, 80 * SCALE


def japanese_font(size):
    # macOS names these files in decomposed Unicode, so match loosely.
    for weight in ('W9', 'W8', 'W7', 'W6'):
        for path in glob.glob('/System/Library/Fonts/*.ttc'):
            name = unicodedata.normalize('NFC', Path(path).name)
            if name.startswith('ヒラギノ角ゴシック') and weight in name:
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def rainbow(size, angle_deg, cycles, saturation=1.0, value=1.0, shift=0.0):
    """Hue bands across the image at the given angle."""
    w, h = size
    img = Image.new('RGBA', size)
    px = img.load()
    a = math.radians(angle_deg)
    dx, dy = math.cos(a), math.sin(a)
    span = abs(w * dx) + abs(h * dy)
    for y in range(h):
        for x in range(w):
            t = (x * dx + y * dy) / span * cycles + shift
            r, g, b = colorsys.hsv_to_rgb(t % 1.0, saturation, value)
            px[x, y] = (round(r * 255), round(g * 255), round(b * 255), 255)
    return img


def outlined_text(canvas, xy, text, font, fill, outlines):
    """outlines: [(width, colour), ...] from the outermost in."""
    x, y = xy
    mask = Image.new('L', canvas.size, 0)
    ImageDraw.Draw(mask).text((x, y), text, font=font, fill=255)
    for width, colour in outlines:
        layer = Image.new('L', canvas.size, 0)
        ImageDraw.Draw(layer).text((x, y), text, font=font, fill=255,
                                   stroke_width=width, stroke_fill=255)
        canvas.paste(Image.new('RGBA', canvas.size, colour), (0, 0), layer)
    if isinstance(fill, Image.Image):
        canvas.paste(fill, (0, 0), mask)
    else:
        canvas.paste(Image.new('RGBA', canvas.size, fill), (0, 0), mask)


def sparkle(draw, cx, cy, r, colour):
    k = r * 0.28
    draw.polygon([(cx, cy - r), (cx + k, cy - k), (cx + r, cy),
                  (cx + k, cy + k), (cx, cy + r), (cx - k, cy + k),
                  (cx - r, cy), (cx - k, cy - k)], fill=colour)


def main():
    s = SCALE
    # Background: bold diagonal rainbow stripes, darkened toward the middle
    # so the lettering pops.
    bg = rainbow((W, H), -30, 2.2, saturation=0.95, value=1.0)
    stripes = Image.new('L', (W, H), 0)
    sd = ImageDraw.Draw(stripes)
    for i in range(-H, W + H, 14 * s):
        sd.polygon([(i, 0), (i + 7 * s, 0), (i + 7 * s - H, H), (i - H, H)],
                   fill=60)
    bg = Image.composite(Image.new('RGBA', (W, H), (255, 255, 255, 255)),
                         bg, stripes)
    shade = Image.new('L', (W, H), 0)
    ImageDraw.Draw(shade).ellipse((-20 * s, 10 * s, W + 20 * s, H - 8 * s),
                                  fill=120)
    shade = shade.filter(ImageFilter.GaussianBlur(10 * s))
    bg = Image.composite(Image.new('RGBA', (W, H), (30, 0, 50, 255)), bg,
                         shade)

    icon = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1),
                                           radius=10 * s, fill=255)
    icon.paste(bg, (0, 0), mask)

    # 「ドパガキ」: huge, rainbow letters, white then black outline, tilted.
    title = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    text = 'ドパガキ'
    # The largest size whose outlined, tilted lettering stays on the card.
    size = 40
    while True:
        font = japanese_font(size * s)
        box = ImageDraw.Draw(title).textbbox((0, 0), text, font=font,
                                             stroke_width=4 * s)
        tw, th = box[2] - box[0], box[3] - box[1]
        if tw <= W - 18 * s and th <= 40 * s:
            break
        size -= 1
    x = (W - tw) // 2 - box[0]
    y = 7 * s - box[1]
    letters = rainbow((W, H), 0, 1.0, saturation=0.85, value=1.0, shift=0.95)
    outlined_text(title, (x, y), text, font, letters,
                  [(4 * s, (20, 0, 30, 255)), (2 * s, (255, 255, 255, 255))])
    title = title.rotate(5, resample=Image.BICUBIC, center=(W / 2, H / 2))
    icon.alpha_composite(title)

    # 「ポータブル!!」 on a black ribbon.
    small = japanese_font(14 * s)
    sub = 'ポータブル!!'
    box = ImageDraw.Draw(icon).textbbox((0, 0), sub, font=small)
    sw = box[2] - box[0]
    sx = (W - sw) // 2 - box[0]
    sy = 56 * s - box[1]
    ribbon = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(ribbon).polygon(
        [(sx - 8 * s, 55 * s), (sx + sw + 8 * s, 53 * s),
         (sx + sw + 6 * s, 73 * s), (sx - 6 * s, 75 * s)],
        fill=(15, 0, 25, 235))
    icon.alpha_composite(ribbon)
    outlined_text(icon, (sx, sy), sub, small, (255, 240, 60, 255),
                  [(s, (255, 60, 150, 255))])

    # Sparkles.
    d = ImageDraw.Draw(icon)
    for cx, cy, r in ((10, 12, 6), (134, 10, 5), (130, 60, 4), (12, 62, 4)):
        sparkle(d, cx * s, cy * s, r * s, (255, 255, 255, 255))

    out = icon.resize((144, 80), Image.LANCZOS)
    path = ROOT / 'assets/ICON0-dopagaki.png'
    out.save(path)
    print(path)


if __name__ == '__main__':
    main()
