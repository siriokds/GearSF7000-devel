"""Contact sheet: each candidate at the sizes macOS actually renders."""
from PIL import Image, ImageDraw

CANDS = [("1  GEAR sopra", "j_gearword.png"),
         ("2  ingranaggio bianco", "k_cog.png"),
         ("3  ingranaggio palette", "l_cog_colour.png")]
SIZES = [512, 128, 64, 32, 16]
BG = (28, 28, 32)
PAD = 28

rows = []
for label, path in CANDS:
    img = Image.open(path).convert("RGBA")
    rows.append((label, img))

col_w = [max(s, 64) + PAD for s in SIZES]
W = PAD + sum(col_w)
row_h = 512 + PAD * 2 + 22
H = PAD + len(rows) * row_h

sheet = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(sheet)

y = PAD
for label, img in rows:
    d.text((PAD, y - 2), label, fill=(210, 214, 224))
    x = PAD
    for i, s in enumerate(SIZES):
        thumb = img.resize((s, s), Image.LANCZOS)
        # centre each thumbnail in its column, aligned on a common baseline
        cx = x + (col_w[i] - PAD) // 2 - s // 2
        cy = y + 22 + (512 - s) // 2
        sheet.paste(thumb, (cx, cy), thumb)
        d.text((x + (col_w[i] - PAD) // 2 - 10, y + 22 + 512 + 6),
               f"{s}px", fill=(130, 136, 150))
        x += col_w[i]
    y += row_h

sheet.save("sheet.png")
print("wrote sheet.png", sheet.size)
