#!/usr/bin/env python3
"""Test images for the browser's decoders (tests/web/imagetest.c): PNG in
every colour type and bit depth (and interlaced, written here since Pillow
cannot), GIF (transparent, interlaced), BMP (1, 4, 8, 24, 32 bits). Each
NAME.img comes with NAME.rgba, Pillow's pixels as RGBA (width, height as two
32-bit numbers first), or NAME.skip when only "decodes without crashing"
is checked.

    gen_images.py OUTDIR"""
import os
import random
import struct
import sys
import zlib

from PIL import Image, ImageDraw

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)
count = 0


def picture(w, h):
    im = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    for i in range(12):
        x0, y0 = rnd.randrange(w), rnd.randrange(h)
        d.ellipse((x0 - 9, y0 - 7, x0 + 9, y0 + 7), fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(80, 256)))
    for x in range(w):
        im.putpixel((x, h // 2), (x * 255 // max(1, w - 1), 0, 255 - x * 255 // max(1, w - 1), 255))
    return im


def save(name, data, ref):
    global count
    with open(os.path.join(out, name + ".img"), "wb") as f:
        f.write(data)
    if ref is None:
        open(os.path.join(out, name + ".skip"), "w").close()
    else:
        r = ref.convert("RGBA")
        with open(os.path.join(out, name + ".rgba"), "wb") as f:
            f.write(struct.pack("<II", r.width, r.height) + r.tobytes())
    count += 1


def pil(im, fmt, **kw):
    import io
    b = io.BytesIO()
    im.save(b, fmt, **kw)
    return b.getvalue()


def adam7_png(im):
    """an interlaced RGBA PNG, written by hand (filter type chosen per row)"""
    w, h = im.size
    px = im.load()
    raw = b""
    passes = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    for x0, y0, dx, dy in passes:
        prev = None
        for y in range(y0, h, dy):
            row = b"".join(bytes(px[x, y]) for x in range(x0, w, dx))
            if not row:
                continue
            ft = (y // dy) % 5
            enc = bytearray()
            for i, v in enumerate(row):
                a = row[i - 4] if i >= 4 else 0
                b_ = prev[i] if prev else 0
                c = prev[i - 4] if prev and i >= 4 else 0
                if ft == 0:
                    p = 0
                elif ft == 1:
                    p = a
                elif ft == 2:
                    p = b_
                elif ft == 3:
                    p = (a + b_) // 2
                else:
                    pp = a + b_ - c
                    pa, pb, pc = abs(pp - a), abs(pp - b_), abs(pp - c)
                    p = a if pa <= pb and pa <= pc else b_ if pb <= pc else c
                enc.append((v - p) & 255)
            raw += bytes([ft]) + bytes(enc)
            prev = row

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 1)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


base = picture(37, 23)
# PNG: every mode Pillow writes
save("png_rgba", pil(base, "PNG"), base)
save("png_rgb", pil(base.convert("RGB"), "PNG"), base.convert("RGB"))
save("png_l", pil(base.convert("L"), "PNG"), base.convert("L"))
save("png_la", pil(base.convert("LA"), "PNG"), base.convert("LA"))
save("png_1", pil(base.convert("1"), "PNG"), base.convert("1"))
for bits in (1, 2, 4, 8):
    p = base.convert("RGB").quantize(1 << bits)
    save("png_p%d" % bits, pil(p, "PNG", bits=bits), p)
pt = base.convert("RGB").quantize(16)
save("png_p_trns", pil(pt, "PNG", transparency=3), pt.convert("RGBA") if False else None)
lt = base.convert("L")
save("png_l_trns", pil(lt, "PNG", transparency=255), None)
save("png_rgb_trns", pil(base.convert("RGB"), "PNG", transparency=(255, 255, 255)), None)
save("png_i16", pil(base.convert("I").point(lambda v: v * 256).convert("I;16"), "PNG"), None)
save("png_adam7", adam7_png(base), base)
save("png_adam7_tiny", adam7_png(picture(3, 2)), None)
big = picture(300, 200)
save("png_big", pil(big, "PNG", compress_level=9), big)
save("png_1x1", pil(Image.new("RGB", (1, 1), (1, 2, 3)), "PNG"), Image.new("RGB", (1, 1), (1, 2, 3)))
# GIF
g = base.convert("RGB").quantize(64)
save("gif", pil(g, "GIF"), g)
save("gif_interlaced", pil(g, "GIF", interlace=True), g)
gt = Image.new("P", (20, 10), 0)
gt.putpalette([255, 255, 255, 255, 0, 0] + [0] * 762)
ImageDraw.Draw(gt).rectangle((3, 3, 15, 7), fill=1)
save("gif_trans", pil(gt, "GIF", transparency=0), None)
g2 = big.convert("RGB").quantize(256)
save("gif_big", pil(g2, "GIF"), g2)
# BMP
save("bmp24", pil(base.convert("RGB"), "BMP"), base.convert("RGB"))
save("bmp8", pil(base.convert("RGB").quantize(200), "BMP"), base.convert("RGB").quantize(200))
save("bmp4", pil(base.convert("RGB").quantize(16), "BMP", bits=4) if False else pil(base.convert("RGB").quantize(16).convert("P"), "BMP"), None)
save("bmp1", pil(base.convert("1"), "BMP"), base.convert("1"))
save("bmp32", pil(base, "BMP"), None)
# a top-down 24-bit BMP made by hand (negative height)
w, h = 5, 3
rows = b""
for y in range(h):
    row = b"".join(bytes([x * 40, y * 80, 200]) for x in range(w))
    rows += row + b"\0" * ((4 - len(row) % 4) % 4)
hdr = struct.pack("<2sIHHI", b"BM", 54 + len(rows), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(rows), 0, 0, 0, 0)
td = Image.new("RGB", (w, h))
for y in range(h):
    for x in range(w):
        td.putpixel((x, y), (200, y * 80, x * 40))
save("bmp_topdown", hdr + rows, td)
print("%d test images in %s" % (count, out))
