"""Draws res/phiwm.ico: the dwindle golden-ratio layout itself, as rounded tiles.

    python3 tools/make_icon.py
"""
from pathlib import Path
from PIL import Image, ImageDraw

S = 1024
PHI = 0.618
BG = (24, 24, 37, 255)
C1 = (0x33, 0xCC, 0xFF)
C2 = (0x00, 0xFF, 0x99)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def tiles(x, y, w, h, n):
    """Golden dwindle: existing window keeps 61.8%, new one goes right/bottom."""
    out = []
    for _ in range(n - 1):
        if w >= h:
            a = w * PHI
            out.append((x, y, a, h))
            x, w = x + a, w - a
        else:
            a = h * PHI
            out.append((x, y, w, a))
            y, h = y + a, h - a
    out.append((x, y, w, h))
    return out


def draw(size):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=S * 0.22, fill=BG)
    n = 5 if size >= 48 else 4 if size >= 24 else 3
    pad = S * 0.12
    gap = S * (0.035 if size >= 32 else 0.06)
    ts = tiles(pad, pad, S - 2 * pad, S - 2 * pad, n)
    for i, (x, y, w, h) in enumerate(ts):
        col = lerp(C1, C2, i / max(1, n - 1)) + (255,)
        r = min(w, h) * 0.18
        d.rounded_rectangle((x + gap / 2, y + gap / 2, x + w - gap / 2, y + h - gap / 2),
                            radius=max(2, r), fill=col)
    return img.resize((size, size), Image.LANCZOS)


def main():
    root = Path(__file__).resolve().parent.parent
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    imgs = [draw(s) for s in sizes]
    out = root / "res" / "phiwm.ico"
    imgs[-1].save(out, format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
    draw(512).save(root / "res" / "phiwm.png")
    print("wrote", out)


if __name__ == "__main__":
    main()
