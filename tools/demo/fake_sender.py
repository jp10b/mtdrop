"""Pretend to be a phone: find an AirDrop-compatible receiver on one interface and
send it a small generated PNG, using the request shapes from docs/FINDINGS.md F-002.

For testing our own receiver on the same machine or LAN segment. Everything sent is
synthetic. Usage:

    python tools/demo/fake_sender.py --interface <ipv6 interface index> [--name "Test Phone"]

Exit code 0 = receiver answered 200 to the upload.
"""
from __future__ import annotations

import argparse
import os
import plistlib
import socket
import ssl
import struct
import sys
import time
import uuid
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))
from airdrop_codec import CpioEntry, chunk_encode, cpio_encode, dvzip_encode  # noqa: E402


def make_png(width: int = 480, height: int = 320) -> bytes:
    """A valid RGB PNG with a simple gradient, built by hand (no imaging library)."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes((x * 255 // width, y * 255 // height, 150))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b""))


def read_name(b: bytes, o: int, depth: int = 0) -> tuple[str, int]:
    out = []
    while depth < 10:
        n = b[o]
        if n == 0:
            o += 1
            break
        if n & 0xC0:
            out.append(read_name(b, ((n & 0x3F) << 8) | b[o + 1], depth + 1)[0])
            o += 2
            break
        out.append(b[o + 1:o + 1 + n].decode())
        o += 1 + n
    return ".".join(out), o


def discover(index: int, timeout: float) -> tuple[str, int]:
    """Ask for _airdrop._tcp and return (address, port) from the first full answer."""
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("::", 5353))
    group = socket.inet_pton(socket.AF_INET6, "ff02::fb")
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, group + struct.pack("@I", index))
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, index)
    s.settimeout(0.5)
    query = bytes(4) + b"\x00\x01" + bytes(6) + b"\x08_airdrop\x04_tcp\x05local\x00\x00\x0c\x00\x01"
    deadline = time.time() + timeout
    next_query = 0.0
    while time.time() < deadline:
        if time.time() >= next_query:
            s.sendto(query, ("ff02::fb", 5353, 0, index))
            next_query = time.time() + 1.5
        try:
            d, _ = s.recvfrom(9000)
        except socket.timeout:
            continue
        flags, _, answers = struct.unpack_from(">HHH", d, 2)
        if not flags & 0x8000:
            continue
        o, port, addr, ours = 12, None, None, False
        try:
            for _ in range(answers):
                name, o = read_name(d, o)
                rtype, _, _, rlen = struct.unpack_from(">HHIH", d, o)
                rdata = d[o + 10:o + 10 + rlen]
                o += 10 + rlen
                ours = ours or name.endswith("_airdrop._tcp.local")
                if rtype == 33:
                    port = struct.unpack_from(">H", rdata, 4)[0]
                elif rtype == 28:
                    addr = socket.inet_ntop(socket.AF_INET6, rdata)
        except (IndexError, struct.error, UnicodeDecodeError):
            continue
        if ours and port and addr:
            return addr, port
    raise SystemExit("no AirDrop-compatible receiver announced itself on that interface")


def connect(addr: str, port: int, index: int) -> ssl.SSLSocket:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE          # receivers use self-signed certificates
    raw = socket.socket(socket.AF_INET6)
    raw.settimeout(90)
    raw.connect((addr, port, 0, index))
    return ctx.wrap_socket(raw)


def request(path: str, headers: str, connection: str, body: bytes, chunk: int) -> bytes:
    head = f"POST {path} HTTP/1.1\r\nUser-Agent: AirDrop/1.0\r\n{headers}Connection: {connection}\r\nTransfer-Encoding: chunked\r\n\r\n"
    return head.encode() + chunk_encode(body, chunk)


def response(t: ssl.SSLSocket) -> tuple[int, bytes]:
    buf = b""
    while b"\r\n\r\n" not in buf:
        part = t.recv(4096)
        if not part:
            raise SystemExit("receiver closed the connection without answering")
        buf += part
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    length = next((int(l.split(b":")[1]) for l in lines if l.lower().startswith(b"content-length")), 0)
    while len(rest) < length:
        rest += t.recv(4096)
    return int(lines[0].split()[1]), rest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interface", type=int, required=True, help="IPv6 interface index (see: mtdrop airdropd --list-interfaces)")
    ap.add_argument("--name", default="Test Phone")
    ap.add_argument("--file-name", default="mtdrop-test.png")
    ap.add_argument("--wait", type=float, default=15, help="seconds to wait for the receiver to appear")
    a = ap.parse_args()

    addr, port = discover(a.interface, a.wait)
    print(f"found a receiver, port {port}")

    t = connect(addr, port, a.interface)
    status, body = response_after(t, request("/Discover", "", "close", plistlib.dumps(
        {"SenderRecordData": bytes(64), "DeviceSupportFlags": 111611}, fmt=plistlib.FMT_BINARY), 16371))
    t.close()
    if status != 200:
        raise SystemExit(f"/Discover -> {status}")
    print("receiver calls itself:", plistlib.loads(body).get("ReceiverComputerName"))

    photo = make_png()
    transfer_id = str(uuid.uuid4()).upper()
    ask = plistlib.dumps({
        "TransferID": {"id": transfer_id},
        "TransferType": {"files": {}},
        "SenderID": "0123456789ab",
        "BundleID": "com.apple.mobileslideshow",
        "SenderComputerName": a.name,
        "SenderModelName": "iPhone",
        "Items": [],
        "Files": [{"FileName": a.file_name, "FileType": "public.png", "FileSize": len(photo),
                   "FileBomPath": "./" + a.file_name, "FileIsDirectory": False, "ShouldConvertMediaFormats": False}],
        "ConvertMediaFormats": False,
    }, fmt=plistlib.FMT_BINARY, sort_keys=False)
    t = connect(addr, port, a.interface)
    print("asking... (answer the prompt on the receiver)")
    status, _ = response_after(t, request("/Ask", "", "keep-alive", ask, 131072))
    if status != 200:
        print(f"declined (HTTP {status})")
        return 1
    archive = cpio_encode([
        CpioEntry(".", 0o040700, b""),
        CpioEntry("./" + a.file_name, 0o100644, photo),
        CpioEntry("./._" + a.file_name, 0o100644, b"\x00\x05\x16\x07" + bytes(60)),
    ])
    headers = (f"TotalBytes: {len(photo)}\r\nContent-Type: application/x-dvzip\r\n"
               f"SenderPseudonym: pseud:AAAAAAAAAAAAAAAAAAAAAA\r\nSenderPushToken: {'0' * 64}\r\nTransferID: {transfer_id}\r\n")
    status, _ = response_after(t, request("/Upload", headers, "keep-alive", dvzip_encode(archive), 16371))
    print(f"upload -> HTTP {status} ({len(photo)} bytes)")
    return 0 if status == 200 else 1


def response_after(t: ssl.SSLSocket, data: bytes) -> tuple[int, bytes]:
    t.sendall(data)
    return response(t)


if __name__ == "__main__":
    sys.exit(main())
