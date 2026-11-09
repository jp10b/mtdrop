import os
import struct
import zlib

import pytest

from airdrop_codec import (CpioEntry, FormatError, chunk_decode, chunk_encode, cpio_decode,
                           cpio_encode, dvzip_decode, dvzip_encode, safe_relative_path)


def synthetic_upload(size=300_000):
    photo = b"\xff\xd8" + os.urandom(size - 4) + b"\xff\xd9"
    entries = [CpioEntry(".", 0o040700, b""),
               CpioEntry("./IMG_0001.JPG", 0o100644, photo),
               CpioEntry("./._IMG_0001.JPG", 0o100644, b"\x00\x05\x16\x07" + bytes(60))]
    return photo, entries


def test_roundtrip_like_the_phone():
    photo, entries = synthetic_upload()
    archive = cpio_encode(entries)
    assert len(archive) % 512 == 0
    wire = chunk_encode(dvzip_encode(archive, stored_from_block=1))
    body, terminated = chunk_decode(wire)
    assert terminated
    raw, leftover = dvzip_decode(body)
    assert leftover == 0 and raw == archive
    got, complete = cpio_decode(raw)
    assert complete
    assert [e.name for e in got] == [".", "./IMG_0001.JPG", "./._IMG_0001.JPG"]
    assert got[1].data == photo and got[0].is_dir and got[2].is_appledouble


def test_missing_zero_chunk_still_yields_complete_archive():
    _, entries = synthetic_upload()
    wire = chunk_encode(dvzip_encode(cpio_encode(entries)), terminate=False)
    body, terminated = chunk_decode(wire)
    assert not terminated
    raw, leftover = dvzip_decode(body)
    assert leftover == 0 and cpio_decode(raw)[1] is True


def test_stalled_upload_is_incomplete_and_returns_no_partial_file():
    _, entries = synthetic_upload()
    wire = chunk_encode(dvzip_encode(cpio_encode(entries), stored_from_block=0), terminate=False)
    body, terminated = chunk_decode(wire[: len(wire) // 2])
    assert not terminated
    raw, leftover = dvzip_decode(body)
    assert leftover > 0
    got, complete = cpio_decode(raw)
    assert not complete and [e.name for e in got] == ["."]


def test_zlib_payload_larger_than_block_is_accepted():
    block = os.urandom(131072)                       # incompressible
    z = zlib.compress(block)
    assert len(z) > 131072
    raw, leftover = dvzip_decode(struct.pack(">I", len(z)) + z)
    assert raw == block and leftover == 0


def test_corrupt_inputs_raise():
    with pytest.raises(FormatError):
        dvzip_decode(struct.pack(">I", 4) + b"nope")
    with pytest.raises(FormatError):
        cpio_decode(b"x" * 100)
    with pytest.raises(FormatError):
        chunk_decode(b"zz\r\nabc\r\n")


@pytest.mark.parametrize("name,expected", [
    ("./IMG_0001.JPG", "IMG_0001.JPG"),
    ("./dir/a.txt", "dir/a.txt"),
    (".", None), ("./._IMG_0001.JPG", None),
    ("../evil", None), ("./a/../../evil", None), ("/etc/passwd", None),
    ("C:/x", None), ("a\\b", None), ("./NUL.txt", None), ("./com1", None),
    ("./trailing.", None), ("./ok\x01", None),
])
def test_safe_relative_path(name, expected):
    assert safe_relative_path(name) == expected
