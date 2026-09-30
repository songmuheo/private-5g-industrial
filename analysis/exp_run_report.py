# exp_run_report.py <results/run> <tdd_period_slots>  — per-stream app summary + gNB UL link report for one run
# (CRC fail structure, HARQ completion, RLC/BSR, PRB use). Joins sched_ul<->ul_crc by time proximity
# because (rnti,sfn,slot,harq) keys collide across SFN wraps (10.24 s). Offline analysis only; writes nothing.
import csv, collections, statistics as st, json, bisect, glob, os, re, sys
RD=sys.argv[1]; NTP_UNIX_MS=2208988800000; SPF=20  # slots per frame @30 kHz
TDD_PERIOD=int(sys.argv[2]) if len(sys.argv)>2 else 10
def rows(p):
    with open(p) as f: return list(csv.DictReader(l for l in f if not l.startswith('#')))
def q(v,p): v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else None
def f0(v): return "-" if v is None else f"{v:.0f}"
def f1(v): return "-" if v is None else f"{v:.1f}"
streams=sorted(os.path.basename(p).replace('-rx-frames.csv','') for p in glob.glob(f'{RD}/app/*-rx-frames.csv'))
# ip -> stream from the control channel log (webrtc: signaling.log, gstreamer: control.log; same line format)
ip2s={}; last_ip=None
CTL_LOG=next((p for p in (f'{RD}/app/signaling.log', f'{RD}/app/control.log') if os.path.exists(p)), None)
for line in (open(CTL_LOG) if CTL_LOG else []):
    m=re.search(r"connection from \('([\d.]+)'",line)
    if m: last_ip=m.group(1)
    m=re.search(r"sender (\w+) registered",line)
    if m and last_ip and last_ip.startswith('10.45'): ip2s[last_ip]=m.group(1)
frames={s:rows(f'{RD}/app/{s}-rx-frames.csv') for s in streams}
T0=min(int(fr[0]['recv_mono_ns']) for fr in frames.values())/1e9
SPAN=max(int(fr[-1]['recv_mono_ns']) for fr in frames.values())/1e9-T0
BIN=5 if SPAN<=90 else 10; NB=int(SPAN//BIN)+1; WEND=SPAN+2
print(f"# {RD}  streams={streams}  T0(mono s)={T0:.1f}  span={SPAN:.0f}s  bins={BIN}s  ip->stream={ip2s}")
print("## A. app per stream")
for s in streams:
    fr=frames[s]; rtp=rows(f'{RD}/app/{s}-rx-rtp.csv')
    t0=int(fr[0]['recv_mono_ns'])/1e9; t1=int(fr[-1]['recv_mono_ns'])/1e9
    byssrc=collections.defaultdict(set)
    for r in rtp:
        if r['dir']=='in': byssrc[r['ssrc']].add(int(r['seq']))
    loss=sum((max(v)-min(v)+1-len(v)) for v in byssrc.values() if len(v)>10)
    # W3C getStats lines exist only in webrtc runs; gstreamer runs carry rtpsession stats instead -> last=None
    last=None; STATS=f'{RD}/app/{s}-rx-stats.jsonl'
    for line in (open(STATS) if os.path.exists(STATS) else []):
        j=json.loads(line)
        for x in j.get('stats', []):
            if x['type']=='inbound-rtp' and x.get('kind')=='video': last=x
    # received kbps per bin from the RTP ledger itself (transport-neutral)
    kb=collections.defaultdict(float)
    for r in rtp:
        if r['dir']=='in': kb[int((int(r['log_mono_ns'])/1e9-T0)//BIN)]+=int(r['pkt_bytes'])*8/(BIN*1000)
    last=last or {}
    net=collections.defaultdict(list); span=collections.defaultdict(list)
    for r in fr:
        cap=int(r['abs_capture_ntp_ms']); lp=int(r['last_pkt_mono_ns']); fp=int(r['first_pkt_mono_ns']); rm=int(r['recv_mono_ns']); rw=int(r['recv_wall_ns'])
        if cap<=0 or lp<=0: continue
        b=int((rm/1e9-T0)//BIN); net[b].append((lp+rw-rm)/1e6-(cap-NTP_UNIX_MS)); span[b].append((lp-fp)/1e6)
    print(f"{s}: frames={len(fr)} span={t1-t0:.1f}s fps={len(fr)/(t1-t0):.1f} rtp_in={sum(len(v) for v in byssrc.values())} seq_loss={loss} | stats: lost={last.get('packetsLost')} nack={last.get('nackCount')} pli={last.get('pliCount')} freeze={last.get('freezeCount')}/{last.get('totalFreezesDuration')}s jbufDelay_avg={1000*last.get('jitterBufferDelay',0)/max(1,last.get('jitterBufferEmittedCount',1)):.0f}ms keyframes={last.get('keyFramesDecoded')} bytes={(last.get('bytesReceived') or sum(int(r['pkt_bytes']) for r in rtp if r['dir']=='in'))/1e6:.2f}MB")
    print(f"   kbps/{BIN}s: {[round(kb[i]) for i in range(0,NB)]}")
    print(f"   net OWD med/p90 (ms, offset unknown): {[f'{f0(st.median(net[i]))}/{f0(q(net[i],.9))}' for i in range(0,NB) if net.get(i)]}")
    print(f"   frame span med/p90/max (ms): {[f'{f0(st.median(span[i]))}/{f0(q(span[i],.9))}/{f0(max(span[i]))}' for i in range(0,NB) if span.get(i)]}")
print("## A2. ICE path per stream: transport.selectedCandidatePairId (rx-stats) + gNB PDCP UL destination (RTP-like rows)")
pdcp_dst=collections.defaultdict(collections.Counter)
for r in (rows(f'{RD}/gnb/gnb_pdcp_ul.csv') if os.path.exists(f'{RD}/gnb/gnb_pdcp_ul.csv') else []):
    if r['rtp_like'] in ('1','true','True') and r['src_ip'].startswith('10.45'): pdcp_dst[r['src_ip']][r['dst_ip']]+=1
s2ip={v:k for k,v in ip2s.items()}
for s in streams:
    cands={}; pairs={}; tr=None; STATS=f'{RD}/app/{s}-rx-stats.jsonl'
    for line in (open(STATS) if os.path.exists(STATS) else []):
        j=json.loads(line)
        for x in j.get('stats', []):
            if x['type'] in ('local-candidate','remote-candidate'): cands[x['id']]=x
            elif x['type']=='candidate-pair': pairs[x['id']]=x
            elif x['type']=='transport': tr=x
    sel=(tr or {}).get('selectedCandidatePairId'); p=pairs.get(sel)
    if p:
        l=cands.get(p['localCandidateId'],{}); r=cands.get(p['remoteCandidateId'],{})
        ip=s2ip.get(s,'?'); dst=pdcp_dst.get(ip,{}); tot=sum(dst.values()) or 1; via5g=100*dst.get(l.get('address',''),0)/tot
        ok=(l.get('address')=='10.53.1.1' and via5g>90)
        print(f"  {s}: selected local {l.get('address')}:{l.get('port')}/{l.get('protocol')} <- remote {r.get('candidateType')} :{r.get('port')} (address redacted for prflx), {p.get('bytesReceived',0)/1e6:.2f} MB, RTT {1000*p.get('currentRoundTripTime',0):.0f} ms | gNB PDCP UL RTP from {ip}: {via5g:.0f}% to {l.get('address')} (n={tot}) -> {'OK: media went over the 5G link' if ok else 'WARNING: check path'}")
    else:
        # no ICE (gstreamer tree): the RTP destination is fixed by the control channel; judge by the gNB PDCP UL rows alone
        ip=s2ip.get(s,'?'); dst=pdcp_dst.get(ip,{}); tot=sum(dst.values())
        if tot: via5g=100*dst.get('10.53.1.1',0)/tot; print(f"  {s}: plain RTP | gNB PDCP UL RTP from {ip}: {via5g:.0f}% to 10.53.1.1 (n={tot}) -> {'OK: media went over the 5G link' if via5g>90 else 'WARNING: check path'}")
        else: print(f"  {s}: plain RTP, no gNB PDCP UL RTP rows for {ip} (local run or path unknown)")
if not os.path.exists(f'{RD}/gnb/gnb_sched_ul.csv'):
    print("## B. gNB: no gnb/ traces in this run (local smoke test) -> skipped"); sys.exit(0)
print("## B. gNB")
sched=rows(f'{RD}/gnb/gnb_sched_ul.csv'); crc=rows(f'{RD}/gnb/gnb_ul_crc.csv'); pd=rows(f'{RD}/gnb/gnb_pdcp_ul.csv')
ue2ip=collections.defaultdict(collections.Counter)
for r in pd:
    if r['src_ip'].startswith('10.45'): ue2ip[r['ue_index']][r['src_ip']]+=1
ue2ip={u:c.most_common(1)[0][0] for u,c in ue2ip.items()}
rnti2ue={r['rnti']:r['ue_index'] for r in crc if r['ue_index']!='1024'}
def lab(rnti): ue=rnti2ue.get(rnti,'?'); ip=ue2ip.get(ue,'?'); return f"{hex(int(rnti))}/ue{ue}/{ip}/{ip2s.get(ip,'-')}"
# proximity join: first CRC row of the same (rnti,harq) at or after the grant slot, within 20 ms (keys collide across SFN wraps)
cidx=collections.defaultdict(list)
for r in crc: cidx[(r['rnti'],r['harq_id'])].append((int(r['mono_ns']),r))
for k in cidx: cidx[k].sort(key=lambda x:x[0])
ckeys={k:[x[0] for x in v] for k,v in cidx.items()}
J=[]
for g in sched:
    k=(g['rnti'],g['harq_id']); lst=cidx.get(k)
    if not lst: continue
    gm=int(g['mono_ns']); i=bisect.bisect_left(ckeys[k],gm)
    if i>=len(lst) or lst[i][0]-gm>20e6: continue
    c=lst[i][1]
    t=int(g['mono_ns'])/1e9-T0
    J.append(dict(t=t,rnti=g['rnti'],h=g['harq_id'],mcs=int(g['mcs']),retx=int(g['nof_retxs']),rb=int(g['rb_count']),rbs=int(g['rb_start']),ngr=int(g['nof_grants_in_slot']),
        slot=int(g['slot'])%TDD_PERIOD, ok=c['crc_ok'] in ('1','true','True'), sinr=float(c['ul_sinr_db']) if c['ul_sinr_db'] not in ('','nan') else None,
        olla=float(g['olla_offset']) if g['olla_offset'] not in ('','nan') else None, tbs=int(g['tbs_bytes']), nd=g['new_data']=='1', gm=int(g['mono_ns']), cmn=int(c['mono_ns'])))
run=[j for j in J if -1<=j['t']<=WEND]
def rate(l): return f"{100*sum(1 for j in l if not j['ok'])/len(l):.1f}%(n={len(l)})" if l else "-"
def by(key,l,label,minn=20):
    g=collections.defaultdict(list)
    for j in l: g[key(j)].append(j)
    print(f"- fail by {label}: "+"  ".join(f"{k}:{rate(g[k])}" for k in sorted(g, key=lambda x:(x is None, x)) if len(g[k])>=minn))
print("grant->CRC latency ms (med/p99/max) per 10 s:", [f"{st.median(v):.2f}/{q(v,.99):.2f}/{max(v):.2f}" for k,v in sorted(collections.defaultdict(list, {b:[(j['cmn']-j['gm'])/1e6 for j in run if int(j['t']//10)==b] for b in range(0,int(WEND//10)+1)}).items()) if v])
by(lambda j:lab(j['rnti']), run, 'UE')
by(lambda j:j['mcs'], run, 'mcs', 50)
by(lambda j:min(j['retx'],3), run, 'nof_retxs')
by(lambda j:j['ngr'], run, 'UEs sharing slot')
by(lambda j:j['slot'], run, f'slot idx (period {TDD_PERIOD})')
by(lambda j:(j['rb']//10)*10, run, 'rb_count bin')
by(lambda j:(j['rbs']//10)*10, run, 'rb_start bin')
by(lambda j:None if j['sinr'] is None else int(j['sinr']//5)*5, run, 'sinr bin', 50)
by(lambda j:None if j['olla'] is None else round(j['olla']), run, 'olla offset (rounded)', 50)
nd27=[j for j in run if j['nd'] and j['mcs']==27]
g=collections.defaultdict(list)
for j in nd27: g[(j['slot'],min(j['ngr'],4))].append(j)
print("- new-data MCS27 fail by (slot, UEs sharing):", "  ".join(f"{k}:{rate(g[k])}" for k in sorted(g) if len(g[k])>=20))
print("- per UE per 10 s: fail% / mcs / olla / rb / grants / retx% / sinr")
g=collections.defaultdict(list)
for j in run: g[(j['rnti'],int(j['t']//10))].append(j)
for rnti in sorted(set(j['rnti'] for j in run)):
    line=[]
    for b in range(0,int(WEND//10)+1):
        v=g.get((rnti,b))
        if v: line.append(f"t{b*10}: {100*sum(1 for x in v if not x['ok'])/len(v):.0f}%/{st.mean(x['mcs'] for x in v):.0f}/{f1(st.mean(x['olla'] for x in v if x['olla'] is not None)) if any(x['olla'] is not None for x in v) else '-'}/{st.mean(x['rb'] for x in v):.0f}/{len(v)}/{100*sum(1 for x in v if x['retx']>0)/len(v):.0f}%/{st.mean(x['sinr'] for x in v if x['sinr'] is not None):.0f}")
    print(f"  {lab(rnti)}: "+"  ".join(line))
# HARQ completion & retx gap
J.sort(key=lambda x:(x['rnti'],x['h'],x['t']))
proc=collections.defaultdict(list); aband=collections.Counter(); gap=collections.defaultdict(list); cur={}
for j in J:
    k=(j['rnti'],j['h'])
    if j['nd']:
        if k in cur and not cur[k]['done']: aband[j['rnti']]+=1
        cur[k]={'t0':j['t'],'done':False,'tl':j['t']}
    else:
        if k in cur: gap[j['rnti']].append((j['t']-cur[k]['tl'])*1e3)
    if k in cur:
        cur[k]['tl']=j['t']
        if j['ok'] and not cur[k]['done'] and -1<=cur[k]['t0']<=WEND:
            cur[k]['done']=True; proc[j['rnti']].append((j['cmn']/1e9-T0-cur[k]['t0'])*1e3)
print("- HARQ completion ms (med/p90/p99/max), abandoned (->RLC), retx gap med ms:")
for rnti in sorted(proc): v=proc[rnti]; print(f"  {lab(rnti)}: {st.median(v):.1f}/{q(v,.9):.1f}/{q(v,.99):.1f}/{max(v):.1f} abandoned={aband[rnti]} gap={st.median(gap[rnti]) if gap[rnti] else '-'}")
# PRB usage per slot idx in UL slots
prb=collections.defaultdict(lambda:collections.defaultdict(int))
for j in run: prb[int(j['t']//10)][j['slot']]+=j['rb']
print("- PUSCH PRBs used per 10 s per slot idx (sum over 10 s; slots per 10 s = 10000/period*... compare relative):", {b:dict(v) for b,v in sorted(prb.items())})
# RLC reassembly expired / BSR per UE per 10 s
rlc=rows(f'{RD}/gnb/gnb_rlc_ul.csv'); bsr=rows(f'{RD}/gnb/gnb_bsr.csv')
re_=collections.defaultdict(int); bb=collections.defaultdict(list)
for r in rlc:
    t=int(r['mono_ns'])/1e9-T0
    if r['event']=='reassembly_expired' and -1<=t<=WEND: re_[(r['ue_index'],int(t//10))]+=1
for r in bsr:
    t=int(r['mono_ns'])/1e9-T0
    if -1<=t<=WEND: bb[(r['ue_index'],int(t//10))].append(int(r['buffer_bytes']))
for ue in sorted(ue2ip):
    print(f"  ue{ue}/{ue2ip[ue]}/{ip2s.get(ue2ip[ue],'-')}: reasm_expired/10s={[re_.get((ue,b),0) for b in range(0,int(WEND//10)+1)]} bsr mean/max KB={[f'{st.mean(bb[(ue,b)])/1e3:.1f}/{max(bb[(ue,b)])/1e3:.0f}' for b in range(0,int(WEND//10)+1) if (ue,b) in bb]}")

ha=rows(f'{RD}/gnb/gnb_dl_harq_ack.csv'); c=collections.Counter(); ps=[]
for r in ha:
    t=int(r['mono_ns'])/1e9-T0
    if -1<=t<=WEND: c[r['ack']]+=1; 
    if -1<=t<=WEND and r['pucch_sinr_db'] not in ('','nan'): ps.append(float(r['pucch_sinr_db']))
tot=sum(c.values()); print(f"- DL HARQ-ACK on PUCCH (run window): ack={100*c['1']/tot:.1f}% nack={100*c['0']/tot:.1f}% dtx={100*c['2']/tot:.1f}% n={tot}; pucch sinr med={st.median(ps) if ps else '-'}")
agg=collections.defaultdict(float)
for s_ in streams:
    prev=None
    for line in open(f'{RD}/app/{s_}-rx-stats.jsonl'):
        j=json.loads(line); t=j['mono_ns']/1e9-T0
        for x in j['stats']:
            if x['type']=='inbound-rtp' and x.get('kind')=='video':
                if prev: agg[int(t//BIN)]+=(x['bytesReceived']-prev)*8/(BIN*1000)
                prev=x['bytesReceived']
print(f"- aggregate received kbps per {BIN} s:", [round(agg[i]) for i in range(0,NB)])
