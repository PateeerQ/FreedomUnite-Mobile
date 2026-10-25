"""The app's icon, drawn from scratch (no Capcom artwork): a hunter's crest --
a brass ring with three claw slashes, "MHFU" below -- on dark leather.

    pip install pillow
    python tools/make_icon.py

writes
    android/app/src/main/res/mipmap-*/ic_launcher.png            the whole icon (older launchers)
    android/app/src/main/res/mipmap-*/ic_launcher_foreground.png  the crest alone (adaptive icon)
    android/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml
    assets/icon/mhfu.ico, assets/icon/mhfu.png                    Windows (mhfu_host, mhfu_launcher)
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
S = 1024
GOLD = (236, 194, 104)
GOLD_DARK = (150, 110, 50)
EMBER = (210, 90, 35)
LEATHER_TOP = (66, 30, 14)
LEATHER_BOTTOM = (16, 11, 8)


def font(size):
    for name in ("georgiab.ttf", "timesbd.ttf", "DejaVuSerif-Bold.ttf"):
        for d in (os.environ.get("WINDIR", "C:/Windows") + "/Fonts", "/usr/share/fonts/truetype/dejavu"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def slash(d, x0, y0, x1, y1, w):
    """A claw mark: thick in the middle, pointed at both ends."""
    import math
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    nx, ny = -dy / n, dx / n
    pts_l, pts_r = [], []
    for i in range(41):
        t = i / 40
        half = w * math.sin(math.pi * t) ** 0.8 / 2
        cx, cy = x0 + dx * t, y0 + dy * t
        pts_l.append((cx + nx * half, cy + ny * half))
        pts_r.append((cx - nx * half, cy - ny * half))
    return pts_l + pts_r[::-1]


def crest(size, text=True):
    """The crest on a transparent square of `size`; drawn at 1024 and scaled."""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx, cy, r = S // 2, (430 if text else S // 2), (290 if text else 360)
    # the ring: gold, a darker inner line, studs at the four points
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=GOLD, width=26)
    d.ellipse((cx - r + 42, cy - r + 42, cx + r - 42, cy + r - 42), outline=GOLD_DARK, width=8)
    for ax, ay in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        x, y, k = cx + ax * r, cy + ay * r, 30
        d.polygon([(x, y - k), (x + k, y), (x, y + k), (x - k, y)], fill=GOLD)
    # three claw slashes, glowing ember under gold
    marks = []
    for off in (-120, 0, 120):
        marks.append(slash(d, cx + off + 150, cy - r + 70, cx + off - 150, cy + r - 70, 70))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for m in marks:
        gd.polygon(m, fill=EMBER + (255,))
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    for m in marks:
        d.polygon(m, fill=GOLD)
    if text:
        f = font(170)
        w = d.textlength("MHFU", font=f)
        d.text(((S - w) / 2 + 4, 770 + 4), "MHFU", font=f, fill=(0, 0, 0, 160))
        d.text(((S - w) / 2, 770), "MHFU", font=f, fill=GOLD)
    return img.resize((size, size), Image.LANCZOS)


def leather(size, rounded=True):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        t = y / (S - 1)
        gd.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(LEATHER_TOP, LEATHER_BOTTOM)) + (255,))
    mask = Image.new("L", (S, S), 0)
    md = ImageDraw.Draw(mask)
    if rounded:
        md.rounded_rectangle((0, 0, S - 1, S - 1), radius=200, fill=255)
    else:
        md.rectangle((0, 0, S, S), fill=255)
    img.paste(grad, (0, 0), mask)
    if rounded:
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((34, 34, S - 35, S - 35), radius=170, outline=GOLD_DARK, width=10)
    return img.resize((size, size), Image.LANCZOS)


def whole(size):
    return Image.alpha_composite(leather(S), crest(S)).resize((size, size), Image.LANCZOS)


def main():
    res = os.path.join(ROOT, "android", "app", "src", "main", "res")
    for dpi, legacy, fg in (("mdpi", 48, 108), ("hdpi", 72, 162), ("xhdpi", 96, 216),
                            ("xxhdpi", 144, 324), ("xxxhdpi", 192, 432)):
        d = os.path.join(res, "mipmap-" + dpi)
        os.makedirs(d, exist_ok=True)
        whole(legacy).save(os.path.join(d, "ic_launcher.png"))
        # Adaptive: the crest inside the middle 66 of 108 dp (the safe zone).
        layer = Image.new("RGBA", (fg, fg), (0, 0, 0, 0))
        inner = int(fg * 66 / 108)
        layer.paste(crest(inner), ((fg - inner) // 2, (fg - inner) // 2))
        layer.save(os.path.join(d, "ic_launcher_foreground.png"))
    d = os.path.join(res, "mipmap-anydpi-v26")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "ic_launcher.xml"), "w", newline="\n") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_background" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
                '</adaptive-icon>\n')
    out = os.path.join(ROOT, "assets", "icon")
    os.makedirs(out, exist_ok=True)
    big = whole(256)
    big.save(os.path.join(out, "mhfu.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    whole(512).save(os.path.join(out, "mhfu.png"))
    print("icons written")


if __name__ == "__main__":
    sys.exit(main())
