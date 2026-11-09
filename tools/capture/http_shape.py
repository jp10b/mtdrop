"""Print the SHAPE of decrypted AirDrop HTTP exchanges (no identifying values).
Usage: http_shape.py <dir with flowN-c2s.bin / flowN-s2c.bin from tls13_decrypt.py>"""
import sys, os, re, plistlib, glob, zlib, struct
SAFE_HDR = {'user-agent','connection','transfer-encoding','content-type','accept','accept-language','accept-encoding','content-length','expect','server','date'}
def red(v):
    if isinstance(v, dict): return {k: red(x) for k, x in v.items()}
    if isinstance(v, list): return [red(x) for x in v[:3]] + (['…%d more' % (len(v) - 3)] if len(v) > 3 else [])
    if isinstance(v, bytes): return '<data %d B magic %s>' % (len(v), v[:8].hex())
    if isinstance(v, bool) or isinstance(v, (int, float)): return v
    if isinstance(v, str): return '<str %d>' % len(v) if not re.fullmatch(r'(public\.[\w.-]+|com\.apple\.[\w.-]+)', v) else v
    return '<%s>' % type(v).__name__
def dechunk(b):
    out = bytearray(); i = 0; sizes = []; term = False
    while True:
        j = b.find(b'\r\n', i)
        if j < 0: break
        try: n = int(b[i:j].split(b';')[0], 16)
        except ValueError: break
        sizes.append(n)
        if n == 0: term = True; i = j + 2; break
        if j + 2 + n > len(b): out += b[j + 2:]; i = len(b); sizes[-1] = -n; break
        out += b[j + 2:j + 2 + n]; i = j + 2 + n + 2
    return bytes(out), sizes, term, i
def show(side, b):
    i = 0
    while i < len(b):
        k = b.find(b'\r\n\r\n', i)
        if k < 0: print('   [trailing %d B]' % (len(b) - i)); break
        head = b[i:k].decode('latin1').split('\r\n'); print('  ', side, head[0])
        hd = {}
        for h in head[1:]:
            n, _, v = h.partition(': '); hd[n.lower()] = v
            print('      %s: %s' % (n, v if n.lower() in SAFE_HDR or n.lower() == 'totalbytes' else '<redacted %d chars>' % len(v)))
        body = b[k + 4:]; used = 0
        if hd.get('transfer-encoding') == 'chunked':
            data, sizes, term, used = dechunk(body)
            print('      chunks: n=%d first=%s max=%d last=%s terminated_by_0_chunk=%s body=%d B' % (len(sizes), sizes[:4], max(map(abs, sizes)) if sizes else 0, sizes[-3:], term, len(data)))
        elif 'content-length' in hd:
            used = int(hd['content-length']); data = body[:used]
        else: data = b''
        if data[:6] == b'bplist':
            try: print('      plist:', red(plistlib.loads(data)))
            except Exception: print('      plist: <incomplete, %d B>' % len(data))
        elif hd.get('content-type') == 'application/x-dvzip':
            j = 0; nb = 0; raw = bytearray(); kinds = {'stored': 0, 'zlib': 0}; lens = []
            while j + 4 <= len(data):
                w, = struct.unpack_from('>I', data, j); ln = w & 0x7fffffff; st = w >> 31
                if j + 4 + ln > len(data): print('      dvzip: truncated block wanted %d have %d' % (ln, len(data) - j - 4)); break
                blk = data[j + 4:j + 4 + ln]; j += 4 + ln; nb += 1; lens.append(ln)
                if st: kinds['stored'] += 1; raw += blk
                else: kinds['zlib'] += 1; raw += zlib.decompress(blk)
            print('      dvzip: blocks=%d %s compressed_lens(first/last)=%s/%s -> %d B; leftover %d B' % (nb, kinds, lens[:3], lens[-2:], len(raw), len(data) - j))
            # cpio odc walk
            p = 0; ents = []
            while raw[p:p + 6] == b'070707':
                f = raw[p:p + 76].decode(); mode = int(f[18:24], 8); nlen = int(f[59:65], 8); fsz = int(f[65:76], 8)
                name = raw[p + 76:p + 76 + nlen - 1].decode('utf8', 'replace'); p += 76 + nlen + fsz
                ents.append((re.sub(r'[^./]', 'x', name) if name not in ('.', 'TRAILER!!!') else name, oct(mode), fsz, len(name)))
            print('      cpio odc entries (name masked, mode, size, namelen):', ents, 'tail', len(raw) - p)
        elif data: print('      body %d B magic %s' % (len(data), data[:8].hex()))
        i = k + 4 + used
d = sys.argv[1]
for path in sorted(glob.glob(os.path.join(d, 'flow*-c2s.bin'))):
    print('===', os.path.basename(path)[:-8])
    for side in ('c2s', 's2c'): show(side, open(path[:-7] + side + '.bin', 'rb').read())
