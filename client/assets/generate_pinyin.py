"""Build the offline candidate index for nine-key input from Rime's pinyin_simp dictionary."""

from collections import defaultdict
from pathlib import Path
import re
import struct
import sys


digits = str.maketrans("abcdefghijklmnopqrstuvwxyz", "22233344455566677778889999")
entries = defaultdict(dict)
for row in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    columns = row.split("\t")
    if len(columns) != 3 or not columns[2].isdigit():
        continue
    word, spelling, weight = columns
    key = spelling.replace(" ", "")
    if not re.fullmatch(r"[a-z]{1,24}", key) or not 1 <= len(word) <= min(4, len(key)):
        continue
    if not all(0x3400 <= ord(char) <= 0x9FFF for char in word):
        continue
    key = key.translate(digits)
    entries[key][word] = max(int(weight), entries[key].get(word, 0))

index = bytearray(struct.pack("<I", len(entries)))
pool = bytearray()
for key, words in sorted(entries.items()):
    key_offset = len(pool)
    pool.extend(key.encode("ascii") + b"\0")
    word_offset = len(pool)
    ranked = sorted(words, key=lambda word: (-words[word], word))[:64]
    pool.extend("\0".join(ranked).encode("utf-8") + b"\0\0")
    index.extend(struct.pack("<III", key_offset, word_offset, words[ranked[0]]))

Path(__file__).with_name("pinyin.bin").write_bytes(index + pool)
