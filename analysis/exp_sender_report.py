# exp_sender_report.py <results/run> [bin_s=10] — sender + receiver + gNB, per camera, on one time axis.
# Needs the sender traces collected by run_experiment.sh (<run>/senders/camK/app). Per bin: GoogCC target
# (median), RTT, delay-based overuse/underuse events, encoded frames, sent/received kbps, relative one-way delay
# (tx-rtp -> rx-rtp joined by (ssrc, seq) with a wrap-aware nearest-time match; min-normalised, no clock sync
# needed), and from the gNB: UL CRC failure %, median MCS, grants, BSR max, RLC t-Reassembly expiries per UE.
# Then a 1 s zoom of the +-15 s around the largest relative-delay spike of the run.
# Offline only; reads a run directory, writes nothing.
import csv, sys, os, glob, re, json, bisect, collections, statistics as st
RD = sys.argv[1]; BIN = int(sys.argv[2]) if len(sys.argv) > 2 else 10
def rows(p):
    with open(p) as f: return list(csv.DictReader(l for l in f if not l.startswith('#')))
def q(v, p): v = sorted(v); return v[min(len(v)-1, int(p*len(v)))] if v else float('nan')
def f0(x): return '-' if x != x else f"{x:.0f}"
cams = sorted(os.path.basename(d) for d in glob.glob(f'{RD}/senders/cam*') if os.path.isdir(d))
if not cams: sys.exit(f"no sender traces under {RD}/senders/")
fr0 = rows(f'{RD}/app/{cams[0]}-rx-frames.csv'); T0 = int(fr0[0]['recv_mono_ns'])/1e9
span = max(int(rows(f'{RD}/app/{c}-rx-frames.csv')[-1]['recv_mono_ns']) for c in cams)/1e9 - T0; NB = int(span//BIN) + 1
# stream -> UE (ip -> rnti) via signaling + pdcp
ip2s = {}; last = None
for line in open(f'{RD}/app/signaling.log'):
    m = re.search(r"connection from \('([\d.]+)'", line); last = m.group(1) if m else last
    m = re.search(r"sender (\w+) registered", line)
    if m and last and last.startswith('10.45'): ip2s[last] = m.group(1)
has_gnb = os.path.exists(f'{RD}/gnb/gnb_sched_ul.csv')
s2rnti = {}
if has_gnb:
    ue2ip = collections.defaultdict(collections.Counter)
    for r in rows(f'{RD}/gnb/gnb_pdcp_ul.csv'):
        if r['src_ip'].startswith('10.45'): ue2ip[r['ue_index']][r['src_ip']] += 1
    ue2ip = {u: c.most_common(1)[0][0] for u, c in ue2ip.items()}
    crc = rows(f'{RD}/gnb/gnb_ul_crc.csv'); sched = rows(f'{RD}/gnb/gnb_sched_ul.csv'); bsr = rows(f'{RD}/gnb/gnb_bsr.csv'); rlc = rows(f'{RD}/gnb/gnb_rlc_ul.csv')
    rnti2ue = {r['rnti']: r['ue_index'] for r in crc if r['ue_index'] != '1024'}
    for rnti, ue in rnti2ue.items():
        s = ip2s.get(ue2ip.get(ue, ''), None)
        if s: s2rnti[s] = (rnti, ue)
    cidx = collections.defaultdict(list)
    for r in crc: cidx[(r['rnti'], r['harq_id'])].append((int(r['mono_ns']), r['crc_ok'] in ('1', 'true', 'True')))
    for k in cidx: cidx[k].sort()
    ckeys = {k: [x[0] for x in v] for k, v in cidx.items()}
    G = collections.defaultdict(list)   # rnti -> [(t, ok, mcs, rb)]
    for g in sched:
        k = (g['rnti'], g['harq_id']); l = cidx.get(k)
        if not l: continue
        gm = int(g['mono_ns']); i = bisect.bisect_left(ckeys[k], gm)
        if i >= len(l) or ckeys[k][i] - gm > 20e6: continue
        G[g['rnti']].append((gm/1e9 - T0, l[i][1], int(g['mcs']), int(g['rb_count'])))
    BS = collections.defaultdict(list)
    for r in bsr: BS[r['rnti']].append((int(r['mono_ns'])/1e9 - T0, int(r['buffer_bytes'])))
    RE = collections.defaultdict(list)
    for r in rlc:
        if r['event'] == 'reassembly_expired': RE[r['ue_index']].append(int(r['mono_ns'])/1e9 - T0)
    prb = collections.defaultdict(int)
    for rnti, v in G.items():
        for t, ok, mcs, rb in v: prb[int(t//BIN)] += rb
print(f"# {RD}  cams={cams}  span={span:.0f}s  bin={BIN}s  (t=0 at the first received frame, gNB PC clock)")
if has_gnb: print("UL PRB utilisation %:", [round(100*prb[b]/(BIN*400*51)) for b in range(NB)])
spikes = []
per = {}
for cam in cams:
    A = f'{RD}/senders/{cam}/app'
    fr = rows(f'{A}/{cam}-tx-frames.csv'); t0 = int(fr[0]['capture_mono_ns'])/1e9
    cc = rows(f'{A}/{cam}-tx-cc.csv'); ev = rows(f'{A}/{cam}-tx-events.csv'); enc = rows(f'{A}/{cam}-tx-encoded.csv'); tx = rows(f'{A}/{cam}-tx-rtp.csv'); rx = rows(f'{RD}/app/{cam}-rx-rtp.csv')
    # laptop clock -> gNB clock offset estimate from the first matched packet (relative delays only)
    txi = collections.defaultdict(list)
    for r in tx:
        if r['dir'] == 'out': txi[(r['ssrc'], r['seq'])].append(int(r['log_mono_ns']))
    for k in txi: txi[k].sort()
    rxin = [r for r in rx if r['dir'] == 'in']
    first = next((r for r in rxin if (r['ssrc'], r['seq']) in txi), None)
    if first is None: print(f"## {cam}: no tx/rx packet match"); continue
    off = int(first['log_mono_ns']) - txi[(first['ssrc'], first['seq'])][0]
    pairs = []
    for r in rxin:
        l = txi.get((r['ssrc'], r['seq']))
        if not l: continue
        target = int(r['log_mono_ns']) - off; i = bisect.bisect_right(l, target + 2_000_000_000) - 1
        if i < 0 or abs(l[i] - target) > 5e9: continue
        pairs.append((int(r['log_mono_ns'])/1e9 - T0, (int(r['log_mono_ns']) - l[i] - off)/1e6))
    base = min(x[1] for x in pairs)
    owd = collections.defaultdict(list)
    for t, v in pairs: owd[int(t//BIN)].append(v - base)
    def tg_(ns): return (ns + off)/1e9 - T0   # laptop mono ns -> gNB-relative seconds
    tgt = collections.defaultdict(list); rtt = collections.defaultdict(list); ov = collections.Counter(); un = collections.Counter(); en = collections.Counter(); sent = collections.defaultdict(int)
    for r in cc:
        b = int(tg_(int(r['log_mono_ns']))//BIN)
        if r['target_bps'] not in ('-1', ''): tgt[b].append(int(r['target_bps'])/1000)
        if r['rtt_us'] not in ('-1', ''): rtt[b].append(int(r['rtt_us'])/1000)
    for r in ev:
        if r['event'] == 'bwe_delay':
            b = int(tg_(int(r['log_mono_ns']))//BIN); ov[b] += r['b'] == '2'; un[b] += r['b'] == '1'
    for r in enc: en[int(tg_(int(r['encode_done_mono_ns']))//BIN)] += 1
    for r in tx:
        if r['dir'] == 'out': sent[int(tg_(int(r['log_mono_ns']))//BIN)] += int(r['pkt_bytes'])*8
    rcv = collections.defaultdict(int)
    for r in rxin: rcv[int((int(r['log_mono_ns'])/1e9 - T0)//BIN)] += int(r['pkt_bytes'])*8
    per[cam] = dict(tgt=tgt, ov=ov, owd=owd, off=off)
    print(f"\n## {cam}  (captured {len(fr)}, encoded {len(enc)}, rtp out {sum(1 for r in tx if r['dir']=='out')}, rtp in {len(rxin)}, joined {len(pairs)})")
    print("  GCC target kbps med :", [f0(st.median(tgt[b])) if tgt.get(b) else '-' for b in range(NB)])
    print("  sent / recv kbps    :", [f"{sent[b]/(BIN*1000):.0f}/{rcv[b]/(BIN*1000):.0f}" for b in range(NB)])
    print("  RTT ms med          :", [f0(st.median(rtt[b])) if rtt.get(b) else '-' for b in range(NB)])
    print("  overuse/underuse    :", [f"{ov.get(b,0)}/{un.get(b,0)}" for b in range(NB)])
    print("  encoded frames      :", [en.get(b, 0) for b in range(NB)])
    print("  rel-OWD p50/p90/max :", [f"{q(owd[b],.5):.0f}/{q(owd[b],.9):.0f}/{max(owd[b]):.0f}" if owd.get(b) else '-' for b in range(NB)])
    if has_gnb and cam in s2rnti:
        rnti, ue = s2rnti[cam]; v = G.get(rnti, [])
        fb = collections.defaultdict(list); mb = collections.defaultdict(list)
        for t, ok, mcs, rb in v: fb[int(t//BIN)].append(not ok); mb[int(t//BIN)].append(mcs)
        bb = collections.defaultdict(list)
        for t, bytes_ in BS.get(rnti, []): bb[int(t//BIN)].append(bytes_)
        reb = collections.Counter(int(t//BIN) for t in RE.get(ue, []))
        print(f"  gNB UE {hex(int(rnti))}: fail% :", [f0(100*sum(fb[b])/len(fb[b])) if fb.get(b) else '-' for b in range(NB)])
        print("           MCS med     :", [f0(st.median(mb[b])) if mb.get(b) else '-' for b in range(NB)])
        print("           grants      :", [len(fb.get(b, [])) for b in range(NB)])
        print("           BSR max KB  :", [f0(max(bb[b])/1e3) if bb.get(b) else '0' for b in range(NB)])
        print("           RLC reasm   :", [reb.get(b, 0) for b in range(NB)])
    for t, v in pairs: spikes.append((v - base, t, cam))
# zoom around the largest delay spike
if spikes and has_gnb:
    peak = max(spikes); tc = int(peak[1]); lo, hi = max(0, tc-15), min(int(span), tc+15)
    print(f"\n## 1 s zoom around the largest rel-OWD spike ({peak[0]:.0f} ms on {peak[2]} at t={peak[1]:.1f}s): per camera: UL fail% MCS grants BSRmaxKB | GCC target Mbps overuse relOWDmax")
    hdr = " t(s) |" + "|".join(f" {c:^44}" for c in cams if c in s2rnti); print(hdr)
    for t in range(lo, hi+1):
        line = f"{t:>5} |"
        for cam in cams:
            if cam not in s2rnti: continue
            rnti, ue = s2rnti[cam]; v = [x for x in G.get(rnti, []) if int(x[0]) == t]
            fl = 100*sum(1 for x in v if not x[1])/len(v) if v else float('nan'); m = st.median([x[2] for x in v]) if v else float('nan')
            b = max([bytes_ for tt, bytes_ in BS.get(rnti, []) if int(tt) == t], default=0)/1e3
            p = per[cam]; off = p['off']
            A = f'{RD}/senders/{cam}/app'
            # per-second GCC target and overuse recomputed cheaply from cached bins is not exact; read once per cam
            if 'cc1' not in p:
                p['cc1'] = collections.defaultdict(list); p['ov1'] = collections.Counter(); p['owd1'] = collections.defaultdict(list)
                for r in rows(f'{A}/{cam}-tx-cc.csv'):
                    if r['target_bps'] not in ('-1', ''): p['cc1'][int((int(r['log_mono_ns'])+off)/1e9 - T0)].append(int(r['target_bps'])/1e6)
                for r in rows(f'{A}/{cam}-tx-events.csv'):
                    if r['event'] == 'bwe_delay' and r['b'] == '2': p['ov1'][int((int(r['log_mono_ns'])+off)/1e9 - T0)] += 1
                for d_, tt, c_ in spikes:
                    if c_ == cam: p['owd1'][int(tt)].append(d_)
            g1 = st.median(p['cc1'][t]) if p['cc1'].get(t) else float('nan'); o1 = p['ov1'].get(t, 0); w1 = max(p['owd1'][t]) if p['owd1'].get(t) else float('nan')
            line += f" {fl:4.0f}% {m:3.0f} {len(v):4d} {b:5.0f} | {g1:5.2f}M {o1:2d} {w1:5.0f}ms |"
        print(line)
