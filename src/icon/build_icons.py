#!/usr/bin/env python3
"""Render the Elfeed2 app icon at every size we need, then assemble
.iconset/.icns for macOS and .ico for Windows.

Run from repo root:
    python3 src/icon/build_icons.py src/

The output replaces src/elfeed2.png, src/elfeed2.icns, src/elfeed2.ico.
Requires Pillow and macOS's iconutil (only for the .icns step). Re-run
this when the artwork or background color changes; otherwise the
committed .icns/.ico are used as-is by the build."""

from PIL import Image, ImageDraw, ImageFont
import os
import subprocess
import sys

NAVY  = (37, 64, 110, 255)
HELV  = "/System/Library/Fonts/Helvetica.ttc"
HELV_BOLD_INDEX = 1

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)

def render(size: int) -> Image.Image:
    """Render the icon at exactly `size` pixels. Renders fresh at each
    size (rather than downscaling a single high-res master) so 16/32/48
    keep crisp edges; the font/padding scale proportionally."""
    img = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(img)

    # Rounded-square background. ~17% radius matches the macOS Big
    # Sur app-icon shape rounding.
    radius = max(2, round(size * 0.17))
    d.rounded_rectangle((0, 0, size - 1, size - 1),
                        radius=radius, fill=NAVY)

    # Helvetica Bold E with a smaller "²" superscript. The "²" sits
    # at roughly half the E's cap height, top-aligned with the cap.
    e_px = round(size * 0.78)
    sup_px = round(size * 0.36)
    e_font = ImageFont.truetype(HELV, e_px, index=HELV_BOLD_INDEX)
    sup_font = ImageFont.truetype(HELV, sup_px, index=HELV_BOLD_INDEX)

    e_bb = d.textbbox((0, 0), "E", font=e_font)
    ew = e_bb[2] - e_bb[0]; eh = e_bb[3] - e_bb[1]
    sup_bb = d.textbbox((0, 0), "2", font=sup_font)
    sw = sup_bb[2] - sup_bb[0]

    gap = max(1, round(size * 0.03))
    total_w = ew + gap + sw
    x = (size - total_w) // 2 - e_bb[0]
    y = (size - eh) // 2 - e_bb[1]
    d.text((x, y), "E", font=e_font, fill="white")

    sx = x + ew + gap - sup_bb[0]
    sy = y + e_bb[1] - sup_bb[1] - max(1, round(size * 0.025))
    d.text((sx, sy), "2", font=sup_font, fill="white")
    return img

# macOS .iconset expects this exact set of names + sizes.
iconset_dir = os.path.join(OUT, "elfeed2.iconset")
os.makedirs(iconset_dir, exist_ok=True)
mac_sizes = [
    ("icon_16x16.png",       16),
    ("icon_16x16@2x.png",    32),
    ("icon_32x32.png",       32),
    ("icon_32x32@2x.png",    64),
    ("icon_128x128.png",    128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png",    256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png",    512),
    ("icon_512x512@2x.png", 1024),
]
for name, size in mac_sizes:
    render(size).save(os.path.join(iconset_dir, name))

# Master 1024px PNG (artwork-of-record for documentation / repos).
render(1024).save(os.path.join(OUT, "elfeed2.png"))

# .icns via macOS iconutil — just maps the iconset directory onto
# Apple's container format.
icns_path = os.path.join(OUT, "elfeed2.icns")
subprocess.check_call(
    ["iconutil", "-c", "icns", iconset_dir, "-o", icns_path])

# Windows .ico with all the standard sizes Windows might ask for.
# Pillow's ICO encoder filters out any size larger than the leading
# image, so we pass the largest first and the rest via append_images.
win_sizes = sorted([16, 24, 32, 48, 64, 128, 256], reverse=True)
win_imgs = [render(s) for s in win_sizes]
win_imgs[0].save(
    os.path.join(OUT, "elfeed2.ico"),
    format="ICO",
    sizes=[(s, s) for s in win_sizes],
    append_images=win_imgs[1:],
)

print("Wrote:")
for f in ["elfeed2.png", "elfeed2.icns", "elfeed2.ico"]:
    print(" ", os.path.join(OUT, f))
