"""Rasterize the Noto Sans CJK SC face into 16×16 four-level antialiased glyphs."""

from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFont


RANGES = (
    (0x0020, 0x007E), (0x00A0, 0x024F), (0x2000, 0x206F), (0x2190, 0x2193),
    (0x3000, 0x303F), (0x3400, 0x4DBF), (0x4E00, 0x9FFF),
    (0xFF00, 0xFFEF),
)

font = ImageFont.truetype(sys.argv[1], 15, index=int(sys.argv[2]), layout_engine=ImageFont.Layout.BASIC)
with Path(__file__).with_name("glyphs.bin").open("wb") as output:
    for first, last in RANGES:
        for codepoint in range(first, last + 1):
            glyph = Image.new("L", (16, 16))
            canvas = ImageDraw.Draw(glyph)
            width = 8 if codepoint <= 0x024F else 16
            left, _, right, _ = font.getbbox(chr(codepoint), anchor="ls")
            canvas.text(((width - right - left) // 2, 13), chr(codepoint), 255, font=font, anchor="ls")
            levels = [(value * 3 + 127) // 255 for value in glyph.tobytes()]
            output.write(bytes(sum(levels[i + j] << (6 - j * 2) for j in range(4))
                               for i in range(0, len(levels), 4)))
