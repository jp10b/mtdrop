import struct, sys, collections
def rt_parse(b):
    ver, pad, ln, pres = struct.unpack_from('<BBHI', b, 0)
    off = 8; p = pres; words=[pres]
    while p & 0x80000000:
        p, = struct.unpack_from('<I', b, off); off += 4; words.append(p)
    pres = words[0]; r = {}
    def al(o, a): return (o + a - 1) & ~(a - 1)
    if pres & 1: off = al(off, 8); r['tsft'], = struct.unpack_from('<Q', b, off); off += 8
    if pres & 2: r['flags'] = b[off]; off += 1
    if pres & 4: r['rate'] = b[off] * 0.5; off += 1
    if pres & 8: off = al(off, 2); r['freq'], r['chflags'] = struct.unpack_from('<HH', b, off); off += 4
    if pres & 16: off += 2
    if pres & 32: r['sig'] = struct.unpack_from('<b', b, off)[0]; off += 1
    if pres & 64: off += 1
    if pres & (1<<7): off = al(off,2)+2
    if pres & (1<<8): off = al(off,2)+2
    if pres & (1<<9): off = al(off,2)+2
    if pres & (1<<10): off += 1
    if pres & (1<<11): off += 1
    if pres & (1<<12): off += 1
    if pres & (1<<13): off += 1
    if pres & (1<<14): off = al(off,2)+2
    if pres & (1<<15): off = al(off,2); r['txflags'], = struct.unpack_from('<H', b, off); off += 2
    if pres & (1<<16): off += 1
    if pres & (1<<17): off += 1
    if pres & (1<<19): r['mcs'] = b[off+2]; off += 3
    r['pres'] = pres; r['len'] = ln
    return r
f = open(sys.argv[1], 'rb'); gh = f.read(24)
magic, = struct.unpack('<I', gh[:4]); ns = magic == 0xa1b23c4d
link, = struct.unpack('<I', gh[20:24]); print('linktype', link, 'ns' if ns else 'us')
AW = bytes.fromhex('002500ff9473')
n=0; freq=collections.Counter(); kinds=collections.Counter(); pres=collections.Counter(); rates=collections.Counter()
retry=collections.Counter(); dirn=collections.Counter(); t0=None; tl=collections.Counter(); sig=collections.Counter(); fcsflag=collections.Counter()
txnoack=collections.Counter(); bad=0; acks=0; ctrl=collections.Counter(); a3=collections.Counter(); persec_ch=collections.defaultdict(collections.Counter)
while True:
    h = f.read(16)
    if len(h) < 16: break
    ts, tf, cl, ol = struct.unpack('<IIII', h); d = f.read(cl); n += 1
    t = ts + tf/(1e9 if ns else 1e6)
    if t0 is None: t0 = t
    try: r = rt_parse(d)
    except Exception: bad += 1; continue
    m = d[r['len']:]
    if len(m) < 10: bad += 1; continue
    fc = m[0]; typ = (fc >> 2) & 3; sub = fc >> 4; fl = m[1]
    pres[hex(r['pres'])] += 1
    injected = 'sig' not in r   # frames we TX have no signal field typically
    side = 'tx(own)' if injected else 'rx'
    if 'freq' in r: freq[(side, r['freq'])] += 1; persec_ch[int(t-t0)//10][r['freq']] += 1
    if 'flags' in r: fcsflag[(side, bool(r['flags'] & 0x10), bool(r['flags'] & 0x40))] += 1
    if 'txflags' in r: txnoack[(side, hex(r['txflags']))] += 1
    if typ == 1:
        ctrl[(side, sub)] += 1; continue
    if len(m) < 24: bad += 1; continue
    is_awdl = m[16:22] == AW
    kinds[(side, ['mgmt','ctrl','data','ext'][typ], sub, 'awdl' if is_awdl else 'other')] += 1
    if is_awdl and typ == 2:
        uni = not (m[4] & 1)
        retry[(side, 'uni' if uni else 'multi', bool(fl & 8))] += 1
        if 'rate' in r: rates[(side, 'rate', r['rate'])] += 1
        elif 'mcs' in r: rates[(side, 'mcs', r['mcs'])] += 1
        else: rates[(side, 'none', 0)] += 1
        tl[(int(t - t0)//10, side)] += len(m)
    if is_awdl and not injected: sig[r['sig']//5*5] += 1
print('frames', n, 'bad', bad, 'duration', round(t - t0, 1))
for name, c in [('present', pres), ('freq', freq), ('fcs/badfcs flag', fcsflag), ('txflags', txnoack), ('ctrl', ctrl), ('kinds', kinds), ('retry awdl data', retry), ('rates awdl data', rates), ('signal', sig)]:
    print('---', name)
    for k, v in sorted(c.items(), key=lambda x: -x[1])[:25]: print('  ', k, v)
print('--- awdl data bytes per 10 s (rx / own tx)')
for s in sorted({k[0] for k in tl}): print('  ', s*10, tl[(s,'rx')], tl[(s,'tx(own)')], dict(persec_ch[s]))
