#!/usr/bin/env python3
"""Draw the Halcyon icon set procedurally (glossy early-2000s style) and emit
C arrays. Every icon is drawn at 4x on a 128x128 canvas (unit = 1/32 of the
icon) and downsampled to 32x32, 16x16 and 48x48.

usage: mkicons.py OUTDIR [--preview PNG]
"""
import os
import sys

# Use FreeType's classic TrueType bytecode interpreter (v35): full hinting,
# like early-2000s font rendering, instead of the modern v40 default. This
# must be set before Pillow loads FreeType (it is read at library init).
_props = [p for p in os.environ.get("FREETYPE_PROPERTIES", "").split() if not p.startswith("truetype:interpreter-version=")]
os.environ["FREETYPE_PROPERTIES"] = " ".join(_props + ["truetype:interpreter-version=35"])

from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 4                      # supersampling factor
N = 32 * S                 # canvas size
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_M = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"


def hexc(c, a=255):
    c = c.lstrip("#")
    return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16), a)


def u(v):
    return int(round(v * S))


def box(x0, y0, x1, y1):
    return [u(x0), u(y0), u(x1), u(y1)]


def gradient(c1, c2, vertical=True, size=N):
    a, b = hexc(c1) if isinstance(c1, str) else c1, hexc(c2) if isinstance(c2, str) else c2
    g = Image.new("RGBA", (size, size))
    px = g.load()
    for i in range(size):
        t = i / (size - 1)
        col = tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(4))
        for j in range(size):
            if vertical:
                px[j, i] = col
            else:
                px[i, j] = col
    return g


def fill(img, shape_fn, c1, c2=None, vertical=True, outline=None, width=1.0):
    """shape_fn(draw, fill) draws the shape on a mask."""
    mask = Image.new("L", (N, N), 0)
    shape_fn(ImageDraw.Draw(mask), 255)
    src = gradient(c1, c2 or c1, vertical)
    layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    layer.paste(src, (0, 0), mask)
    img.alpha_composite(layer)
    if outline:
        edge = mask.filter(ImageFilter.MaxFilter(3))
        inner = mask.filter(ImageFilter.MinFilter(max(3, int(width * S) | 1)))
        ring = Image.eval(Image.merge("L", [edge]), lambda v: v)
        ring = Image.composite(Image.new("L", (N, N), 0), mask, inner)
        ol = Image.new("RGBA", (N, N), hexc(outline))
        layer = Image.new("RGBA", (N, N), (0, 0, 0, 0))
        layer.paste(ol, (0, 0), ring)
        img.alpha_composite(layer)
    return mask


def gloss(img, mask, top_frac=0.5, strength=110):
    """white highlight over the upper part of a shape"""
    bb = mask.getbbox()
    if not bb:
        return
    x0, y0, x1, y1 = bb
    h = int((y1 - y0) * top_frac)
    g = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = g.load()
    for y in range(y0, y0 + h):
        a = int(strength * (1 - (y - y0) / max(1, h)))
        for x in range(x0, x1):
            px[x, y] = (255, 255, 255, a)
    m2 = Image.new("L", (N, N), 0)
    m2.paste(mask, (0, 0))
    out = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    out.paste(g, (0, 0), m2)
    img.alpha_composite(out)


def shadow(img, mask, dx=1.0, dy=1.5, blur=1.5, alpha=90):
    sh = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    m = Image.new("L", (N, N), 0)
    m.paste(mask, (u(dx), u(dy)))
    m = m.filter(ImageFilter.GaussianBlur(blur * S))
    m = m.point(lambda v: v * alpha // 255)
    sh.putalpha(m)
    base = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    base.alpha_composite(sh)
    base.alpha_composite(img)
    return base


def text(img, x, y, s, size, color, font=FONT_B, anchor="mm"):
    f = ImageFont.truetype(font, u(size))
    ImageDraw.Draw(img).text((u(x), u(y)), s, font=f, fill=hexc(color), anchor=anchor)


def rrect(b, r):
    return lambda d, f: d.rounded_rectangle(box(*b), radius=u(r), fill=f)


def poly(pts):
    return lambda d, f: d.polygon([(u(x), u(y)) for x, y in pts], fill=f)


def ellipse(b):
    return lambda d, f: d.ellipse(box(*b), fill=f)


def line(img, pts, color, w):
    ImageDraw.Draw(img).line([(u(x), u(y)) for x, y in pts], fill=hexc(color), width=u(w), joint="curve")


def new():
    return Image.new("RGBA", (N, N), (0, 0, 0, 0))


def finish(img, mask=None):
    if mask is None:
        mask = img.split()[3]
    return shadow(img, mask)


# ---------------------------------------------------------------- icons
def ic_folder(open_=False, paper=False):
    img = new()
    m = fill(img, poly([(2, 7), (12, 7), (14, 9.5), (29, 9.5), (29, 27), (2, 27)]), "#F7D774", "#D9A93A", outline="#A9781E")
    if paper:
        fill(img, rrect((6, 5, 26, 24), 1), "#FFFFFF", "#E8EEF8", outline="#8A9BB8")
        for yy in (9, 12, 15):
            line(img, [(9, yy), (23, yy)], "#9FB2D6", 0.8)
    if open_:
        fill(img, poly([(5, 14), (31, 14), (28, 27), (2, 27)]), "#FFE89A", "#E6B54A", outline="#A9781E")
    else:
        fm = fill(img, rrect((2, 12, 29, 27), 1.2), "#FFE58F", "#E3AE3E", outline="#A9781E")
        gloss(img, fm, 0.45, 120)
    return finish(img, m)


def ic_page(kind="text"):
    img = new()
    m = fill(img, poly([(6, 2), (21, 2), (27, 8), (27, 30), (6, 30)]), "#FFFFFF", "#E4E9F2", outline="#7D8BA6")
    fill(img, poly([(21, 2), (27, 8), (21, 8)]), "#D6DEEC", "#B8C4DA", outline="#7D8BA6")
    if kind == "text":
        for i, yy in enumerate(range(11, 28, 3)):
            line(img, [(9, yy), (24 - (i % 3) * 3, yy)], "#5A6E96", 1.0)
    elif kind == "image":
        fill(img, rrect((9, 11, 24, 26), 0.5), "#7EC8F8", "#D8F0FF")
        fill(img, poly([(9, 26), (9, 21), (14, 17), (19, 22), (24, 19), (24, 26)]), "#5DBB4A", "#2E8A2A")
        fill(img, ellipse((18, 12.5, 22, 16.5)), "#FFE04A", "#FFB020")
    elif kind == "app":
        fill(img, rrect((9, 12, 24, 25), 1), "#FFFFFF", "#DDE6F4", outline="#2A55A8")
        fill(img, rrect((9, 12, 24, 15), 1), "#4A86E0", "#1F4FA8")
    elif kind == "sys":
        fill(img, ellipse((11, 13, 22, 24)), "#A7B4C8", "#5E6E8A", outline="#3D4A60")
        fill(img, ellipse((14.5, 16.5, 18.5, 20.5)), "#FFFFFF")
    elif kind == "audio":
        # a beamed pair of notes
        fill(img, ellipse((8.5, 21, 14, 25.5)), "#5A86E0", "#1F4FA8", outline="#123070")
        fill(img, ellipse((17.5, 19, 23, 23.5)), "#5A86E0", "#1F4FA8", outline="#123070")
        line(img, [(13.3, 23), (13.3, 12.5)], "#123070", 1.3)
        line(img, [(22.3, 21), (22.3, 10.5)], "#123070", 1.3)
        fill(img, poly([(12.6, 11.8), (23, 9.8), (23, 13), (12.6, 15)]), "#3A66C8", "#123070")
    return finish(img, m)


def ic_computer():
    img = new()
    m1 = fill(img, rrect((2, 3, 30, 22), 2), "#F2F2EE", "#C9C8BE", outline="#6E6D66")
    sc = fill(img, rrect((4.5, 5.5, 27.5, 19.5), 0.8), "#5FA8F5", "#1D4FB0")
    fill(img, poly([(4.5, 19.5), (12, 13), (17, 17), (21, 14), (27.5, 19.5)]), "#69C25A", "#2F8A2E")
    gloss(img, sc, 0.5, 90)
    fill(img, poly([(12, 22), (20, 22), (21.5, 26), (10.5, 26)]), "#D9D8D0", "#A9A89E")
    m2 = fill(img, rrect((6, 26, 26, 29.5), 1.2), "#EDEDE8", "#B8B7AE", outline="#6E6D66")
    m = Image.new("L", (N, N)); m.paste(m1); m.paste(255, None, m2)
    return finish(img, m)


def ic_hdd():
    img = new()
    m = fill(img, rrect((2, 9, 30, 25), 2.5), "#F0F2F6", "#A3A9B6", outline="#5A6070")
    fill(img, rrect((4, 18, 28, 23), 1), "#C8CDD8", "#8D94A4")
    gloss(img, m, 0.45, 120)
    fill(img, ellipse((23, 19.2, 26, 22.2)), "#9CFF7A", "#25B020")
    for x in range(6, 20, 2):
        line(img, [(x, 19.5), (x, 22)], "#6E7586", 0.6)
    return finish(img, m)


def ic_usbdrive():
    img = new()
    fill(img, rrect((2, 11.5, 10, 20.5), 0.6), "#E8EBF0", "#9AA2B0", outline="#5A6070")
    fill(img, rrect((4, 13.5, 6, 15.5), 0.2), "#404850")
    fill(img, rrect((4, 16.5, 6, 18.5), 0.2), "#404850")
    m = fill(img, rrect((9, 9, 30, 23), 3), "#4D8BEA", "#183F94", outline="#0E2A66")
    gloss(img, m, 0.5, 130)
    fill(img, ellipse((24.5, 14.5, 27.5, 17.5)), "#FFDA6A", "#FF9A10")
    return finish(img)


def ic_floppy():
    img = new()
    m = fill(img, poly([(3, 3), (27, 3), (29, 5), (29, 29), (3, 29)]), "#3E5E9C", "#1D3366", outline="#0F1E40")
    fill(img, rrect((9, 3, 23, 11), 0.5), "#E4E7EC", "#A7ADB8", outline="#5A6070")
    fill(img, rrect((18, 4.5, 21, 9.5), 0.3), "#3E5E9C")
    fill(img, rrect((6, 15, 26, 28), 0.8), "#FFFFFF", "#E2E6EE")
    for yy in (18, 21, 24):
        line(img, [(8, yy), (24, yy)], "#9FB2D6", 0.7)
    return finish(img, m)


def ic_cdrom():
    img = new()
    m = fill(img, ellipse((2, 2, 30, 30)), "#F4F6FA", "#B9C2D2", outline="#6C778C")
    fill(img, ellipse((5, 5, 27, 27)), (190, 230, 255, 255), (255, 210, 240, 255), vertical=False)
    gloss(img, m, 0.5, 90)
    fill(img, ellipse((12, 12, 20, 20)), "#E8ECF2", "#B0B8C8", outline="#6C778C")
    fill(img, ellipse((14.5, 14.5, 17.5, 17.5)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse(box(14.5, 14.5, 17.5, 17.5), fill=(0, 0, 0, 0))
    return finish(img, m)


def ic_notepad():
    img = new()
    m = fill(img, rrect((5, 4, 27, 30), 1), "#FFFFFF", "#EEF1E4", outline="#6D7A5A")
    fill(img, rrect((5, 4, 27, 9), 1), "#7FB3E8", "#2F68B8")
    for x in range(8, 26, 3):
        fill(img, ellipse((x - 1, 2.5, x + 1, 6.5)), "#D0D4DC", "#7A8090")
    for yy in range(13, 28, 3):
        line(img, [(8, yy), (24, yy)], "#9DB8DC", 0.8)
    line(img, [(10.5, 10), (10.5, 29)], "#E88A8A", 0.6)
    return finish(img, m)


def ic_terminal():
    img = new()
    m = fill(img, rrect((2, 4, 30, 28), 2), "#5C88D6", "#1F4FA8", outline="#0E2A66")
    fill(img, rrect((4, 8, 28, 26), 0.8), "#151A22", "#2A3240")
    line(img, [(7, 12), (11, 15), (7, 18)], "#7CFF7C", 1.6)
    line(img, [(13, 18.5), (19, 18.5)], "#E0E6F0", 1.4)
    return finish(img, m)


def ic_paint():
    img = new()
    m = fill(img, ellipse((2, 6, 28, 28)), "#F4D8A8", "#C99A58", outline="#8A5E28")
    fill(img, ellipse((17, 18, 23, 24)), (0, 0, 0, 0))
    colors = [("#FF4040", (6, 11)), ("#FFD020", (11, 8.5)), ("#40C040", (17, 9)), ("#3080FF", (22, 12)), ("#A040E0", (7, 17))]
    for c, (x, y) in colors:
        fill(img, ellipse((x - 2.2, y - 2.2, x + 2.2, y + 2.2)), c)
    fill(img, poly([(20, 23), (29, 3), (31, 4), (23, 25)]), "#C08040", "#704010", outline="#402000")
    fill(img, poly([(18, 22), (21, 22.5), (22.5, 25.5), (19, 29)]), "#303030")
    return finish(img, m)


def ic_calc():
    img = new()
    m = fill(img, rrect((5, 2, 27, 30), 2), "#E9EBEF", "#A9AEB8", outline="#545A66")
    fill(img, rrect((7.5, 4.5, 24.5, 10), 0.8), "#D9F2C8", "#A8CC90", outline="#5F7A50")
    text(img, 22.5, 7.5, "123", 4, "#2F4020", anchor="rm")
    for r in range(4):
        for c in range(4):
            col = ("#FF9A50", "#D8641A") if c == 3 else ("#FFFFFF", "#C9CED8")
            fill(img, rrect((7.5 + c * 4.4, 12 + r * 4.4, 10.9 + c * 4.4, 15.4 + r * 4.4), 0.6), col[0], col[1], outline="#6A7080")
    return finish(img, m)


def ic_chip():
    img = new()
    for i in range(5):
        p = 8 + i * 4
        for (a, b) in ((p, 2), (p, 27)):
            fill(img, rrect((a - 0.8, b, a + 0.8, b + 3), 0.3), "#F8E08A", "#C89A28")
        for (a, b) in ((2, p), (27, p)):
            fill(img, rrect((a, b - 0.8, a + 3, b + 0.8), 0.3), "#F8E08A", "#C89A28")
    m = fill(img, rrect((5, 5, 27, 27), 2), "#4A5466", "#20262F", outline="#0C0F14")
    gloss(img, m, 0.45, 60)
    fill(img, rrect((10, 10, 22, 22), 1), "#B8C4D8", "#7A889E", outline="#4A5466")
    text(img, 16, 16, "i", 8, "#1F4FA8", font=FONT_B)
    return finish(img, m)


def ic_control():
    img = new()
    m = fill(img, rrect((2, 4, 30, 28), 2.5), "#E8EEF8", "#B0BED6", outline="#4E6290")
    gloss(img, m, 0.4, 90)
    for i, (x, kn) in enumerate(((8, 18), (16, 11), (24, 15))):
        line(img, [(x, 8), (x, 24)], "#4E6290", 1.2)
        fill(img, rrect((x - 3, kn - 1.8, x + 3, kn + 1.8), 0.8), ["#FF8A3A", "#42B04A", "#3A86F0"][i], "#404858", outline="#283040")
    return finish(img, m)


def ic_round(c1, c2, outline, glyph, gcol="#FFFFFF", size=17, font=FONT_B, dy=0):
    img = new()
    m = fill(img, ellipse((2, 2, 30, 30)), c1, c2, outline=outline)
    gloss(img, m, 0.5, 120)
    text(img, 16, 16.5 + dy, glyph, size, gcol, font=font)
    return finish(img, m)


def ic_warning():
    img = new()
    m = fill(img, poly([(16, 2.5), (30.5, 28.5), (1.5, 28.5)]), "#FFE36A", "#F0A818", outline="#9A6400")
    text(img, 16, 19.5, "!", 15, "#202020")
    return finish(img, m)


def ic_power(c1, c2, outline, kind):
    img = new()
    m = fill(img, rrect((2, 2, 30, 30), 5), c1, c2, outline=outline)
    gloss(img, m, 0.5, 120)
    d = ImageDraw.Draw(img)
    if kind == "off":
        d.arc(box(9, 9, 23, 23), start=-60, end=240, fill=(255, 255, 255, 255), width=u(2.2))
        line(img, [(16, 6.5), (16, 15)], "#FFFFFF", 2.2)
    else:
        d.arc(box(9, 9, 23, 23), start=-30, end=250, fill=(255, 255, 255, 255), width=u(2.2))
        fill(img, poly([(22.5, 4.5), (25.5, 12.5), (17.5, 11)]), "#FFFFFF")
    return finish(img, m)


def ic_logo():
    img = new()
    m = fill(img, ellipse((1.5, 1.5, 30.5, 30.5)), "#5FD3E8", "#1546A8", outline="#0B2A6A")
    gloss(img, m, 0.55, 140)
    # kingfisher-like swoosh: a wing made of two curves
    fill(img, poly([(5, 20), (12, 13), (19, 10.5), (27, 11), (20, 14), (14, 18.5), (9, 23)]), "#FFFFFF", "#FFE7B0")
    fill(img, poly([(9, 23), (15, 19.5), (22, 18), (26, 19.5), (19, 22), (13, 25)]), "#FFB040", "#F07010")
    return finish(img, m)


def ic_user():
    img = new()
    m = fill(img, rrect((1, 1, 31, 31), 3.5), "#FFFFFF", "#D8E2F2", outline="#5A78B0")
    fill(img, rrect((3, 3, 29, 29), 2.2), "#8CCBFF", "#E6F5FF")
    fill(img, poly([(3, 24), (10, 19), (16, 22), (22, 18), (29, 22), (29, 29), (3, 29)]), "#3E9AD8", "#1E5FA8")
    fill(img, poly([(12, 21), (16, 8), (16, 21)]), "#FFFFFF", "#E0E8F0")
    fill(img, poly([(17, 21), (17, 10), (22.5, 21)]), "#FF8C40", "#E05A10")
    fill(img, poly([(10.5, 21.5), (24, 21.5), (22, 24), (12.5, 24)]), "#8A5A2A", "#5A3410")
    fill(img, ellipse((22, 5, 27, 10)), "#FFF0A0", "#FFC830")
    return finish(img, m)


def ic_usb_symbol():
    img = new()
    line(img, [(16, 4), (16, 26)], "#203040", 2.2)
    line(img, [(16, 20), (9, 14), (9, 10)], "#203040", 2.0)
    line(img, [(16, 17), (23, 12), (23, 9)], "#203040", 2.0)
    fill(img, poly([(16, 1.5), (19.5, 6.5), (12.5, 6.5)]), "#203040")
    fill(img, ellipse((6.8, 6.8, 11.2, 11.2)), "#203040")
    fill(img, rrect((21, 5.5, 25, 9.5), 0.2), "#203040")
    fill(img, ellipse((13, 25, 19, 31)), "#203040")
    return img


def ic_display():
    img = new()
    m = fill(img, rrect((2, 4, 30, 23), 1.5), "#3A3E48", "#15171C", outline="#000000")
    sc = fill(img, rrect((4, 6, 28, 21), 0.5), "#6EC6FF", "#2A5FC8")
    fill(img, poly([(4, 21), (11, 15), (16, 18), (22, 13), (28, 21)]), "#FFB040", "#E05A10")
    gloss(img, sc, 0.5, 80)
    fill(img, rrect((10, 24, 22, 28), 1), "#5A5E68", "#2A2D34", outline="#000000")
    return finish(img, m)


def ic_keyboard():
    img = new()
    m = fill(img, rrect((1, 9, 31, 24), 2), "#F2F2EE", "#BEBDB2", outline="#5E5D56")
    for r in range(3):
        for c in range(9):
            fill(img, rrect((3 + c * 3.1, 11 + r * 3.6, 5.4 + c * 3.1, 13.6 + r * 3.6), 0.4), "#FFFFFF", "#CFCEC4", outline="#7A796F")
    return finish(img, m)


def ic_mouse():
    img = new()
    m = fill(img, ellipse((8, 5, 24, 29)), "#F4F4F0", "#B8B7AE", outline="#5E5D56")
    line(img, [(16, 5), (16, 15)], "#5E5D56", 0.8)
    line(img, [(9, 15), (23, 15)], "#5E5D56", 0.8)
    fill(img, rrect((15, 8, 17, 12.5), 1), "#606060")
    gloss(img, m, 0.4, 120)
    return finish(img, m)


def ic_memory():
    img = new()
    m = fill(img, rrect((1, 10, 31, 22), 1), "#2F9A4A", "#11622A", outline="#0A3A18")
    for i in range(4):
        fill(img, rrect((3.5 + i * 7, 12, 8.5 + i * 7, 18), 0.4), "#2A2D34", "#101216")
    for i in range(14):
        fill(img, rrect((2.5 + i * 2.1, 20, 3.7 + i * 2.1, 22.5), 0.1), "#F8E08A", "#C89A28")
    return finish(img, m)


def ic_hub():
    img = new()
    m = fill(img, rrect((2, 9, 30, 23), 3), "#E8EEF8", "#9EAAC0", outline="#4E5A70")
    gloss(img, m, 0.45, 120)
    for i in range(4):
        fill(img, rrect((5 + i * 6.2, 14, 9.5 + i * 6.2, 18), 0.4), "#2A2D34")
    return finish(img, m)


def ic_help():
    img = new()
    m = fill(img, rrect((5, 3, 27, 29), 1.5), "#4A86E0", "#1A3F8C", outline="#0E2A66")
    fill(img, rrect((5, 25, 27, 29), 1), "#FFFFFF", "#D0D8E8")
    gloss(img, m, 0.4, 100)
    text(img, 16, 14, "?", 15, "#FFFFFF")
    return finish(img, m)


def ic_monitor_search():
    img = ic_computer()
    return img


def ic_game():
    img = new()
    m = fill(img, rrect((3, 4, 21, 28), 2), "#FFFFFF", "#E8E8E8", outline="#505050")
    text(img, 8, 9, "A", 5, "#D02020")
    fill(img, poly([(12, 11), (16, 16), (12, 21), (8, 16)]), "#E03030", "#A01010")
    m2 = fill(img, rrect((11, 6, 29, 30), 2), "#FFFFFF", "#E8E8E8", outline="#505050")
    text(img, 20, 18, "♠", 12, "#202020")
    return finish(img)


def ic_drive_sys():
    img = ic_hdd()
    return img


def ic_recycle(full=False):
    """a glass wastebasket with a curved rim; full: crumpled paper in it"""
    img = new()
    if full:
        # paper behind the rim, well above it: a page and crumpled sheets
        fill(img, poly([(9.5, 0.5), (20, 0), (21, 9), (10.5, 10)]), "#FFFFFF", "#E2E8F2", outline="#5E6A80")
        line(img, [(12, 3), (18.5, 2.6)], "#8A96AA", 0.7)
        line(img, [(12.2, 5), (18.7, 4.6)], "#8A96AA", 0.7)
        fill(img, ellipse((4.5, 2.5, 15, 11.5)), "#FFF6CC", "#E0C870", outline="#8A7020")
        line(img, [(7, 6), (10, 8.5), (12.5, 5.5)], "#A8903A", 0.8)
        fill(img, ellipse((16.5, 2, 27.5, 11)), "#FFFFFF", "#C8D4E6", outline="#5E6A80")
        line(img, [(18.5, 6.5), (21.5, 4.5), (25, 7)], "#8A96AA", 0.8)
    # body: slightly tapered, bluish glass
    body = fill(img, poly([(6, 9), (26, 9), (23.6, 29.4), (8.4, 29.4)]), "#E8F2FC", "#8FA9CC", outline="#41557E")
    gloss(img, body, 0.55, 90)
    # ribs
    for x0, x1 in ((11, 11.8), (16, 16), (21, 20.2)):
        line(img, [(x0, 12.5), (x1, 27)], "#5E7398", 0.9)
        line(img, [(x0 + 0.9, 12.5), (x1 + 0.9, 27)], "#F4F8FF", 0.5)
    # rim and base
    fill(img, rrect((4.5, 7, 27.5, 11), 2), "#F6FAFF", "#A4B8D6", outline="#41557E")
    fill(img, rrect((8.2, 28.2, 23.8, 30.2), 1), "#9AB0D0", "#5C7096", outline="#41557E")
    if full:
        # a sheet hanging over the rim
        fill(img, poly([(20.5, 6), (26.5, 5.5), (25.5, 14), (22.5, 12.5)]), "#FFFFFF", "#E0E6F0", outline="#5E6A80")
    return finish(img)


def ic_web():
    """a globe: the web browser"""
    img = new()
    m = fill(img, ellipse((2, 2, 30, 30)), "#7FD0FF", "#1F5FC0", outline="#0E2A66")
    # continents
    fill(img, poly([(7, 9), (12, 7), (15, 9), (14, 13), (10, 15), (11, 19), (8, 21), (5, 16)]), "#7ED86A", "#2E9A20")
    fill(img, poly([(17, 13), (23, 12), (27, 16), (25, 22), (21, 26), (19, 21), (17, 18)]), "#7ED86A", "#2E9A20")
    fill(img, poly([(18, 5), (22, 5.5), (21, 8), (18, 8)]), "#7ED86A", "#2E9A20")
    d = ImageDraw.Draw(img)
    d.ellipse(box(9, 2.3, 23, 29.7), outline=(255, 255, 255, 120), width=u(0.8))
    line(img, [(2.5, 16), (29.5, 16)], "#FFFFFF", 0.7)
    gloss(img, m, 0.5, 130)
    return finish(img, m)


def ic_nav_arrow(left, c1, c2, outline):
    img = new()
    m = fill(img, ellipse((2, 2, 30, 30)), c1, c2, outline=outline)
    gloss(img, m, 0.5, 120)
    if left:
        fill(img, poly([(8, 16), (16, 8), (16, 12.5), (24, 12.5), (24, 19.5), (16, 19.5), (16, 24)]), "#FFFFFF", "#E8F0E0")
    else:
        fill(img, poly([(24, 16), (16, 8), (16, 12.5), (8, 12.5), (8, 19.5), (16, 19.5), (16, 24)]), "#FFFFFF", "#E8F0E0")
    return finish(img, m)


def ic_reload():
    img = new()
    d = ImageDraw.Draw(img)
    d.arc(box(6, 6, 26, 26), start=-40, end=260, fill=hexc("#2E9A20"), width=u(4.2))
    d.arc(box(7, 7, 25, 25), start=-40, end=260, fill=hexc("#7ED86A"), width=u(2))
    fill(img, poly([(20.5, 2), (29, 10.5), (19, 12.5)]), "#7ED86A", "#2E9A20", outline="#145A0C")
    return finish(img)


def ic_stop():
    img = new()
    pts = []
    import math
    for i in range(8):
        a = math.pi / 8 + i * math.pi / 4
        pts.append((16 + 14 * math.cos(a), 16 + 14 * math.sin(a)))
    m = fill(img, poly(pts), "#FF7A6A", "#C01810", outline="#700800")
    gloss(img, m, 0.5, 110)
    line(img, [(11, 11), (21, 21)], "#FFFFFF", 3)
    line(img, [(21, 11), (11, 21)], "#FFFFFF", 3)
    return finish(img, m)


def ic_home():
    img = new()
    m = fill(img, poly([(16, 3), (30, 15), (26, 15), (26, 29), (6, 29), (6, 15), (2, 15)]), "#FFF2C8", "#E0C070", outline="#7A5A20")
    fill(img, poly([(16, 2), (31, 15.5), (28, 17), (16, 6.5), (4, 17), (1, 15.5)]), "#E86A3A", "#A83010", outline="#701800")
    fill(img, rrect((13, 19, 19, 29), 0.5), "#9A6A30", "#6A4010", outline="#4A2A08")
    fill(img, rrect((20.5, 18, 24.5, 22), 0.3), "#BFE6FF", "#6AAAE0", outline="#3A5A80")
    return finish(img, m)


def ic_network(off=False):
    """two computers on a cable: network connections (off: a red cross)"""
    img = new()
    def pc(x, y):
        m = fill(img, rrect((x, y, x + 14, y + 11), 1.2), "#E8ECF4", "#9AA4B8", outline="#3A4458")
        fill(img, rrect((x + 1.8, y + 1.8, x + 12.2, y + 8.6), 0.4), "#8FD0FF", "#2A6AD0")
        fill(img, rrect((x + 4.5, y + 11, x + 9.5, y + 13.5), 0.3), "#9AA4B8", "#6A7488", outline="#3A4458")
        gloss(img, m, 0.5, 90)
        return m
    line(img, [(9, 16), (9, 22), (23, 22), (23, 26)], "#3A4458", 1.8)
    line(img, [(9, 16), (9, 22), (23, 22), (23, 26)], "#F0C040" if not off else "#A0A0A0", 0.9)
    pc(2, 2)
    pc(16, 16)
    if off:
        line(img, [(12, 19), (20, 27)], "#701800", 3.6)
        line(img, [(20, 19), (12, 27)], "#701800", 3.6)
        line(img, [(12, 19), (20, 27)], "#F04020", 2.2)
        line(img, [(20, 19), (12, 27)], "#F04020", 2.2)
    return finish(img)


def ic_speaker(muted=False):
    """a loudspeaker with sound waves (muted: a red slashed circle instead)"""
    img = new()
    m = fill(img, rrect((3, 11, 10, 21), 1), "#E8ECF4", "#8A94A8", outline="#3A4458")
    fill(img, poly([(9, 11), (17, 4), (17, 28), (9, 21)]), "#D8DEEA", "#6E7890", outline="#3A4458")
    gloss(img, m, 0.5, 90)
    d = ImageDraw.Draw(img)
    if not muted:
        for r, a in ((5, 255), (9, 230), (13, 200)):
            d.arc(box(15 - r, 16 - r, 15 + r, 16 + r), start=-50, end=50, fill=(30, 90, 200, a), width=u(1.9))
    else:
        d.ellipse(box(19, 16, 30, 27), outline=hexc("#C01810"), width=u(2.3))
        line(img, [(21.2, 25), (27.8, 18)], "#C01810", 2.3)
    return finish(img)


def ic_media():
    """Media Player: a round blue-violet button with a play triangle and a note"""
    img = new()
    m = fill(img, ellipse((2, 2, 30, 30)), "#A890FF", "#3A22A0", outline="#1E1060")
    gloss(img, m, 0.5, 120)
    fill(img, poly([(12, 9), (24, 16), (12, 23)]), "#FFFFFF", "#E6E0FF")
    return finish(img, m)


ICONS = [
    ("computer", ic_computer),
    ("hdd", ic_hdd),
    ("usbdrive", ic_usbdrive),
    ("floppy", ic_floppy),
    ("cdrom", ic_cdrom),
    ("folder", lambda: ic_folder()),
    ("folder_open", lambda: ic_folder(open_=True)),
    ("documents", lambda: ic_folder(paper=True)),
    ("file_text", lambda: ic_page("text")),
    ("file", lambda: ic_page("none")),
    ("file_image", lambda: ic_page("image")),
    ("file_app", lambda: ic_page("app")),
    ("file_sys", lambda: ic_page("sys")),
    ("notepad", ic_notepad),
    ("terminal", ic_terminal),
    ("paint", ic_paint),
    ("calc", ic_calc),
    ("sysinfo", ic_chip),
    ("control", ic_control),
    ("info", lambda: ic_round("#6FB2FF", "#1A4FB8", "#0E2A66", "i", size=18, font=FONT_B)),
    ("question", lambda: ic_round("#6FB2FF", "#1A4FB8", "#0E2A66", "?", size=18)),
    ("error", lambda: ic_round("#FF7A6A", "#C01810", "#700800", "✖", size=15)),
    ("warning", ic_warning),
    ("shutdown", lambda: ic_power("#FF8A6A", "#C83010", "#701800", "off")),
    ("restart", lambda: ic_power("#7ED86A", "#2E9A20", "#145A0C", "restart")),
    ("logo", ic_logo),
    ("user", ic_user),
    ("usb", ic_usb_symbol),
    ("display", ic_display),
    ("keyboard", ic_keyboard),
    ("mouse", ic_mouse),
    ("memory", ic_memory),
    ("hub", ic_hub),
    ("help", ic_help),
    ("game", ic_game),
    ("recycle", ic_recycle),
    ("web", ic_web),
    ("back", lambda: ic_nav_arrow(True, "#7ED86A", "#2E9A20", "#145A0C")),
    ("forward", lambda: ic_nav_arrow(False, "#7ED86A", "#2E9A20", "#145A0C")),
    ("reload", ic_reload),
    ("stop", ic_stop),
    ("home", ic_home),
    ("network", ic_network),
    ("network_off", lambda: ic_network(True)),
    ("speaker", ic_speaker),
    ("speaker_muted", lambda: ic_speaker(True)),
    ("file_audio", lambda: ic_page("audio")),
    ("media", ic_media),
    ("recycle_full", lambda: ic_recycle(True)),
]

SIZES = (16, 32, 48)


def main():
    out = sys.argv[1]
    preview = None
    if "--preview" in sys.argv:
        preview = sys.argv[sys.argv.index("--preview") + 1]
    os.makedirs(out, exist_ok=True)
    rendered = []
    for name, fn in ICONS:
        big = fn()
        per = {}
        for sz in SIZES:
            im = big.resize((sz, sz), Image.LANCZOS)
            if sz == 16:
                im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
            per[sz] = im
        rendered.append((name, per))

    with open(os.path.join(out, "icons.h"), "w") as h:
        h.write("/* generated by tools/mkicons.py */\n#ifndef GEN_ICONS_H\n#define GEN_ICONS_H\n#include <stdint.h>\n")
        h.write("typedef struct { int w, h; const uint32_t *px; } icon_t;   /* 0xAARRGGBB, straight alpha */\n")
        h.write("enum {\n")
        for name, _ in rendered:
            h.write("    ICON_%s,\n" % name.upper())
        h.write("    ICON_COUNT\n};\n")
        for sz in SIZES:
            h.write("extern const icon_t icons%d[ICON_COUNT];\n" % sz)
        h.write("#endif\n")
    with open(os.path.join(out, "icons.c"), "w") as c:
        c.write("/* generated by tools/mkicons.py */\n#include \"icons.h\"\n")
        for sz in SIZES:
            for name, per in rendered:
                im = per[sz]
                c.write("static const uint32_t px_%s_%d[%d] = {" % (name, sz, sz * sz))
                raw = im.tobytes()
                data = [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4)]
                for i, (r, g, b, a) in enumerate(data):
                    if i % 12 == 0:
                        c.write("\n")
                    c.write("0x%02x%02x%02x%02x," % (a, r, g, b))
                c.write("\n};\n")
            c.write("const icon_t icons%d[ICON_COUNT] = {\n" % sz)
            for name, _ in rendered:
                c.write("    { %d, %d, px_%s_%d },\n" % (sz, sz, name, sz))
            c.write("};\n")
    if preview:
        cols = 12
        rows = (len(rendered) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * 60, rows * 90), (236, 233, 216, 255))
        for i, (name, per) in enumerate(rendered):
            x, y = (i % cols) * 60, (i // cols) * 90
            sheet.alpha_composite(per[48], (x + 6, y + 4))
            sheet.alpha_composite(per[32], (x + 6, y + 54))
            sheet.alpha_composite(per[16], (x + 42, y + 62))
        sheet.save(preview)


if __name__ == "__main__":
    main()
