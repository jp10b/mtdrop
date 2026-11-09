import struct, sys, collections
f = open(sys.argv[1], 'rb'); gh = f.read(24)
link, = struct.unpack('<I', gh[20:24]); print('linktype', link)
et = collections.Counter(); nh = collections.Counter(); ports = collections.Counter(); udp = collections.Counter()
flows = {}; t0 = None; n = 0; sizes = collections.Counter()
while True:
    h = f.read(16)
    if len(h) < 16: break
    ts, tf, cl, ol = struct.unpack('<IIII', h); d = f.read(cl); n += 1; t = ts + tf/1e6
    if t0 is None: t0 = t
    e, = struct.unpack_from('>H', d, 12); et[hex(e)] += 1
    if e != 0x86dd: continue
    plen, nxt = struct.unpack_from('>HB', d, 18); src = d[22:38]; dst = d[38:54]; p = d[54:]
    nh[nxt] += 1
    if nxt == 17:
        sp, dp = struct.unpack_from('>HH', p, 0); udp[(sp, dp)] += 1
    if nxt != 6: continue
    sp, dp, seq, ack, offf, win = struct.unpack_from('>HHIIHH', p, 0)
    hl = (offf >> 12) * 4; fl = offf & 0x3f; pay = plen - hl
    tous = dp == 8771
    key = (src, sp) if tous else (dst, dp)
    fw = flows.setdefault(key, dict(t0=t, t1=t, rx=0, tx=0, rxp=0, retr=0, maxseq=None, mss=None, ws=None, sack=False, maxpay=0, gaps=[], lastrx=None, syn=0, fin=0, rst=0, tl=collections.Counter(), dupack=0, lastack=None))
    fw['t1'] = t
    if fl & 2:
        fw['syn'] += 1; o = p[20:hl]; i = 0
        while i < len(o):
            k = o[i]
            if k == 0: break
            if k == 1: i += 1; continue
            l = o[i+1]
            if tous and k == 2: fw['mss'], = struct.unpack_from('>H', o, i+2)
            if tous and k == 3: fw['ws'] = o[i+2]
            if k == 4: fw['sack'] = True
            i += l
    if fl & 1: fw['fin'] += 1
    if fl & 4: fw['rst'] += 1
    if tous and pay > 0:
        fw['rx'] += pay; fw['rxp'] += 1; fw['maxpay'] = max(fw['maxpay'], pay); sizes[pay] += 1
        end = (seq + pay) & 0xffffffff
        if fw['maxseq'] is not None and ((end - fw['maxseq']) & 0xffffffff) > 0x7fffffff or end == fw['maxseq']: fw['retr'] += 1
        else: fw['maxseq'] = end
        if fw['lastrx'] is not None: fw['gaps'].append(t - fw['lastrx'])
        fw['lastrx'] = t; fw['tl'][int(t - fw['t0']) // 5] += pay
    elif not tous:
        if pay > 0: fw['tx'] += pay
        elif fl == 0x10:
            if ack == fw['lastack']: fw['dupack'] += 1
            fw['lastack'] = ack
print('frames', n, 'ethertypes', dict(et), 'ipv6 next-headers', dict(nh)); print('udp ports', dict(udp))
print('top payload sizes', sizes.most_common(6))
for i, (k, w) in enumerate(sorted(flows.items(), key=lambda x: x[1]['t0'])):
    g = sorted(w['gaps']) or [0]
    print(f"flow{i}: start +{w['t0']-t0:.1f}s dur {w['t1']-w['t0']:.1f}s rx {w['rx']} B in {w['rxp']} segs, tx {w['tx']} B, retrans/ooo {w['retr']}, dupacks {w['dupack']}, mss {w['mss']} ws {w['ws']} sack {w['sack']} maxseg {w['maxpay']}, gap med {g[len(g)//2]*1000:.1f} ms p99 {g[int(len(g)*.99)]*1000:.0f} ms max {g[-1]:.2f} s, syn {w['syn']} fin {w['fin']} rst {w['rst']}")
    if w['rx'] > 100000: print('   kB per 5 s:', [round(w['tl'][s]/1000) for s in range(max(w['tl'])+1)])
