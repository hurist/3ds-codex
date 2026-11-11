"""Build bounded, local previews for images referenced by Codex history."""

from base64 import b64decode, b64encode
from binascii import Error as Base64Error
from collections import OrderedDict
from dataclasses import dataclass
from hashlib import sha256
from io import BytesIO
import os
from pathlib import Path
import re
from secrets import token_hex
import stat
import struct
import zlib

try:
    from PIL import Image, ImageOps
except ImportError:
    Image = ImageOps = None


MAX_SOURCE = 12 * 1024 * 1024
FORMATS = ("PNG", "JPEG", "WEBP", "GIF")
DATA_IMAGE = re.compile(r"data:image/(?:png|jpeg|webp|gif);base64,", re.IGNORECASE)
MARKDOWN_IMAGE = re.compile(r"!\[[^\]\n]*\]\((<[^>\n]+>|[^\s)]+)\)")


@dataclass(frozen=True)
class Attachment:
    value: str
    embedded: bool = False


def pixels(image):
    out = bytearray(image.width * image.height * 2)
    rgb = image.tobytes()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i:i + 3]
        struct.pack_into("<H", out, i // 3 * 2, ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3))
    return bytes(out)


def resize(image, bounds):
    thumbnail = image.copy()
    thumbnail.thumbnail(bounds, Image.Resampling.LANCZOS)
    return thumbnail.width, thumbnail.height, pixels(thumbnail)


class ImagePreviews:
    def __init__(self):
        self.entries = OrderedDict()
        self.sources = {}

    @staticmethod
    def open_source(attachment, project, codex_home):
        if not isinstance(attachment.value, str) or not attachment.value:
            return None
        data = DATA_IMAGE.match(attachment.value)
        if data or attachment.embedded:
            value = attachment.value[data.end():] if data else attachment.value
            if len(value) > (MAX_SOURCE + 2) // 3 * 4:
                return None
            decoded = b64decode(value, validate=True)
            return BytesIO(decoded) if len(decoded) <= MAX_SOURCE else None

        if "://" in attachment.value or attachment.value.lower().startswith("data:"):
            return None
        path = Path(attachment.value)
        if not path.is_absolute():
            if project is None:
                return None
            path = project / path
        path = path.resolve(strict=True)
        roots = [codex_home / name for name in ("generated_images", "attachments")]
        if project is not None and project != Path(project.anchor):
            roots.append(project)
        if not any(path.is_relative_to(root.resolve()) for root in roots):
            return None
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_SOURCE:
                return None
            payload = source.read(MAX_SOURCE + 1)
        return BytesIO(payload) if len(payload) <= MAX_SOURCE else None

    def register(self, thread, attachment, project, codex_home):
        if Image is None:
            return None
        try:
            source = self.open_source(attachment, project, codex_home)
            if source is None:
                return None
            key = (thread, sha256(source.getbuffer()).digest())
            token = self.sources.get(key)
            if token:
                self.entries.move_to_end(token)
                return token, self.entries[token][1]
            with Image.open(source, formats=FORMATS) as image:
                if image.width * image.height > 32_000_000:
                    return None
                image = ImageOps.exif_transpose(image)
                image.thumbnail((800, 384), Image.Resampling.LANCZOS)
                if "A" in image.getbands() or "transparency" in image.info:
                    rgba = image.convert("RGBA")
                    background = Image.new("RGBA", rgba.size, (32, 46, 58))
                    image = Image.alpha_composite(background, rgba)
                image = image.convert("RGB")
                width, height, thumb = resize(image, (56, 36))
                full = f"3ds-codex/image/2\n{image.width} {image.height}\n".encode() + zlib.compress(pixels(image), 3)
            token = token_hex(16)
            self.entries[token] = (key, (width, height, b64encode(thumb).decode("ascii")), full)
            self.sources[key] = token
            if len(self.entries) > 24:
                _, (old, _, _) = self.entries.popitem(last=False)
                del self.sources[old]
            return token, self.entries[token][1]
        except (OSError, ValueError, RuntimeError, OverflowError, Base64Error, Image.DecompressionBombError):
            return None

    def get(self, thread, token):
        entry = self.entries.get(token)
        if entry is None or entry[0][0] != thread:
            return None
        self.entries.move_to_end(token)
        return entry[2]
