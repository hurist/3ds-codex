.section .rodata
.global glyphs
glyphs:
.incbin "../assets/glyphs.bin"

.balign 4
.global pinyin_data
pinyin_data:
.incbin "../assets/pinyin.bin"
