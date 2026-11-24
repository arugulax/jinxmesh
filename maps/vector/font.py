"""font.py <out.h>: the two bitmap fonts the vector map draws its names with, as a C++ header.

Made from Liberation Sans Bold (SIL Open Font License), drawn without smoothing at two sizes. Per glyph: its
width, then one uint16 per row (bit 15 is the left pixel). ASCII 32 to 126.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

TTF = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"


def font(name, size):
    f = ImageFont.truetype(TTF, size)
    ascent, descent = f.getmetrics()
    height = ascent + descent
    glyphs = []
    for code in range(32, 127):
        ch = chr(code)
        width = max(1, round(f.getlength(ch)))
        img = Image.new("1", (16, height), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((0, 0), ch, font=f, fill=1)
        rows = []
        for y in range(height):
            bits = 0
            for x in range(16):
                if img.getpixel((x, y)):
                    bits |= 0x8000 >> x
            rows.append(bits)
        glyphs.append((min(width, 16), rows))
    # rows that no glyph uses, at the top and the bottom
    top = 0
    while all(g[1][top] == 0 for g in glyphs):
        top += 1
    bottom = height
    while all(g[1][bottom - 1] == 0 for g in glyphs):
        bottom -= 1
    out = [f"constexpr uint8_t {name}Height = {bottom - top};", f"const uint16_t {name}[95][{bottom - top + 1}] = {{"]
    for code, (width, rows) in zip(range(32, 127), glyphs):
        cells = ", ".join(f"0x{r:04X}" for r in rows[top:bottom])
        label = chr(code) if chr(code) not in "\\" else "backslash"
        out.append(f"    {{{width}, {cells}}}, // {label}")
    out.append("};")
    return "\n".join(out)


open(sys.argv[1], "w").write(
    "// Made by kbview-harness/maps/vector/font.py from Liberation Sans Bold (SIL Open Font License). Do not edit.\n"
    "#pragma once\n#include <cstdint>\n\nnamespace vectormapfont\n{\n" + font("small", 10) + "\n\n" + font("large", 13) + "\n} // namespace vectormapfont\n"
)
