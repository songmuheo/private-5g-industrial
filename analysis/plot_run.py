# plot_run.py <results/run> [bitrate|delay|all]  — graphs of one run into <run>/graphs/*.png
#
#   bitrate  : per UE, GoogCC target bitrate (tx-cc target_bps, solid line) drawn over the bandwidth estimate
#              (tx-events bwe_delay a=bps, wide translucent band) and the stable target (tx-cc stable_target_bps,
#              dotted). tx-cc's est_bandwidth_bps is deprecated in M120 (always -1), so the estimate is taken from
#              the delay-based estimator event. The bwe_loss event is not drawn: in this build LossBasedBweV2 is
#              bounded by the delay-based estimate and its logged value equals bwe_delay (checked on 20260929-192247:
#              1 of 725 rows differed), so it would only overdraw the same line. Where target == estimate the band
#              simply haloes the line; the band shows through where they differ (probe ramps, backoff lag).
#   delay    : top = per-packet one-way delay (rx log_wall_ns - tx log_wall_ns, RTP joined by (ssrc, seq),
#              sequence-wrap aware); bottom = per-frame delay: capture -> last packet arrived (network) and
#              capture -> delivered to the app (network + jitter buffer + decode). Absolute values are valid
#              when both hosts were chrony-synced (the run's clock-*.txt records the RMS offset; it is printed on
#              the figure). If a laptop reports > 1 ms or no chrony, that camera is min-normalised and labelled.
#
# Needs the sender traces collected by run_experiment.sh (<run>/senders/camK/app). One hue per camera, fixed
# order cam0..cam4 (never cycled); colours from the project's validated categorical palette.
import csv, sys, os, glob, re, bisect, collections
RD = sys.argv[1].rstrip('/'); WHAT = sys.argv[2] if len(sys.argv) > 2 else 'all'
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
OUT = f'{RD}/graphs'; os.makedirs(OUT, exist_ok=True)
NTP_UNIX_MS = 2208988800000
PALETTE = {'cam0': '#2a78d6', 'cam1': '#eb6834', 'cam2': '#1baf7a', 'cam3': '#eda100', 'cam4': '#e87ba4',
           'cam5': '#008300', 'cam6': '#4a3aa7', 'cam7': '#e34948'}
INK, INK2, GRID, SURFACE = '#1f1f1f', '#5f5f5f', '#e6e6e6', '#ffffff'
plt.rcParams.update({'font.size': 9, 'axes.edgecolor': INK2, 'axes.labelcolor': INK, 'xtick.color': INK2, 'ytick.color': INK2,
                     'axes.spines.top': False, 'axes.spines.right': False, 'figure.facecolor': SURFACE, 'axes.facecolor': SURFACE,
                     'axes.grid': True, 'grid.color': GRID, 'grid.linewidth': 0.6, 'legend.frameon': False})

def rows(p):
    with open(p) as f: return list(csv.DictReader(l for l in f if not l.startswith('#')))
cams = sorted(os.path.basename(d) for d in glob.glob(f'{RD}/senders/cam*') if os.path.isdir(d) and os.path.isdir(f'{d}/app'))
if not cams: sys.exit(f"no sender traces under {RD}/senders/ (run_experiment.sh collects them)")
# common time origin: first received frame of the first camera (gNB PC wall clock)
T0 = min(int(rows(f'{RD}/app/{c}-rx-frames.csv')[0]['recv_wall_ns']) for c in cams if os.path.exists(f'{RD}/app/{c}-rx-frames.csv'))
def tw(ns): return (ns - T0)/1e9      # wall ns -> seconds since T0
def sync_ms(cam):
    """chrony RMS offset (ms) recorded by run_sender.sh at start; None if chrony was not running."""
    p = f'{RD}/senders/{cam}/app/clock-{cam}.txt'
    if not os.path.exists(p): return None
    m = re.search(r'RMS offset\s*:\s*([0-9.]+) seconds', open(p).read()); return float(m.group(1))*1000 if m else None
run_name = os.path.basename(RD)

def plot_bitrate():
    fig, ax = plt.subplots(figsize=(11, 4.2), dpi=130)
    for cam in cams:
        A = f'{RD}/senders/{cam}/app'; col = PALETTE.get(cam, INK2)
        ccr = rows(f'{A}/{cam}-tx-cc.csv')
        cc = [(tw(int(r['log_wall_ns'])), int(r['target_bps'])/1e6) for r in ccr if r['target_bps'] not in ('-1', '')]
        st = [(tw(int(r['log_wall_ns'])), int(r['stable_target_bps'])/1e6) for r in ccr if r['stable_target_bps'] not in ('-1', '')]
        dl = [(tw(int(r['log_wall_ns'])), int(r['a'])/1e6) for r in rows(f'{A}/{cam}-tx-events.csv') if r['event'] == 'bwe_delay' and r['a'] not in ('-1', '')]
        if dl: ax.step(*zip(*dl), where='post', color=col, lw=4.5, alpha=0.28, solid_capstyle='butt', label=f'{cam} bandwidth estimate (delay-based)')
        if st: ax.step(*zip(*st), where='post', color=col, lw=1.0, ls=':', label=f'{cam} stable target')
        if cc: ax.step(*zip(*cc), where='post', color=col, lw=1.4, label=f'{cam} target')
    ax.set_xlabel('time since first received frame (s)'); ax.set_ylabel('bitrate (Mbps)'); ax.set_ylim(bottom=0)
    ax.set_title(f'GoogCC target and estimated bitrate per UE — {run_name}', loc='left', color=INK)
    ax.legend(ncol=len(cams), fontsize=8, loc='upper center', bbox_to_anchor=(0.5, -0.18))
    fig.tight_layout(); fig.savefig(f'{OUT}/bitrate.png'); plt.close(fig); print(f'{OUT}/bitrate.png')

def plot_delay():
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), dpi=130, sharex=True)
    notes = []
    for cam in cams:
        A = f'{RD}/senders/{cam}/app'; col = PALETTE.get(cam, INK2)
        tx = rows(f'{A}/{cam}-tx-rtp.csv'); rx = rows(f'{RD}/app/{cam}-rx-rtp.csv'); frs = rows(f'{RD}/app/{cam}-rx-frames.csv')
        txi = collections.defaultdict(list)
        for r in tx:
            if r['dir'] == 'out': txi[(r['ssrc'], r['seq'])].append(int(r['log_wall_ns']))
        for k in txi: txi[k].sort()
        pkts = []   # (t, one-way delay ms) using wall clocks; wrap-aware: nearest earlier tx within 5 s
        for r in rx:
            if r['dir'] != 'in': continue
            l = txi.get((r['ssrc'], r['seq']))
            if not l: continue
            rw = int(r['log_wall_ns']); i = bisect.bisect_right(l, rw) - 1
            if i < 0 or rw - l[i] > 5e9: continue
            pkts.append((tw(rw), (rw - l[i])/1e6))
        s = sync_ms(cam); label = cam
        if s is None or s > 1.0:
            base = min(v for _, v in pkts) if pkts else 0; pkts = [(t, v - base) for t, v in pkts]; label += ' (relative: clock not synced)'
            notes.append(f'{cam}: chrony {"absent" if s is None else f"{s:.1f} ms"} -> min-normalised')
        else: notes.append(f'{cam}: chrony RMS offset {s*1000:.0f} µs')
        if pkts: ax1.plot(*zip(*pkts), '.', ms=2.0, color=col, alpha=0.5, label=label, rasterized=True)
        # per-frame delays: capture -> last packet (network), capture -> delivered (app)
        net, app = [], []
        for r in frs:
            cap = int(r['abs_capture_ntp_ms']); lp = int(r['last_pkt_mono_ns']); rm = int(r['recv_mono_ns']); rw = int(r['recv_wall_ns'])
            if cap <= 0 or lp <= 0: continue
            cap_wall_ms = cap - NTP_UNIX_MS; t = tw(rw)
            net.append((t, (lp + rw - rm)/1e6 - cap_wall_ms)); app.append((t, rw/1e6 - cap_wall_ms))
        if s is None or s > 1.0:
            b = min(v for _, v in net) if net else 0; net = [(t, v-b) for t, v in net]; app = [(t, v-b) for t, v in app]
        if app: ax2.plot(*zip(*app), lw=0.8, color=col, alpha=0.9, label=f'{cam} capture → delivered')
        if net: ax2.plot(*zip(*net), lw=0.8, color=col, alpha=0.45, ls='--', label=f'{cam} capture → last packet')
    ax1.set_ylabel('per-packet one-way delay (ms)'); ax1.set_ylim(bottom=0)
    ax1.set_title(f'Packet and frame delay per UE — {run_name}', loc='left', color=INK)
    ax1.legend(fontsize=8, markerscale=4, loc='upper right')
    ax2.set_ylabel('per-frame delay (ms)'); ax2.set_ylim(bottom=0); ax2.set_xlabel('time since first received frame (s)')
    ax2.legend(fontsize=8, ncol=len(cams), loc='upper center', bbox_to_anchor=(0.5, -0.22))
    fig.text(0.01, 0.005, 'clocks: ' + '; '.join(notes), fontsize=7, color=INK2)
    fig.tight_layout(); fig.savefig(f'{OUT}/delay.png'); plt.close(fig); print(f'{OUT}/delay.png')

if WHAT in ('bitrate', 'all'): plot_bitrate()
if WHAT in ('delay', 'all'): plot_delay()
