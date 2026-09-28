# exp_ran_audit.py <results/run>  — RAN-side audit of one run: slot occupancy, per-UE grants/fairness, SR/BSR->grant
# latency, SINR/RSRP/TA/MCS/OLLA stability, PUCCH/HARQ-ACK/CSI, DL, anomalies, HARQ/RLC recoveries, MAC->PDCP
# hold time, UL capacity headroom. Offline only; reads a run directory, writes nothing.
import csv,collections,bisect,statistics as st,re,json,math,sys
RD=sys.argv[1]; PER=5; UL_SLOT=4; NPRB=51; SLOT_S=0.0005
def rows(p):
    with open(p) as f: return list(csv.DictReader(l for l in f if not l.startswith('#')))
def q(v,p): v=sorted(v); return v[min(len(v)-1,int(p*len(v)))] if v else float('nan')
def f(v,d=1): return "-" if v is None or (isinstance(v,float) and math.isnan(v)) else f"{v:.{d}f}"
fr=rows(f'{RD}/app/cam0-rx-frames.csv'); T0=int(fr[0]['recv_mono_ns'])/1e9; W=(0,60)
def t(r): return int(r['mono_ns'])/1e9-T0
def inw(r): return W[0]<=t(r)<=W[1]
sched=[r for r in rows(f'{RD}/gnb/gnb_sched_ul.csv') if inw(r)]; sdl=[r for r in rows(f'{RD}/gnb/gnb_sched_dl.csv') if inw(r)]
crc=rows(f'{RD}/gnb/gnb_ul_crc.csv'); ha=[r for r in rows(f'{RD}/gnb/gnb_dl_harq_ack.csv') if inw(r)]; bsr=[r for r in rows(f'{RD}/gnb/gnb_bsr.csv') if inw(r)]
sr=[r for r in rows(f'{RD}/gnb/gnb_sr.csv') if inw(r)]; csi=[r for r in rows(f'{RD}/gnb/gnb_csi.csv') if inw(r)]; mac=[r for r in rows(f'{RD}/gnb/gnb_mac_ul_pdu.csv') if inw(r)]
rlc=[r for r in rows(f'{RD}/gnb/gnb_rlc_ul.csv') if inw(r)]; pul=[r for r in rows(f'{RD}/gnb/gnb_pdcp_ul.csv') if inw(r)]; pdl=[r for r in rows(f'{RD}/gnb/gnb_pdcp_dl.csv') if inw(r)]
ue2ip=collections.defaultdict(collections.Counter)
for r in pul:
    if r['src_ip'].startswith('10.45'): ue2ip[r['ue_index']][r['src_ip']]+=1
ue2ip={u:c.most_common(1)[0][0] for u,c in ue2ip.items()}
rnti2ue={r['rnti']:r['ue_index'] for r in crc if r['ue_index']!='1024'}
ip2s={}; last=None
for line in open(f'{RD}/app/signaling.log'):
    m=re.search(r"connection from \('([\d.]+)'",line); last=m.group(1) if m else last
    m=re.search(r"sender (\w+) registered",line)
    if m and last and last.startswith('10.45'): ip2s[last]=m.group(1)
def lab(rnti): ue=rnti2ue.get(rnti,'?'); ip=ue2ip.get(ue,'?'); return f"{ip2s.get(ip,'-')}({hex(int(rnti))})"
UES=sorted(set(r['rnti'] for r in sched), key=lambda r: lab(r))
# proximity join sched<->crc
cidx=collections.defaultdict(list)
for r in crc: cidx[(r['rnti'],r['harq_id'])].append((int(r['mono_ns']),r))
for k in cidx: cidx[k].sort(key=lambda x:x[0])
ck={k:[x[0] for x in v] for k,v in cidx.items()}
J=[]
for g in sched:
    k=(g['rnti'],g['harq_id']); lst=cidx.get(k)
    if not lst: continue
    gm=int(g['mono_ns']); i=bisect.bisect_left(ck[k],gm)
    if i>=len(lst) or lst[i][0]-gm>20e6: continue
    c=lst[i][1]; g['_ok']=c['crc_ok'] in ('1','true','True'); g['_sinr']=float(c['ul_sinr_db']) if c['ul_sinr_db'] not in ('','nan') else None
    g['_rsrp']=float(c['ul_rsrp_dbfs']) if c['ul_rsrp_dbfs'] not in ('','nan') else None; g['_ta']=float(c['ta_us']) if c['ta_us'] not in ('','nan') else None; J.append(g)
print(f"# RAN audit {RD}  window {W} s (gNB mono rel. first rx frame)  UEs: {[lab(u) for u in UES]}")
print("\n## 1. Slot occupancy (cell)")
ulslots=collections.defaultdict(lambda:[0,set()]); 
for g in sched:
    k=int(g['mono_ns'])//500000; ulslots[k][0]+=int(g['rb_count']); ulslots[k][1].add(g['rnti'])
n_ul_slots=int(60/(PER*SLOT_S)); used=len(ulslots); prb=[v[0] for v in ulslots.values()]
print(f"UL slots in window ~{n_ul_slots}; with >=1 grant: {used} ({100*used/n_ul_slots:.0f}%). PRBs/used slot: med {q(prb,.5):.0f} p90 {q(prb,.9):.0f} max {max(prb)} of {NPRB}. Avg PRB utilisation over all UL slots: {100*sum(prb)/(n_ul_slots*NPRB):.0f}%")
print("UEs per used UL slot:", dict(sorted(collections.Counter(len(v[1]) for v in ulslots.values()).items())))
persec=collections.defaultdict(int)
for k,v in ulslots.items(): persec[int((k*500000/1e9)-T0)]+=v[0]
print("UL PRB utilisation % per 5 s:", [round(100*sum(persec[s] for s in range(b*5,b*5+5))/(5*400*NPRB)) for b in range(12)])
dlslots=collections.defaultdict(int)
for g in sdl: dlslots[int(g['mono_ns'])//500000]+=int(g['rb_count'])
n_dl_slots=int(60/SLOT_S*3/5)
print(f"DL: slots with PDSCH {len(dlslots)} of ~{n_dl_slots} full DL slots ({100*len(dlslots)/n_dl_slots:.0f}%); avg DL PRB utilisation {100*sum(dlslots.values())/(n_dl_slots*NPRB):.1f}% (DL carries RTCP/TWCC feedback only)")
print("\n## 2. Grants per UE (UL)")
print(f"{'UE':<14}{'grants/s':>9}{'PRB share%':>11}{'PRB/grant':>10}{'TBS med B':>10}{'MCS p10/50/90':>15}{'retx%':>7}{'CRC fail%':>10}{'new-data fail%':>15}{'granted kbps':>13}{'delivered kbps':>15}{'BSR med/p90 KB':>16}{'grant gap p90 ms':>17}")
tot_prb=sum(int(g['rb_count']) for g in sched); share={}; delivered={}
pulb=collections.defaultdict(int)
for r in pul:
    if r['dst_ip']=='10.53.1.1': pulb[rnti_for:=None] if False else None
ue_bytes=collections.defaultdict(int)
for r in pul:
    if r['dst_ip']=='10.53.1.1': ue_bytes[r['ue_index']]+=int(r['sdu_bytes'])
for u in UES:
    gs=[g for g in sched if g['rnti']==u]; js=[g for g in J if g['rnti']==u]
    p=sum(int(g['rb_count']) for g in gs); share[u]=p/tot_prb
    mcs=[int(g['mcs']) for g in gs]; retx=100*sum(1 for g in gs if int(g['nof_retxs'])>0)/len(gs)
    fail=100*sum(1 for g in js if not g['_ok'])/len(js); nd=[g for g in js if g['new_data']=='1']; ndf=100*sum(1 for g in nd if not g['_ok'])/len(nd)
    okb=sum(int(g['tbs_bytes']) for g in js if g['_ok'] and g['new_data']=='1')*8/60/1000
    ue=rnti2ue.get(u); dl=ue_bytes.get(ue,0)*8/60/1000; delivered[u]=dl
    b=[int(r['buffer_bytes']) for r in bsr if r['rnti']==u]
    ts=sorted(int(g['mono_ns']) for g in gs); gaps=[(b2-a)/1e6 for a,b2 in zip(ts,ts[1:])]
    print(f"{lab(u):<14}{len(gs)/60:>9.0f}{100*share[u]:>11.1f}{p/len(gs):>10.1f}{st.median(int(g['tbs_bytes']) for g in gs):>10.0f}{f'{q(mcs,.1):.0f}/{q(mcs,.5):.0f}/{q(mcs,.9):.0f}':>15}{retx:>7.1f}{fail:>10.1f}{ndf:>15.1f}{okb:>13.0f}{dl:>15.0f}{f'{q(b,.5)/1e3:.1f}/{q(b,.9)/1e3:.1f}':>16}{q(gaps,.9):>17.1f}")
sh=list(share.values()); print(f"Jain fairness of PRB share: {sum(sh)**2/(len(sh)*sum(x*x for x in sh)):.3f} (1 = equal)  | delivered kbps sum {sum(delivered.values()):.0f}")
print("\n## 3. Scheduling latency")
srs=collections.defaultdict(list); grants=collections.defaultdict(list)
for r in sr: srs[r['rnti']].append(int(r['mono_ns']))
for g in sched: grants[g['rnti']].append(int(g['mono_ns']))
for u in UES:
    gl=sorted(grants[u]); d=[]
    for s in srs[u]:
        i=bisect.bisect_left(gl,s)
        if i<len(gl): d.append((gl[i]-s)/1e6)
    # BSR>0 -> next grant
    bl=sorted((int(r['mono_ns']),int(r['buffer_bytes'])) for r in bsr if r['rnti']==u and int(r['buffer_bytes'])>0); d2=[]
    for m,_ in bl:
        i=bisect.bisect_right(gl,m)
        if i<len(gl): d2.append((gl[i]-m)/1e6)
    print(f"  {lab(u):<14} SR detected {len(srs[u]):>4}  SR->grant med/p90 {f(q(d,.5))}/{f(q(d,.9))} ms | BSR>0 reports {len(bl):>5}  BSR->next grant med/p90/max {f(q(d2,.5))}/{f(q(d2,.9))}/{f(max(d2) if d2 else None)} ms")
print("\n## 4. Reception quality & stability per UE (5 s bins): SINR med | RSRP med dBFS | TA us | MCS | OLLA | fail%")
for u in UES:
    js=[g for g in J if g['rnti']==u]; line=[]
    for b in range(0,12):
        v=[g for g in js if b*5<=t(g)<b*5+5]
        if not v: line.append("   -   "); continue
        s=[g['_sinr'] for g in v if g['_sinr'] is not None]; ta=[g['_ta'] for g in v if g['_ta'] is not None]; rs=[g['_rsrp'] for g in v if g['_rsrp'] is not None]
        line.append(f"{q(s,.5):.0f}|{q(rs,.5):.0f}|{st.mean(ta):.2f}|{st.median(int(g['mcs']) for g in v):.0f}|{st.median(float(g['olla_offset']) for g in v if g['olla_offset'] not in ('','nan')):.0f}|{100*sum(1 for g in v if not g['_ok'])/len(v):.0f}")
    print(f"  {lab(u):<14} " + "  ".join(line))
ta_all=[g['_ta'] for g in J if g['_ta'] is not None]; print(f"  TA overall: med {st.median(ta_all):.2f} us, p99 {q(ta_all,.99):.2f}, max {max(ta_all):.2f} (CP 30 kHz = 2.34 us)")
print("\n## 5. Control channels: PUCCH (PHY log), HARQ-ACK, SR, CSI")
pat=re.compile(r'\[\s*(\d+)\.(\d+)\] PUCCH: rnti=(0x[0-9a-f]+) format=(\d) .*?(ack=(\S+)|sr=(\S+)|csi[^ ]*) metric=([\d.]+) sinr=([-\d.]+)dB')
pc=collections.defaultdict(lambda:collections.defaultdict(list))
for line in open(f'{RD}/gnb/gnb.log'):
    m=pat.search(line)
    if not m: continue
    rnti=str(int(m[3],16)); kind='ack' if m[6] else ('sr' if m[7] else 'csi'); val=m[6] or m[7] or ''
    pc[rnti][kind].append((val,float(m[8]),float(m[9])))
for u in UES:
    a=pc[u]['ack']; s_=pc[u]['sr']; c=pc[u]['csi']
    ackv=collections.Counter(x[0] for x in a); srv=collections.Counter(x[0] for x in s_)
    print(f"  {lab(u):<14} PUCCH ACK occasions {len(a):>5} (ack {ackv.get('1',0)}, nack {ackv.get('0',0)}, dtx/other {len(a)-ackv.get('1',0)-ackv.get('0',0)}) sinr med {f(q([x[2] for x in a],.5))} dB | SR occasions {len(s_):>5} positive {srv.get('yes',0)} sinr(pos) med {f(q([x[2] for x in s_ if x[0]=='yes'],.5))} | CSI occasions {len(c):>4} sinr med {f(q([x[2] for x in c],.5))}")
hu=collections.defaultdict(collections.Counter)
for r in ha: hu[r['rnti']][r['ack']]+=1
cu=collections.defaultdict(list); ri=collections.defaultdict(list); cv=collections.defaultdict(lambda:[0,0])
for r in csi:
    cv[r['rnti']][0]+=1
    if r['valid'] in ('1','true','True'): cv[r['rnti']][1]+=1; cu[r['rnti']].append(int(r['cqi'])); ri[r['rnti']].append(int(r['ri']))
for u in UES:
    tot=sum(hu[u].values()) or 1
    print(f"  {lab(u):<14} DL HARQ-ACK: ack {100*hu[u]['1']/tot:.0f}% nack {100*hu[u]['0']/tot:.0f}% dtx {100*hu[u]['2']/tot:.0f}% (n={tot}) | CSI valid {cv[u][1]}/{cv[u][0]} CQI p10/50/90 {f(q(cu[u],.1),0)}/{f(q(cu[u],.5),0)}/{f(q(cu[u],.9),0)} RI {collections.Counter(ri[u]).most_common(2)}")
print("\n## 6. DL per UE: grants, MCS, retx, BLER (from HARQ-ACK), PDCP DL bytes")
pdlb=collections.defaultdict(int)
for r in pdl: pdlb[r['ue_index']]+=int(r['sdu_bytes'])
for u in UES:
    d=[g for g in sdl if g['rnti']==u]; 
    if not d: continue
    tot=sum(hu[u].values()) or 1
    print(f"  {lab(u):<14} DL grants {len(d):>5} ({len(d)/60:.0f}/s) MCS med {st.median(int(g['mcs']) for g in d):.0f} PRB/grant {st.mean(int(g['rb_count']) for g in d):.1f} retx {100*sum(1 for g in d if int(g['nof_retxs'])>0)/len(d):.0f}% | NACK+DTX {100*(hu[u]['0']+hu[u]['2'])/tot:.0f}% | PDCP DL {pdlb.get(rnti2ue.get(u),0)*8/60/1000:.0f} kbps")
print("\n## 7. Errors / anomalies")
pr=sum(1 for line in open(f'{RD}/gnb/gnb.log') if 'PRACH:' in line and 'detected_preambles=[{' in line)
print(f"  PRACH detections in whole log: {pr}; RLF: {sum(1 for l in open(f'{RD}/gnb/gnb.log') if 'RLF detected' in l)}; UCI discards: {sum(1 for l in open(f'{RD}/gnb/gnb.log') if 'Discarding UCI' in l)}")
cm=collections.defaultdict(float); n=0
for line in open(f'{RD}/gnb/gnb_metrics.jsonl'):
    j=json.loads(line); m=j['metrics']
    if 'cells' not in m or not (0<=j['recv_mono_ns']/1e9-T0<=60): continue
    n+=1
    for c in m['cells']:
        for k in ['late_dl_harqs','late_ul_harqs','nof_failed_pdcch_allocs','nof_failed_uci_allocs','error_indication_count','msg3_nof_nok','msg3_nof_ok','pucch_tot_rb_usage_avg']: cm[k]+=float(c['cell_metrics'].get(k,0) or 0)
print(f"  cell metrics sums over {n} samples: {dict((k,round(v,1)) for k,v in cm.items())}")
# HARQ: abandoned processes (new_data on an unfinished process), retx timeouts
J.sort(key=lambda g:(g['rnti'],g['harq_id'],int(g['mono_ns']))); cur={}; ab=collections.Counter(); comp=collections.defaultdict(list)
for g in J:
    k=(g['rnti'],g['harq_id'])
    if g['new_data']=='1':
        if k in cur and not cur[k][1]: ab[g['rnti']]+=1
        cur[k]=[int(g['mono_ns']),False]
    if k in cur and g['_ok'] and not cur[k][1]:
        cur[k][1]=True; comp[g['rnti']].append((int(g['mono_ns'])-cur[k][0])/1e6+4.1)
re_=collections.Counter(); stc=collections.Counter()
for r in rlc:
    if r['event']=='reassembly_expired': re_[r['ue_index']]+=1
    if r['event']=='status_pdu_rx': stc[r['ue_index']]+=1
for u in UES:
    ue=rnti2ue.get(u); c=comp[u]
    print(f"  {lab(u):<14} HARQ processes abandoned after max retx (-> RLC ARQ): {ab[u]:>3} | HARQ completion med/p90/p99/max {f(q(c,.5))}/{f(q(c,.9))}/{f(q(c,.99))}/{f(max(c) if c else None)} ms | RLC t-Reassembly expiries {re_[ue]:>3}, status PDUs {stc[ue]}")
# MAC padding: mac_ul_pdu bytes vs tbs
mi=collections.defaultdict(list)
for r in mac: mi[(r['rnti'],r['harq_id'])].append((int(r['mono_ns']),int(r['pdu_bytes'])))
for k in mi: mi[k].sort()
pad=collections.defaultdict(lambda:[0,0])
for g in J:
    if not g['_ok']: continue
    k=(g['rnti'],g['harq_id']); lst=mi.get(k)
    if not lst: continue
    ks=[x[0] for x in lst]; i=bisect.bisect_left(ks,int(g['mono_ns']))
    if i<len(lst) and ks[i]-int(g['mono_ns'])<20e6: pad[g['rnti']][0]+=int(g['tbs_bytes']); pad[g['rnti']][1]+=lst[i][1]
print("  grant efficiency (MAC PDU bytes / TBS of successful grants):", {lab(u):f"{100*pad[u][1]/pad[u][0]:.0f}%" for u in UES if pad[u][0]})
print("\n## 8. RAN-internal UL delay: MAC PDU rx -> PDCP SDU delivered (same UE, RLC reordering/hold), ms")
macs=collections.defaultdict(list)
for r in mac: macs[r['ue_index']].append(int(r['mono_ns']))
for k in macs: macs[k].sort()
for u in UES:
    ue=rnti2ue.get(u); ml=macs.get(ue,[]); d=[]
    for r in pul:
        if r['ue_index']!=ue or r['dst_ip']!='10.53.1.1': continue
        m=int(r['mono_ns']); i=bisect.bisect_right(ml,m)-1
        if i>=0: d.append((m-ml[i])/1e6)
    print(f"  {lab(u):<14} n={len(d):>5} med {f(q(d,.5),2)} p90 {f(q(d,.9),2)} p99 {f(q(d,.99),1)} max {f(max(d) if d else None,1)}  (>20 ms: {100*sum(1 for x in d if x>20)/max(1,len(d)):.1f}%)")
print("\n## 9. Capacity headroom (UL)")
se={0:0.2344,1:0.377,2:0.6016,3:0.877,4:1.1758,5:1.4766,6:1.6953,7:1.9141,8:2.1602,9:2.4063,10:2.5703,11:2.7305,12:3.0293,13:3.3223,14:3.6094,15:3.9023,16:4.2129,17:4.5234,18:4.8164,19:5.1152,20:5.332,21:5.5547,22:5.8906,23:6.2266,24:6.5703,25:6.9141,26:7.1602,27:7.4063}
for u in UES:
    gs=[g for g in sched if g['rnti']==u and g['new_data']=='1']; m=st.median(int(g['mcs']) for g in gs); js=[g for g in J if g['rnti']==u and g['new_data']=='1']; fr_=sum(1 for g in js if not g['_ok'])/len(js)
    full=se[int(m)]*12*12*NPRB*400*(1-fr_)/1e6   # bits/s if the UE had the whole UL slot every 2.5 ms (12 data symbols approx)
    print(f"  {lab(u):<14} median new-data MCS {m:.0f} -> if given all 51 PRB in every UL slot: ~{full:.1f} Mbps (x(1-BLER)); actual PRB share {100*share[u]:.0f}% -> fair-share ceiling ~{full*share[u]/max(share.values())*max(share.values()):.2f} Mbps at current share; delivered {delivered[u]/1000:.2f} Mbps")
print(f"  cell: UL slot PRB utilisation avg {100*sum(prb)/(n_ul_slots*NPRB):.0f}% -> roughly {100-100*sum(prb)/(n_ul_slots*NPRB):.0f}% of UL resource unused in this run")
