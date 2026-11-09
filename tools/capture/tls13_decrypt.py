"""Decrypt TLS 1.3 sessions from an Ethernet/IPv6 pcap using an NSS keylog.
Usage: tlsdec.py capture.pcap keylog outdir [port]
Writes outdir/flowN-c2s.bin and flowN-s2c.bin (plaintext application data). PRIVATE output."""
import struct, sys, os, hmac, hashlib
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
pcap, keylog, outdir = sys.argv[1:4]; PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 8771
keys = {}
for line in open(keylog):
    p = line.split()
    if len(p) == 3 and not line.startswith('#'): keys.setdefault(bytes.fromhex(p[1]), {})[p[0]] = bytes.fromhex(p[2])
def expand_label(secret, label, length, h):
    info = struct.pack('>H', length) + bytes([len(label) + 6]) + b'tls13 ' + label + b'\0'
    out = b''; t = b''; i = 1
    while len(out) < length:
        t = hmac.new(secret, t + info + bytes([i]), h).digest(); out += t; i += 1
    return out[:length]
SUITES = {0x1301: (hashlib.sha256, 16, AESGCM), 0x1302: (hashlib.sha384, 32, AESGCM), 0x1303: (hashlib.sha256, 32, ChaCha20Poly1305)}
# --- TCP reassembly
f = open(pcap, 'rb'); f.read(24); flows = {}; order = []
while True:
    h = f.read(16)
    if len(h) < 16: break
    ts, tf, cl, ol = struct.unpack('<IIII', h); d = f.read(cl)
    if struct.unpack_from('>H', d, 12)[0] != 0x86dd or d[20] != 6: continue
    plen = struct.unpack_from('>H', d, 18)[0]; p = d[54:54 + plen]
    sp, dp, seq = struct.unpack_from('>HHI', p, 0); hl = (p[12] >> 4) * 4; fl = p[13]; pay = p[hl:]
    if PORT not in (sp, dp): continue
    c2s = dp == PORT; key = (d[22:38], sp) if c2s else (d[38:54], dp)
    if key not in flows: flows[key] = {True: {'isn': None, 'seg': {}}, False: {'isn': None, 'seg': {}}, 't': ts + tf / 1e6}; order.append(key)
    s = flows[key][c2s]
    if fl & 2: s['isn'] = (seq + 1) & 0xffffffff
    if pay and s['isn'] is not None: s['seg'].setdefault((seq - s['isn']) & 0xffffffff, pay)
def stream(s):
    out = bytearray(); holes = 0
    for off in sorted(s['seg']):
        seg = s['seg'][off]
        if off > len(out): holes += off - len(out); break   # stop at first hole
        if off + len(seg) > len(out): out += seg[len(out) - off:]
    return bytes(out), holes
def records(b):
    i = 0
    while i + 5 <= len(b):
        t, v, l = struct.unpack_from('>BHH', b, i)
        if i + 5 + l > len(b): break
        yield t, b[i:i + 5], b[i + 5:i + 5 + l]; i += 5 + l
os.makedirs(outdir, exist_ok=True)
for n, key in enumerate(order):
    fl = flows[key]; c, ch = stream(fl[True]); s, sh = stream(fl[False])
    cr = None; suite = None; alpn = None; sni = None
    for t, hdr, body in records(c):
        if t == 22 and body[0] == 1: cr = body[6:38]; break
    for t, hdr, body in records(s):
        if t == 22 and body[0] == 2:
            sid = body[38]; suite = struct.unpack_from('>H', body, 39 + sid)[0]; break
    info = f"flow{n} t={fl['t']:.0f} c2s={len(c)}B(hole {ch}) s2c={len(s)}B(hole {sh}) suite={hex(suite) if suite else None}"
    if cr not in keys or suite not in SUITES: print(info, 'NO KEYS'); continue
    hf, klen, aead = SUITES[suite]; k = keys[cr]
    for side, data, hs, ap in (('c2s', c, 'CLIENT_HANDSHAKE_TRAFFIC_SECRET', 'CLIENT_TRAFFIC_SECRET_0'), ('s2c', s, 'SERVER_HANDSHAKE_TRAFFIC_SECRET', 'SERVER_TRAFFIC_SECRET_0')):
        out = bytearray(); phase = hs; seqn = 0; err = 0; hsmsgs = []; alerts = []
        def mk(sec): return aead(expand_label(sec, b'key', klen, hf)), expand_label(sec, b'iv', 12, hf)
        if phase not in k: print(info, side, 'missing', phase); continue
        cipher, iv = mk(k[phase])
        for t, hdr, body in records(data):
            if t != 23: continue
            nonce = bytes(a ^ b for a, b in zip(iv, b'\0' * 4 + struct.pack('>Q', seqn)))
            try: pt = cipher.decrypt(nonce, body, hdr)
            except Exception: err += 1; break
            seqn += 1; pt = pt.rstrip(b'\0'); ct = pt[-1]; pt = pt[:-1]
            if ct == 23: out += pt
            elif ct == 21: alerts.append(tuple(pt))
            elif ct == 22:
                hsmsgs.append(pt[0])
                if pt[0] == 20 and phase == hs and ap in k:   # Finished -> switch to application secret
                    phase = ap; cipher, iv = mk(k[ap]); seqn = 0
        open(os.path.join(outdir, f'flow{n}-{side}.bin'), 'wb').write(out)
        print(info, side, f'plaintext={len(out)}B hs_msgs={hsmsgs} alerts={alerts} decrypt_err={err}')
