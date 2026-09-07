"""GearSF7000 icon candidates.

The SF-7000 wordmark is rebuilt from primitives rather than set in a font. The
original is a geometric face with thick strokes, rounded corners and a thin
inline running inside each stroke. Each glyph is drawn as a union of
rectangles/polygons, rounded by a blur-and-threshold pass, and the inline is
the difference between two eroded copies of that shape.

TMS9918 palette values come from src/Video.h.
"""
from PIL import Image, ImageDraw, ImageChops, ImageFilter
import sys

OUT = 1024
WORK = 2048

PAL = [(0,0,0),(17,17,17),(51,187,68),(153,221,153),(85,85,221),(136,136,238),
       (187,68,51),(85,238,238),(238,102,68),(255,170,119),(187,187,85),
       (221,221,136),(51,170,51),(204,136,204),(238,238,238),(255,255,255)]

BAR = 0.20          # stroke thickness / cap height
R = 0.34            # zero corner radius / cap height

GLYPHS = {
    "S": (0.88,
          [(0, 0, 0.88, BAR),
           (0, 0, BAR, 0.5 + BAR / 2),
           (0, 0.5 - BAR / 2, 0.88, 0.5 + BAR / 2),
           (0.88 - BAR, 0.5 - BAR / 2, 0.88, 1.0),
           (0, 1.0 - BAR, 0.88, 1.0)], [], [], []),
    "F": (0.76,
          [(0, 0, BAR, 1.0),
           (0, 0, 0.76, BAR),
           (0, 0.5 - BAR / 2, 0.64, 0.5 + BAR / 2)], [], [], []),
    "-": (0.42, [(0.03, 0.5 - BAR / 2, 0.39, 0.5 + BAR / 2)], [], [], []),
    "7": (0.86,
          [(0, 0, 0.86, BAR)],
          [[(0.86 - BAR, BAR), (0.86, BAR), (0.30 + BAR, 1.0), (0.30, 1.0)]],
          [], []),
    "G": (0.86,
          [(0, 0, 0.86, BAR), (0, 0, BAR, 1.0), (0, 1.0 - BAR, 0.86, 1.0),
           (0.86 - BAR, 0.5 - BAR / 2, 0.86, 1.0),
           (0.46, 0.5 - BAR / 2, 0.86, 0.5 + BAR / 2)], [], [], []),
    "E": (0.76,
          [(0, 0, BAR, 1.0), (0, 0, 0.76, BAR),
           (0, 0.5 - BAR / 2, 0.64, 0.5 + BAR / 2),
           (0, 1.0 - BAR, 0.76, 1.0)], [], [], []),
    "A": (0.82,
          [(0, 0, 0.82, BAR), (0, 0, BAR, 1.0), (0.82 - BAR, 0, 0.82, 1.0),
           (0, 0.5 - BAR / 2, 0.82, 0.5 + BAR / 2)], [], [], []),
    "R": (0.82,
          [(0, 0, BAR, 1.0), (0, 0, 0.82, BAR),
           (0.82 - BAR, 0, 0.82, 0.5 + BAR / 2),
           (0, 0.5 - BAR / 2, 0.82, 0.5 + BAR / 2)],
          # diagonal leg, otherwise a full right stem makes the R read as an A
          [[(0.36, 0.5 - BAR / 2), (0.36 + BAR, 0.5 - BAR / 2),
            (0.82, 1.0), (0.82 - BAR, 1.0)]], [], []),
    "0": (0.86, [], [], [(0, 0, 0.86, 1.0)],
          [(BAR, BAR, 0.86 - BAR, 1.0 - BAR)]),
}


def erode(mask, px):
    out, left = mask, int(px)
    while left > 0:
        step = min(4, left)
        out = out.filter(ImageFilter.MinFilter(2 * step + 1))
        left -= step
    return out


def glyph_mask(c, cap, round_px):
    adv, rects, polys, rounds, holes = GLYPHS[c]
    pad = int(round_px * 3)
    m = Image.new("L", (int(adv * cap) + 2 * pad, int(cap) + 2 * pad), 0)
    d = ImageDraw.Draw(m)
    for (x0, y0, x1, y1) in rects:
        d.rectangle([x0 * cap + pad, y0 * cap + pad,
                     x1 * cap + pad, y1 * cap + pad], fill=255)
    for poly in polys:
        d.polygon([(px * cap + pad, py * cap + pad) for px, py in poly], fill=255)
    for (x0, y0, x1, y1) in rounds:
        d.rounded_rectangle([x0 * cap + pad, y0 * cap + pad,
                             x1 * cap + pad, y1 * cap + pad],
                            radius=cap * R, fill=255)
    for (x0, y0, x1, y1) in holes:
        d.rounded_rectangle([x0 * cap + pad, y0 * cap + pad,
                             x1 * cap + pad, y1 * cap + pad],
                            radius=cap * R * 0.55, fill=0)
    if round_px:
        m = m.filter(ImageFilter.GaussianBlur(round_px))
        m = m.point(lambda v: 255 if v >= 128 else 0)
    return m, pad


def line_width(text, cap, tracking):
    return sum(GLYPHS[c][0] for c in text) * cap + tracking * cap * (len(text) - 1)



def cap_for_width(text, target_w, tracking):
    """Cap height that makes `text` come out exactly `target_w` wide."""
    units = sum(GLYPHS[c][0] for c in text) + tracking * (len(text) - 1)
    return target_w / units


def draw_line(img, text, cap, tracking, left, top, colours=None,
              inline_colour=None):
    """Paints each glyph solid, then cuts a thin inline inside the stroke."""
    round_px = max(2, int(cap * 0.045))
    inset = max(2, int(cap * 0.045))
    thick = max(2, int(cap * 0.028))

    x = left
    for i, c in enumerate(text):
        m, pad = glyph_mask(c, cap, round_px)
        col = (255, 255, 255) if colours is None else colours[i % len(colours)]
        pos = (int(x) - pad, int(top) - pad)
        img.paste(Image.new("RGB", m.size, col), pos, m)
        if inline_colour is not None:
            band = ImageChops.subtract(erode(m, inset), erode(m, inset + thick))
            img.paste(Image.new("RGB", m.size, inline_colour), pos, band)
        x += GLYPHS[c][0] * cap + tracking * cap


def squircle(size, ratio=0.2237):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1],
                                        radius=int(size * ratio), fill=255)
    return m


def vgrad(size, top, bottom):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        t = y / (size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t)
                                 for i in range(3)))
    return g.resize((size, size))


def finish(img, name):
    canvas = Image.new("RGBA", (OUT, OUT), (0, 0, 0, 0))
    inset = int(OUT * 0.055)
    side = OUT - 2 * inset
    canvas.paste(img.resize((side, side), Image.LANCZOS), (inset, inset),
                 squircle(side))
    canvas.save(name)
    print("wrote", name)


TRACK = 0.13
BLUE_TOP, BLUE_BOT = (30, 66, 142), (8, 14, 34)


def cog_mask(size, teeth=8, r_out=0.50, r_root=0.34, r_hub=0.195,
             base_frac=0.48, tip_frac=0.60, round_frac=0.035):
    """Chunky gear: teeth flare outwards to a rounded tip, disc body, hub hole.

    Drawn rather than traced from a system symbol so the icon carries no
    third-party font outline, and so tooth mass can be tuned for legibility at
    16 px - thin teeth were the problem with the first attempt.
    """
    import math
    ss = int(size) * 4                  # supersample, then threshold back down
    m = Image.new("L", (ss, ss), 0)
    d = ImageDraw.Draw(m)
    c = ss / 2
    pitch = 2 * math.pi / teeth
    for i in range(teeth):
        a = pitch * i
        hb, ht = pitch * base_frac / 2, pitch * tip_frac / 2
        pts = [(r_root, a - hb), (r_out, a - ht),
               (r_out, a + ht), (r_root, a + hb)]
        d.polygon([(c + math.cos(t) * r * ss, c + math.sin(t) * r * ss)
                   for r, t in pts], fill=255)
    d.ellipse([c - r_root * ss, c - r_root * ss,
               c + r_root * ss, c + r_root * ss], fill=255)
    d.ellipse([c - r_hub * ss, c - r_hub * ss,
               c + r_hub * ss, c + r_hub * ss], fill=0)
    m = m.filter(ImageFilter.GaussianBlur(ss * round_frac))
    m = m.point(lambda v: 255 if v >= 128 else 0)
    return m.resize((int(size), int(size)), Image.LANCZOS)


def base(img, stripe_top=0.800, stripe_h=0.075, word_cy=0.635):
    """Shared bottom half: SF-7000 over the TMS9918 palette stripe."""
    cap = cap_for_width("SF-7000", WORK * 0.84, TRACK)
    w = line_width("SF-7000", cap, TRACK)
    draw_line(img, "SF-7000", cap, TRACK, (WORK - w) / 2,
              WORK * word_cy - cap / 2)
    d = ImageDraw.Draw(img)
    picks = [6, 8, 10, 2, 7, 4, 13]
    bw = WORK / len(picks)
    for i, p in enumerate(picks):
        d.rectangle([i * bw, WORK * stripe_top,
                     (i + 1) * bw, WORK * (stripe_top + stripe_h)], fill=PAL[p])
    return img


COG_SIZE, COG_CY = 0.375, 0.275


def paint_cog(img, fill):
    size = WORK * COG_SIZE
    m = cog_mask(size)
    pos = (int((WORK - size) / 2), int(WORK * COG_CY - size / 2))
    layer = Image.new("RGB", m.size, fill) if isinstance(fill, tuple) else fill
    img.paste(layer, pos, m)


# --- palette cog ------------------------------------------------------------
def candidate_cog_colour():
    img = vgrad(WORK, BLUE_TOP, BLUE_BOT)
    size = int(WORK * COG_SIZE)
    stripes = Image.new("RGB", (size, size))
    sd = ImageDraw.Draw(stripes)
    picks = [7, 2, 11, 9, 13]
    bw = size / len(picks)
    for i, p in enumerate(picks):
        sd.rectangle([i * bw, 0, (i + 1) * bw, size], fill=PAL[p])
    paint_cog(img, stripes)
    return base(img)


# --- white cog --------------------------------------------------------------
def candidate_cog():
    img = vgrad(WORK, BLUE_TOP, BLUE_BOT)
    paint_cog(img, (255, 255, 255))
    return base(img)


# --- GEAR wordmark ----------------------------------------------------------
def candidate_gear_word():
    img = vgrad(WORK, BLUE_TOP, BLUE_BOT)
    cap = cap_for_width("GEAR", WORK * 0.56, TRACK)
    w = line_width("GEAR", cap, TRACK)
    draw_line(img, "GEAR", cap, TRACK, (WORK - w) / 2, WORK * 0.285 - cap / 2)
    return base(img)


finish(candidate_cog_colour(), sys.argv[1])
finish(candidate_cog(), sys.argv[2])
finish(candidate_gear_word(), sys.argv[3])
