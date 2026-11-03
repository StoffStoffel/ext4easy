#!/usr/bin/env python3
"""Generates res/ext4win.ico (all sizes Windows uses). Needs Pillow.

    python tools/make_icon.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 256]
BODY_TOP = (74, 88, 112)       # drive body gradient
BODY_BOTTOM = (38, 47, 64)
FACE = (226, 232, 240)          # label strip
TEXT = (30, 41, 59)
LED = (34, 197, 94)
ACCENT = (245, 158, 11)         # orange stripe (Linux)


def draw(size: int) -> Image.Image:
    # Draw 4x larger and scale down for smooth edges.
    s = size * 4
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = s * 0.06
    top, bottom = s * 0.18, s * 0.86
    radius = s * 0.12

    # body with vertical gradient
    grad = Image.new('RGBA', (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = min(max((y - top) / (bottom - top), 0), 1)
        c = tuple(int(BODY_TOP[i] + (BODY_BOTTOM[i] - BODY_TOP[i]) * t) for i in range(3))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new('L', (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([pad, top, s - pad, bottom], radius, fill=255)
    img.paste(grad, (0, 0), mask)

    # orange stripe along the top edge
    d.rounded_rectangle([pad, top, s - pad, top + s * 0.07], radius * 0.6, fill=ACCENT + (255,))

    # label strip with "ext4" (only where it is still readable)
    strip = [pad + s * 0.08, top + s * 0.16, s - pad - s * 0.08, top + s * 0.44]
    d.rounded_rectangle(strip, s * 0.05, fill=FACE + (255,))
    if size >= 32:
        font = ImageFont.truetype(os.path.join(os.environ.get('WINDIR', 'C:\\Windows'),
                                               'Fonts', 'segoeuib.ttf'), int(s * 0.22))
        cx, cy = (strip[0] + strip[2]) / 2, (strip[1] + strip[3]) / 2
        d.text((cx, cy), 'ext4', font=font, fill=TEXT + (255,), anchor='mm')

    # LED and vent lines
    led_r = s * 0.06
    lx, ly = s - pad - s * 0.16, bottom - s * 0.14
    d.ellipse([lx - led_r, ly - led_r, lx + led_r, ly + led_r], fill=LED + (255,))
    if size >= 24:
        for i in range(3):
            x = pad + s * 0.14 + i * s * 0.08
            d.line([(x, ly - s * 0.05), (x, ly + s * 0.05)], fill=(148, 163, 184, 255),
                   width=max(1, int(s * 0.025)))

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'res', 'ext4win.ico')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    images = [draw(sz) for sz in SIZES]
    images[-1].save(out, format='ICO', sizes=[(sz, sz) for sz in SIZES],
                    append_images=images[:-1])
    images[-1].save(os.path.join(root, 'res', 'ext4win-256.png'))
    draw(64).save(os.path.join(root, 'res', 'ext4win-64.png'))  # installer logo
    print('wrote', out)


if __name__ == '__main__':
    main()
