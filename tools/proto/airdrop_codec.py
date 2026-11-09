"""Reference codecs for the AirDrop upload path, written from docs/PROTOCOL.md.

Executable specification and fixture generator. Not shipped to users.
Formats: HTTP chunked transfer coding, "dvzip" block container, cpio "odc".
All layouts are FACT for iOS 26.5.2 (docs/FINDINGS.md F-002).
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

DVZIP_BLOCK = 131072          # every block inflates to 128 KiB except first/last
DVZIP_STORED = 0x80000000     # header bit 31: payload stored, not deflated
CPIO_MAGIC = b"070707"
CPIO_TRAILER = "TRAILER!!!"
CPIO_HEADER_LEN = 76


class FormatError(ValueError):
    pass


# ---------------------------------------------------------------- chunked

def chunk_encode(data: bytes, chunk_size: int = 16371, terminate: bool = True) -> bytes:
    out = bytearray()
    for i in range(0, len(data), chunk_size):
        part = data[i:i + chunk_size]
        out += b"%x\r\n" % len(part) + part + b"\r\n"
    if terminate:
        out += b"0\r\n\r\n"
    return bytes(out)


def chunk_decode(body: bytes) -> tuple[bytes, bool]:
    """Return (payload, terminated).

    `terminated` is False when the 0-chunk is missing or the last chunk is cut
    short. Both happen with iOS 26 and are not errors by themselves.
    """
    out = bytearray()
    i = 0
    while True:
        j = body.find(b"\r\n", i)
        if j < 0:
            return bytes(out), False
        try:
            n = int(body[i:j].split(b";")[0], 16)
        except ValueError as e:
            raise FormatError("bad chunk size line") from e
        if n == 0:
            return bytes(out), True
        start = j + 2
        if start + n > len(body):
            out += body[start:]
            return bytes(out), False
        out += body[start:start + n]
        i = start + n + 2


# ------------------------------------------------------------------ dvzip

def dvzip_encode(raw: bytes, stored_from_block: int | None = None) -> bytes:
    """Encode 128 KiB input blocks. Blocks with index >= stored_from_block are
    stored; the others are deflated (the phone mixes both in one stream)."""
    out = bytearray()
    for n, i in enumerate(range(0, len(raw), DVZIP_BLOCK)):
        part = raw[i:i + DVZIP_BLOCK]
        if stored_from_block is not None and n >= stored_from_block:
            out += struct.pack(">I", DVZIP_STORED | len(part)) + part
        else:
            z = zlib.compress(part)
            out += struct.pack(">I", len(z)) + z
    return bytes(out)


def dvzip_decode(data: bytes) -> tuple[bytes, int]:
    """Return (inflated bytes, count of trailing bytes not forming a whole block).

    A deflated payload may be LARGER than the 128 KiB it inflates to.
    """
    out = bytearray()
    i = 0
    while i + 4 <= len(data):
        (word,) = struct.unpack_from(">I", data, i)
        length = word & 0x7FFFFFFF
        if length == 0:
            raise FormatError("empty dvzip block")
        if i + 4 + length > len(data):
            break
        payload = data[i + 4:i + 4 + length]
        if word & DVZIP_STORED:
            out += payload
        else:
            try:
                out += zlib.decompress(payload)
            except zlib.error as e:
                raise FormatError("corrupt zlib block") from e
        i += 4 + length
    return bytes(out), len(data) - i


# ------------------------------------------------------------------- cpio

@dataclass
class CpioEntry:
    name: str
    mode: int
    data: bytes

    @property
    def is_dir(self) -> bool:
        return (self.mode & 0o170000) == 0o040000

    @property
    def is_appledouble(self) -> bool:
        return self.name.rsplit("/", 1)[-1].startswith("._")


def _cpio_header(name: str, mode: int, size: int, ino: int, nlink: int = 1) -> bytes:
    nb = name.encode() + b"\0"
    h = "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o" % (
        0, ino, mode, 0, 0, nlink, 0, 0, len(nb), size)
    assert len(h) == CPIO_HEADER_LEN
    return h.encode() + nb


def cpio_encode(entries: list[CpioEntry], pad_to: int = 512) -> bytes:
    out = bytearray()
    for ino, e in enumerate(entries):
        out += _cpio_header(e.name, e.mode, len(e.data), ino, 2 if e.is_dir else 1) + e.data
    out += _cpio_header(CPIO_TRAILER, 0, 0, len(entries))
    if pad_to and len(out) % pad_to:
        out += b"\0" * (pad_to - len(out) % pad_to)
    return bytes(out)


def cpio_decode(raw: bytes) -> tuple[list[CpioEntry], bool]:
    """Return (entries, complete).

    `complete` is True only if TRAILER!!! was reached. A member cut short by a
    stalled upload is not returned.
    """
    entries: list[CpioEntry] = []
    p = 0
    while True:
        if p + CPIO_HEADER_LEN > len(raw):
            return entries, False
        if raw[p:p + 6] != CPIO_MAGIC:
            raise FormatError("bad cpio magic at %d" % p)
        try:
            f = raw[p:p + CPIO_HEADER_LEN].decode("ascii")
            mode = int(f[18:24], 8)
            namelen = int(f[59:65], 8)
            size = int(f[65:76], 8)
        except ValueError as e:
            raise FormatError("bad cpio header") from e
        end_name = p + CPIO_HEADER_LEN + namelen
        if namelen == 0 or end_name > len(raw):
            return entries, False
        name = raw[p + CPIO_HEADER_LEN:end_name - 1].decode("utf-8", "replace")
        if name == CPIO_TRAILER:
            return entries, True
        if end_name + size > len(raw):
            return entries, False
        entries.append(CpioEntry(name, mode, raw[end_name:end_name + size]))
        p = end_name + size


_RESERVED = ({"CON", "PRN", "AUX", "NUL"}
             | {"COM%d" % i for i in range(1, 10)}
             | {"LPT%d" % i for i in range(1, 10)})


def safe_relative_path(name: str) -> str | None:
    """Map an archive member name to a safe relative path, or None to skip it.

    Skips: the root entry, absolute paths, drive letters, '..', backslashes,
    control characters, AppleDouble sidecars ('._*'), Windows reserved device
    names and names ending in a dot or space.
    """
    while name.startswith("./"):
        name = name[2:]
    if name in ("", "."):
        return None
    if name.startswith("/") or "\\" in name or ":" in name:
        return None
    parts = name.split("/")
    for part in parts:
        if part in ("", ".", "..") or any(ord(c) < 32 for c in part):
            return None
        if part.startswith("._") or part.endswith((" ", ".")):
            return None
        if part.split(".")[0].upper() in _RESERVED:
            return None
    return "/".join(parts)
