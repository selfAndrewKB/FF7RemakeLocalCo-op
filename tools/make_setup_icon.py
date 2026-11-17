"""Draws the setup app's icon, LocalSplitScreen/setup/app.ico: two screens side by side, each with the corner marks the app
puts on a chosen monitor, on the app's dark panel. Original drawing; nothing of the game's is used.

    python -I tools/make_setup_icon.py
"""
import pathlib

from PIL import Image, ImageDraw

OUT = pathlib.Path(__file__).resolve().parent.parent / "LocalSplitScreen" / "setup" / "app.ico"
S = 1024  # drawn large, then reduced: smooth edges at every size

PANEL = ((14, 24, 44), (5, 9, 18))
SCREEN = ((28, 86, 140), (14, 46, 88))
EDGE = (58, 74, 104)
ACCENT = (98, 222, 242)


def fade(size, top, bottom):
    image = Image.new("RGB", size)
    draw = ImageDraw.Draw(image)
    for y in range(size[1]):
        t = y / max(size[1] - 1, 1)
        draw.line([(0, y), (size[0], y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return image


def brackets(draw, box, length, thick):
    left, top, right, bottom = box
    for x in (left, right):
        for y in (top, bottom):
            dx = 1 if x == left else -1
            dy = 1 if y == top else -1
            draw.rectangle(sorted_box(x, y, x + dx * length, y + dy * thick), fill=ACCENT)
            draw.rectangle(sorted_box(x, y, x + dx * thick, y + dy * length), fill=ACCENT)


def sorted_box(x0, y0, x1, y1):
    return [min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)]


def draw_icon():
    icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([24, 24, S - 24, S - 24], radius=180, fill=255)
    icon.paste(fade((S, S), *PANEL), (0, 0), mask)
    draw = ImageDraw.Draw(icon)
    draw.rounded_rectangle([24, 24, S - 24, S - 24], radius=180, outline=EDGE, width=14)
    for left in (136, 536):
        box = (left, 384, left + 352, 640)
        icon.paste(fade((box[2] - box[0], box[3] - box[1]), *SCREEN), box[:2])
        brackets(draw, box, 100, 36)
    return icon


def main():
    icon = draw_icon()
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    icon.resize((256, 256), Image.LANCZOS).save(OUT, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
