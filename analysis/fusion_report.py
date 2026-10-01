#!/usr/bin/env python3
"""Multi-camera fusion analysis of one run (docs/SCENARIO_EDGE_PROFILES.md §5.6, items 1-6 and 8).

  .venv/bin/python analysis/fusion_report.py results/<run> [--group g1=cam0,cam1:100] [--warmup-s 5]

A fusion group G is a set of cameras whose frames captured at the SAME instant are consumed by one inference; the
inference for capture instant k can start only when every member's frame k has arrived, before the group deadline D
(ms after capture). The group comes from the run's scenario.json `"groups": [{"id", "cams", "deadline_ms"}]` (the edge's
descriptor; later carried by the profile) or from --group (overrides).

Frame identity: every camera captures on one 30 fps base grid anchored at the run's epoch T; a camera at f fps uses every
(30/f)-th base instant, so its grid_slot k is base instant k*30/f. A group frame is a base instant at which EVERY member
captured (mixed 15/30 fps groups -> the common, even instants). Every expected group frame counts in the deadline
statistics; a member frame that never arrived is a miss (latency infinite), never dropped from the denominator.

Time points per member i and frame k (wall clock, chrony across hosts):
  capture   tx-frames.capture_wall_ns                  (laptop)
  sent0/1   first / last tx-rtp row of the frame         (laptop, just before sendto)
  gNB g0    first packet of the frame in gnb_pdcp_ul      (RAN side)
  gNB g1    the frame's MARKER packet in gnb_pdcp_ul      (RAN-side completion; no marker seen = not complete)
  app       rx-frames.recv_wall_ns                        (receiver, decoded)
Items 1-2 use the app time (end-to-end); items 3, 4, 6 use gNB times (what the RAN can observe). Scheduler decisions are
reconstructed as gnb_sched_ul.wall_ns - k2 slots (the row is logged for the PUSCH slot; its DCI is k2 slots earlier).
Outputs (derived, CLAUDE.md rule 3): <run>/analysis/fusion_<gid>.txt, fusion_<gid>.json, graphs/fusion_<gid>_*.png.
Several outputs are HEURISTIC (marked as such): item 4 uses the laptop's sendto as a proxy for data waiting in the phone,
item 5 associates RAN events with a straggler frame by time window, not by causality.
"""
from __future__ import annotations

import argparse, bisect, collections, csv, itertools, json, math, os, statistics as st, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# validated categorical palette, fixed camera order (same as analysis/plot_run.py)
PALETTE = {"cam0": "#2a78d6", "cam1": "#eb6834", "cam2": "#1baf7a", "cam3": "#eda100", "cam4": "#e87ba4",
           "cam5": "#008300", "cam6": "#4a3aa7", "cam7": "#e34948"}
INK, INK2, GRID, SURFACE = "#1f1f1f", "#5f5f5f", "#e6e6e6", "#ffffff"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]   # non-camera series (predictors), same palette order
plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
                     "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "legend.frameon": False})

BASE_FPS = 30                   # capture base grid (every profile's fps divides it; prepare_video.sh)
SLOT_NS = 500_000               # UL slot duration at 30 kHz SCS (ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml common_scs: 30)
SPREAD_MEANINGFUL_MS = 5.0      # items 5, 6: a straggler "counts" when it completes this much after the first member
DECISION_OFFSETS_MS = (0, 5, 10)  # item 6: decision instants after the first member's frame-k data reaches the gNB
IDR_OFFSETS = range(-3, 11)     # item 8: group frames around a member's IDR
PHASE_FRAMES = 3                # 30 (and 15) fps vs 20 ms SR period vs 2.5 ms UL slot: grids realign every 3 frames
RTP_IP_UDP_OVERHEAD = 40        # bytes per packet in a PDCP SDU beyond the H.264 payload: IPv4 20 + UDP 8 + RTP 12 (RFC 3550 §5.1)
INF = float("inf")


def rows(path):
    with open(path) as f:   # skips a torn last line (a trace still being written by a live gNB session)
        yield from (r for r in csv.DictReader(l for l in f if not l.startswith("#")) if None not in r.values())


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p * (len(v) - 1)))))] if v else float("nan")


def dist(v):
    v = [x for x in v if x != INF]
    return {"n": len(v), "median": round(st.median(v), 2) if v else None, "mean": round(st.mean(v), 2) if v else None,
            "p90": round(pct(v, .9), 2) if v else None, "p99": round(pct(v, .99), 2) if v else None, "max": round(max(v), 2) if v else None}


def fmt(d):
    return f"median {d['median']:.1f}  p90 {d['p90']:.1f}  p99 {d['p99']:.1f}  max {d['max']:.1f}  (n={d['n']})" if d["n"] else "n=0"


# ------------------------------------------------------------------------------------------------ loading
def load(rd, cams, warmup_s):
    exp = json.load(open(f"{rd}/experiment.json"))
    T = int(round(exp["start_time_epoch"] * 1e9))
    dur = int(exp["scenario"].get("duration_s", 300))
    fps = {c: int(exp["scenario"]["cams"][c].get("fps", BASE_FPS)) for c in cams}
    C = {}
    for c in cams:
        a = f"{rd}/senders/{c}/app"; step = BASE_FPS // fps[c]
        enc = {r["rtp_ts"]: r for r in rows(f"{a}/{c}-tx-encoded.csv")}
        s0 = {}; s1 = {}; ssrc = None
        for r in rows(f"{a}/{c}-tx-rtp.csv"):
            ssrc = ssrc or r["ssrc"]; w = int(r["log_wall_ns"]); ts = r["rtp_ts"]
            s0[ts] = min(w, s0.get(ts, w)); s1[ts] = max(w, s1.get(ts, w))
        fr = {}
        for r in rows(f"{a}/{c}-tx-frames.csv"):
            ts = r["rtp_ts"]; e = enc.get(ts)
            fr[int(r["grid_slot"]) * step] = dict(ts=ts, cap=int(r["capture_wall_ns"]), src=int(r["src_frame_idx"]),
                                                  submitted=r["to_encoder"] == "1", sent0=s0.get(ts), sent1=s1.get(ts),
                                                  idr=(e is not None and e["is_idr"] == "1"), bytes=int(e["bytes"]) if e else 0)
        app = {r["rtp_ts"]: int(r["recv_wall_ns"]) for r in rows(f"{rd}/app/{c}-rx-frames.csv") if r["ssrc"] == ssrc}
        C[c] = dict(ssrc=ssrc, frames=fr, app=app, fps=fps[c])
    by_ssrc = {v["ssrc"]: c for c, v in C.items()}
    pk = {c: collections.defaultdict(list) for c in cams}   # (wall, sdu_bytes, marker) per rtp_ts, in arrival order
    ue = {}
    for r in rows(f"{rd}/gnb/gnb_pdcp_ul.csv"):
        if r["rtp_like"] != "1": continue
        c = by_ssrc.get(r["rtp_ssrc"])
        if c is None: continue
        ue[c] = r["ue_index"]
        pk[c][r["rtp_ts"]].append((int(r["wall_ns"]), int(r["sdu_bytes"]), r["rtp_marker"] == "1"))
    sched = []
    for r in rows(f"{rd}/gnb/gnb_sched_ul.csv"):
        r["dec_ns"] = int(r["wall_ns"]) - int(r["k_offset"] or 0) * SLOT_NS   # DCI (decision) instant
        sched.append(r)
    sched.sort(key=lambda r: r["dec_ns"])
    rnti = {}
    for r in sched: rnti.setdefault(r["ue_index"], r["rnti"])
    no_marker = 0
    for c in cams:
        if c not in ue: sys.exit(f"{c}: no RTP rows at the gNB PDCP for ssrc {C[c]['ssrc']} (not an OTA run?)")
        C[c]["ue"] = ue[c]; C[c]["rnti"] = rnti.get(ue[c])
        for k, f in C[c]["frames"].items():
            p = pk[c].get(f["ts"], [])
            f["g0"] = p[0][0] if p else None
            m = [w for w, _, mk in p if mk]
            f["g1"] = m[0] if m else None                   # completion = marker packet at the gNB
            if p and not m: no_marker += 1
            f["gpk"] = p
            f["app"] = C[c]["app"].get(f["ts"])
    lo = T + int(warmup_s * 1e9); hi = T + dur * 1_000_000_000
    common = sorted(set.intersection(*(set(C[c]["frames"]) for c in cams)))
    E = [k for k in common if lo <= min(C[c]["frames"][k]["cap"] for c in cams) < hi]    # expected group frames
    return T, dur, C, E, sched, no_marker


# ------------------------------------------------------------------------------------------------ analysis
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run"); ap.add_argument("--group", help="id=camA,camB[,...]:deadline_ms (overrides scenario.json groups)")
    ap.add_argument("--warmup-s", type=float, default=5.0)
    a = ap.parse_args(); rd = a.run.rstrip("/")
    if a.group:
        gid, rest = a.group.split("="); cams_s, d = rest.split(":"); groups = [{"id": gid, "cams": cams_s.split(","), "deadline_ms": float(d)}]
    else:
        groups = json.load(open(f"{rd}/scenario.json")).get("groups") or sys.exit("no groups: add \"groups\" to the scenario or pass --group id=cam0,cam1:100")
    out = f"{rd}/analysis"; gdir = f"{out}/graphs"; os.makedirs(gdir, exist_ok=True)
    for g in groups: analyse_group(rd, g, a.warmup_s, out, gdir)


def write(out, gid, res, txt):
    with open(f"{out}/fusion_{gid}.json", "w") as f: json.dump(res, f, indent=2)
    with open(f"{out}/fusion_{gid}.txt", "w") as f: f.write("\n".join(txt) + "\n")
    print("\n".join(txt)); print(f"\n[fusion_report] -> {out}/fusion_{gid}.txt, .json, graphs/fusion_{gid}_*.png")


def analyse_group(rd, g, warmup_s, out, gdir):
    gid, cams, D = g["id"], list(g["cams"]), float(g["deadline_ms"])
    T, dur, C, E, sched, no_marker = load(rd, cams, warmup_s)
    if not E: print(f"[fusion_report] group {gid}: no common capture instants in the window"); return
    F = lambda c, k: C[c]["frames"][k]
    fps_g = min(C[c]["fps"] for c in cams); period = 1000.0 / fps_g          # group frame period (ms)
    res = {"group": gid, "cams": cams, "deadline_ms": D, "warmup_s": warmup_s, "expected_frames": len(E), "group_fps": fps_g,
           "member_fps": {c: C[c]["fps"] for c in cams}, "frames_without_marker_at_gnb": no_marker}
    txt = [f"# fusion analysis {rd}  group {gid} = {'+'.join(cams)}  deadline {D:.0f} ms  group rate {fps_g} fps "
           f"(expected group frames with capture >= T+{warmup_s:g} s: {len(E)})",
           f"# same content frame at every member: {all(len({F(c, k)['src'] * (BASE_FPS // C[c]['fps']) for c in cams}) == 1 for k in E)}"
           f";  member frames seen at the gNB without their marker packet: {no_marker}"]
    cap = {k: min(F(c, k)["cap"] for c in cams) for k in E}
    lat = {c: [((F(c, k)["app"] - cap[k]) / 1e6) if F(c, k)["app"] else INF for k in E] for c in cams}
    glat = [max(lat[c][i] for c in cams) for i in range(len(E))]
    n = len(E)

    # ---------------------------------------------------------------- 1. group latency and deadline satisfaction
    missing = {c: sum(x == INF for x in lat[c]) for c in cams}
    sec1 = {"per_camera": {c: dict(dist(lat[c]), deadline_met=round(sum(x <= D for x in lat[c]) / n, 4), missing=missing[c]) for c in cams},
            "group": dict(dist(glat), deadline_met=round(sum(x <= D for x in glat) / n, 4), missing=sum(x == INF for x in glat))}
    sec1["group"]["independence_prediction"] = round(math.prod(sec1["per_camera"][c]["deadline_met"] for c in cams), 4)
    sec1["by_group_size"] = {}
    for m in range(2, len(cams) + 1):
        ok = [sum(max(lat[c][i] for c in S) <= D for i in range(n)) / n for S in itertools.combinations(cams, m)]
        sec1["by_group_size"][m] = {"subsets": len(ok), "deadline_met_mean": round(st.mean(ok), 4), "worst": round(min(ok), 4)}
    gl_g = [((max(F(c, k)["g1"] for c in cams) - cap[k]) / 1e6) if all(F(c, k)["g1"] for c in cams) else INF for k in E]
    sec1["group_at_gnb"] = dict(dist(gl_g), incomplete=sum(x == INF for x in gl_g))
    miss_bins = collections.defaultdict(lambda: [0, 0])
    for i, k in enumerate(E):
        b = int((cap[k] - T) / 10e9); miss_bins[b][1] += 1; miss_bins[b][0] += glat[i] > D
    res["1_group_latency"] = sec1
    txt += ["", f"## 1. group latency (capture -> last member decoded) and deadline D = {D:.0f} ms (denominator: all {n} expected group frames; a frame never decoded counts as a miss)"]
    for c in cams: txt.append(f"  {c:6s} {fmt(sec1['per_camera'][c])}  met {100 * sec1['per_camera'][c]['deadline_met']:.2f} %  missing {missing[c]}")
    txt.append(f"  GROUP  {fmt(sec1['group'])}  met {100 * sec1['group']['deadline_met']:.2f} %  "
               f"(if members were independent: {100 * sec1['group']['independence_prediction']:.2f} %)")
    txt.append(f"  group completion at the gNB (last member's marker packet): {fmt(sec1['group_at_gnb'])}, incomplete {sec1['group_at_gnb']['incomplete']}")
    for m, v in sec1["by_group_size"].items(): txt.append(f"  |G| = {m}: {v['subsets']} subsets, deadline met mean {100 * v['deadline_met_mean']:.2f} %, worst {100 * v['worst']:.2f} %")
    worst_bins = sorted(((v[0] / v[1], b) for b, v in miss_bins.items() if v[1]), reverse=True)[:3]
    txt.append("  worst 10 s bins (miss rate): " + ", ".join(f"t={10 * b}-{10 * b + 10} s {100 * r:.1f} %" for r, b in worst_bins))

    fig, ax = plt.subplots(1, 2, figsize=(12, 3.8), gridspec_kw={"width_ratios": [1, 1.6]})
    for c in cams:
        v = sorted(x for x in lat[c] if x != INF); ax[0].plot(v, [i / n for i in range(len(v))], color=PALETTE[c], lw=1.6, label=c)
    v = sorted(x for x in glat if x != INF); ax[0].plot(v, [i / n for i in range(len(v))], color=INK, lw=2, label=f"group {gid} (max)")
    ax[0].axvline(D, color=INK2, ls="--", lw=1); ax[0].text(D, 0.5, f" D={D:.0f} ms", color=INK2)
    ax[0].set_xscale("log"); ax[0].set_xlabel("capture -> decoded (ms, log)"); ax[0].set_ylabel("share of expected frames"); ax[0].legend(loc="upper left")
    ax[0].set_title(f"group met {100 * sec1['group']['deadline_met']:.1f} % (independent: {100 * sec1['group']['independence_prediction']:.1f} %)", fontsize=9, loc="left")
    t = [(cap[k] - T) / 1e9 for k in E]
    ax[1].plot([x for x, y in zip(t, glat) if y != INF], [y for y in glat if y != INF], color=INK, lw=0.5, alpha=0.6, label="group latency per frame")
    ax[1].axhline(D, color=INK2, ls="--", lw=1)
    ms = [(tt, y) for tt, y in zip(t, glat) if D < y != INF]
    if ms: ax[1].scatter(*zip(*ms), s=10, color=PALETTE["cam7"], zorder=3, label=f"late ({len(ms)})")
    mi = [tt for tt, y in zip(t, glat) if y == INF]
    if mi: ax[1].scatter(mi, [D] * len(mi), marker="x", s=14, color=INK, zorder=3, label=f"never arrived ({len(mi)})")
    ax[1].set_xlabel("time since T (s)"); ax[1].set_ylabel("group latency (ms)"); ax[1].legend(loc="upper left")
    ax2 = ax[1].twinx(); ax2.grid(False)
    bs = sorted(miss_bins); ax2.step([10 * b + 5 for b in bs], [100 * miss_bins[b][0] / miss_bins[b][1] for b in bs], where="mid", color=INK2, lw=1)
    ax2.set_ylabel("miss rate per 10 s (%)", color=INK2); ax2.spines["right"].set_visible(True)
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_1_group_latency.png", dpi=130); plt.close(fig)

    if len(cams) == 1:   # a single-camera task: only the deadline view applies
        txt.append("  (single-member group: items 2-8 do not apply)"); write(out, gid, res, txt); return

    # ---------------------------------------------------------------- 2. intra-group spread and waiting
    full = [k for k in E if all(F(c, k)["app"] for c in cams)]            # all members delivered
    arr = {k: {c: F(c, k)["app"] for c in cams} for k in full}
    gc = {k: max(arr[k].values()) for k in full}
    spread = [(gc[k] - min(arr[k].values())) / 1e6 for k in full]
    wait = [sum(gc[k] - v for v in arr[k].values()) / 1e6 for k in full]
    gl_full = [(gc[k] - cap[k]) / 1e6 for k in full]
    corr = st.correlation(spread, gl_full) if len(full) > 2 else float("nan")
    res["2_spread"] = {"frames": len(full), "spread": dist(spread), "waiting_sum": dist(wait), "corr_spread_group_latency": round(corr, 3),
                       # waiting / total member time from capture to GROUP completion (each member is held until the group is complete)
                       "waiting_share_of_member_time_to_group_completion": round(sum(wait) / sum(len(cams) * (gc[k] - cap[k]) / 1e6 for k in full), 4),
                       "group_period_ms": round(period, 1), "spread_gt_one_period": round(sum(x > period for x in spread) / len(full), 4)}
    txt += ["", "## 2. intra-group arrival spread (last member - first member) and waiting (sum over members of the time spent waiting for the last)"]
    txt.append(f"  spread   {fmt(res['2_spread']['spread'])};  > 1 group frame period ({period:.1f} ms): {100 * res['2_spread']['spread_gt_one_period']:.2f} % of frames")
    txt.append(f"  waiting  {fmt(res['2_spread']['waiting_sum'])};  waiting / member time from capture to group completion: "
               f"{100 * res['2_spread']['waiting_share_of_member_time_to_group_completion']:.1f} %")
    txt.append(f"  correlation(spread, group latency) = {corr:.2f}")
    fig, ax = plt.subplots(1, 2, figsize=(11, 3.4))
    v = sorted(spread); ax[0].plot(v, [i / len(v) for i in range(len(v))], color=INK, lw=1.8)
    ax[0].axvline(period, color=INK2, ls=":", lw=1); ax[0].text(period, 0.05, " 1 frame period", color=INK2)
    ax[0].set_xscale("symlog", linthresh=1); ax[0].set_xlabel("arrival spread within the group (ms)"); ax[0].set_ylabel("CDF")
    ax[0].set_title("how far apart the members' frame k arrive", fontsize=9, loc="left")
    ax[1].scatter(spread, gl_full, s=3, color=INK, alpha=0.3); ax[1].axhline(D, color=INK2, ls="--", lw=1)
    ax[1].set_xlabel("spread (ms)"); ax[1].set_ylabel("group latency (ms)"); ax[1].set_title(f"spread vs group latency (r = {corr:.2f})", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_2_spread.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 3. frontier lag (gNB view)
    # f_i(t) = index (in E) of the last frame such that member i's frames E[0..f] all reached the gNB (marker) by t.
    # A frame that never completes blocks the frontier, as it would block the fusion.
    comp = {c: [F(c, k)["g1"] if F(c, k)["g1"] else INF for k in E] for c in cams}
    prefix = {}
    for c in cams:
        m = 0; pre = []
        for x in comp[c]: m = max(m, x); pre.append(m)
        prefix[c] = pre                                                   # non-decreasing -> frontier by bisect
    def frontier_idx(c, t): return bisect.bisect_right(prefix[c], t) - 1
    evt = sorted({x for c in cams for x in prefix[c] if x != INF})
    gap_t = [(w, max(frontier_idx(c, w) for c in cams) - min(frontier_idx(c, w) for c in cams)) for w in evt]
    tw = collections.Counter(); s2 = collections.defaultdict(int); smax = collections.defaultdict(int)
    for (w0, gp), (w1, _) in zip(gap_t, gap_t[1:]):
        tw[gp] += w1 - w0
        a = w0
        while a < w1:                                                     # split the interval at second boundaries
            sec = int((a - T) // 1_000_000_000); b = min(w1, T + (sec + 1) * 1_000_000_000)
            smax[sec] = max(smax[sec], gp)
            if gp >= 2: s2[sec] += b - a
            a = b
    total = sum(tw.values()) or 1
    episodes = []; start = None
    for w, gp in gap_t:
        if gp >= 2 and start is None: start = w
        if gp < 2 and start is not None: episodes.append((w - start) / 1e6); start = None
    res["3_frontier"] = {"time_share_by_gap_frames": {str(k): round(v / total, 4) for k, v in sorted(tw.items())},
                         "time_share_gap_ge_2": round(sum(v for k, v in tw.items() if k >= 2) / total, 4),
                         "episodes_gap_ge_2": dist(episodes), "max_gap": max(tw) if tw else 0}
    txt += ["", "## 3. frontier lag at the gNB: gap = max_i f_i(t) - min_i f_i(t) in group frames; f_i = last frame received (marker) from member i with all earlier ones"]
    txt.append("  share of time by gap: " + ", ".join(f"{k}: {100 * v / total:.1f} %" for k, v in sorted(tw.items())))
    txt.append(f"  episodes with a member >= 2 frames behind: {len(episodes)}, duration {fmt(dist(episodes)) if episodes else 'n=0'} ms")
    fig, ax = plt.subplots(1, 2, figsize=(12, 3.4), gridspec_kw={"width_ratios": [1.8, 1]})
    secs = sorted(smax)
    ax[0].bar(secs, [smax[x] for x in secs], width=1.0, color=GRID, edgecolor=INK2, lw=0.3, label="max gap in the second")
    ax[0].set_xlabel("time since T (s)"); ax[0].set_ylabel("max frontier gap (frames)")
    a2 = ax[0].twinx(); a2.grid(False); a2.spines["right"].set_visible(True)
    a2.plot(secs, [100 * s2[x] / 1e9 for x in secs], color=INK, lw=1, label=">= 2 frames behind (% of the second)")
    a2.set_ylabel("% of time >= 2 frames behind")
    ax[0].set_title("how far the slowest member is behind (gNB view), per second", fontsize=9, loc="left")
    h1, l1 = ax[0].get_legend_handles_labels(); h2, l2 = a2.get_legend_handles_labels(); ax[0].legend(h1 + h2, l1 + l2, loc="upper left", fontsize=8)
    if episodes:
        v = sorted(episodes); ax[1].plot(v, [1 - i / len(v) for i in range(len(v))], color=INK, lw=1.8)
        ax[1].set_xscale("log"); ax[1].set_yscale("log")
    ax[1].set_xlabel("episode duration with gap >= 2 frames (ms)"); ax[1].set_ylabel("CCDF"); ax[1].set_title(f"{len(episodes)} episodes", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_3_frontier.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 4. PRBs to members ahead of the frontier (HEURISTIC proxy)
    # At each UL grant DECISION (wall - k2 slots) to member u: member j counts as "behind with data" when its gNB frontier is
    # the group minimum, strictly below u's, and the laptop has started sending j's next frame (first sendto <= t). The
    # laptop sendto is a proxy: it does not prove the bytes are queued in the phone (tethering delay, or already in the air).
    sent0 = {c: [F(c, k)["sent0"] or INF for k in E] for c in cams}
    rnti2cam = {C[c]["rnti"]: c for c in cams}
    lo, hi = cap[E[0]], cap[E[-1]] + int(period * 1e6)
    cls = collections.Counter(); per_s = collections.defaultdict(collections.Counter)
    for r in sched:
        w = r["dec_ns"]; c = rnti2cam.get(r["rnti"])
        if c is None or not lo <= w < hi: continue
        prb = int(r["rb_count"]); fi = {x: frontier_idx(x, w) for x in cams}; Fmin = min(fi.values())
        behind = [j for j in cams if j != c and fi[j] == Fmin and fi[j] < fi[c] and fi[j] + 1 < len(E) and sent0[j][fi[j] + 1] <= w]
        kind = "ahead_while_behind" if behind else ("frontier" if fi[c] == Fmin else "ahead_no_pending")
        cls[kind] += prb; per_s[int((w - T) / 1e9)][kind] += prb
    tot = sum(cls.values()) or 1
    res["4_ahead_resources"] = {"prb_share": {k: round(v / tot, 4) for k, v in cls.items()}, "prb_total": tot,
                                "note": "proxy: 'behind with data' = laptop started sending the member's next frame; decision time = wall - k2 slots"}
    txt += ["", "## 4. (proxy) UL PRBs granted to a member AHEAD of the group frontier while another member was behind and its next frame had been sent by its laptop"]
    txt.append("  " + ", ".join(f"{k}: {100 * v / tot:.1f} %" for k, v in cls.most_common()) + f"  (of {tot} PRBs, decisions at wall - k2 slots)")
    txt.append("  not a re-allocation bound: whether those bytes were in the phone's queue, and HARQ constraints, are not observed")
    fig, ax = plt.subplots(figsize=(12, 3.2))
    secs = sorted(per_s); share = [100 * per_s[s]["ahead_while_behind"] / max(1, sum(per_s[s].values())) for s in secs]
    ax.bar(secs, share, width=1.0, color=INK2); ax.set_xlabel("time since T (s)"); ax.set_ylabel("% of UL PRBs")
    ax.set_title(f"(proxy) PRBs granted ahead of the group frontier while a member was behind with data sent: {100 * cls['ahead_while_behind'] / tot:.1f} % overall", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_4_ahead_prbs.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 5. straggler identity, persistence, associated RAN events (HEURISTIC)
    strag = {}
    for k in full:
        sp = (gc[k] - min(arr[k].values())) / 1e6
        if sp > SPREAD_MEANINGFUL_MS: strag[k] = max(cams, key=lambda c: arr[k][c])
    share = collections.Counter(strag.values()); n_s = len(strag)
    runs = collections.defaultdict(list); seq = [strag[k] for k in sorted(strag)]   # runs on the sequence of qualifying frames
    for who, grp in itertools.groupby(seq): runs[who].append(len(list(grp)))
    def idx(rs, key, tkey):
        d = collections.defaultdict(list)
        for r in rs: d[key(r)].append(r)
        for v in d.values(): v.sort(key=tkey)
        return {k: (v, [tkey(r) for r in v]) for k, v in d.items()}
    SU = idx(sched, lambda r: r["rnti"], lambda r: r["dec_ns"])
    RL = idx((r for r in rows(f"{rd}/gnb/gnb_rlc_ul.csv") if r["event"] == "reassembly_expired"), lambda r: r["ue_index"], lambda r: int(r["wall_ns"]))
    def win(I, key, a, b):
        v, ts = I.get(key, ([], [])); return v[bisect.bisect_left(ts, a):bisect.bisect_right(ts, b)]
    def feats(c, k):   # events inside this member frame's own transport window [first sendto, marker at gNB]
        f = F(c, k); a, b = f["sent0"], f["g1"]
        if a is None or b is None: return None
        g = win(SU, C[c]["rnti"], a, b)
        first_grant = g[0]["dec_ns"] if g else b
        return dict(access=(first_grant - a) / 1e6, drain=(b - first_grant) / 1e6, retx=sum(r["new_data"] == "0" for r in g),
                    rlc=len(win(RL, C[c]["ue"], a, b)), idr=f["idr"])
    assoc = collections.Counter(); assoc_big = collections.Counter()
    for k, s in strag.items():
        fs = feats(s, k); fo = [x for x in (feats(c, k) for c in cams if c != s) if x]
        if not fs or not fo: assoc["(no gNB window)"] += 1; continue
        if fs["rlc"] > max(x["rlc"] for x in fo): why = "RLC reassembly expiry in its window"
        elif fs["retx"] > max(x["retx"] for x in fo): why = "more HARQ retx grants in its window"
        elif fs["idr"] and not any(F(c, k)["idr"] for c in cams if c != s): why = "its frame is an IDR"
        elif fs["access"] - st.mean(x["access"] for x in fo) >= fs["drain"] - st.mean(x["drain"] for x in fo): why = "longer sendto -> first grant"
        else: why = "longer first grant -> marker"
        assoc[why] += 1
        if (gc[k] - min(arr[k].values())) / 1e6 > period: assoc_big[why] += 1
    p = {c: share[c] / n_s for c in cams} if n_s else {}
    res["5_straggler"] = {"meaningful_threshold_ms": SPREAD_MEANINGFUL_MS, "frames_with_straggler": n_s,
                          "share": {c: round(p[c], 4) for c in cams} if n_s else {},
                          "mean_run_length": {c: round(st.mean(runs[c]), 2) if runs[c] else None for c in cams},
                          "iid_expected_run_length": {c: round(1 / (1 - p[c]), 2) if n_s and p[c] < 1 else None for c in cams},
                          "heuristic_association": dict(assoc), "heuristic_association_spread_gt_period": dict(assoc_big)}
    txt += ["", f"## 5. stragglers (frames whose last member arrives > {SPREAD_MEANINGFUL_MS:.0f} ms after the first: {n_s} of {len(full)})"]
    for c in cams:
        txt.append(f"  {c}: straggler in {100 * p.get(c, 0):.1f} %  mean run {res['5_straggler']['mean_run_length'][c]} qualifying frames "
                   f"(i.i.d. expectation {res['5_straggler']['iid_expected_run_length'][c]})")
    txt.append("  HEURISTIC association with RAN events in the straggler frame's own window [first sendto, marker at gNB] "
               "(first match of: RLC expiry > HARQ retx > IDR > access vs drain); not a causal attribution:")
    txt.append("    all: " + ", ".join(f"{k} {100 * v / max(1, n_s):.1f} %" for k, v in assoc.most_common()))
    nb = sum(assoc_big.values())
    txt.append(f"    spread > 1 period ({period:.0f} ms, n={nb}): " + ", ".join(f"{k} {100 * v / max(1, nb):.1f} %" for k, v in assoc_big.most_common()))
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.4), gridspec_kw={"width_ratios": [0.7, 1, 1.6]})
    ax[0].bar(cams, [100 * p.get(c, 0) for c in cams], color=[PALETTE[c] for c in cams]); ax[0].set_ylabel("% of straggler frames")
    ax[0].set_title("who is last", fontsize=9, loc="left")
    for c in cams:
        if runs[c]:
            h = collections.Counter(runs[c]); xs = sorted(h); ax[1].plot(xs, [h[x] for x in xs], marker="o", ms=3, color=PALETTE[c], label=c)
    ax[1].set_yscale("log"); ax[1].set_xlabel("consecutive qualifying frames with the same straggler"); ax[1].set_ylabel("count"); ax[1].legend()
    ax[1].set_title("persistence", fontsize=9, loc="left")
    labels = [k for k, _ in assoc.most_common()]
    ax[2].barh(labels, [100 * assoc[k] / max(1, n_s) for k in labels], color=INK2, label="all")
    ax[2].barh(labels, [100 * assoc_big[k] / max(1, nb) for k in labels], color="none", edgecolor=INK, hatch="//", label="spread > 1 period")
    ax[2].invert_yaxis(); ax[2].set_xlabel("% of straggler frames"); ax[2].legend(loc="lower right")
    ax[2].set_title("heuristic association with RAN events (not causal)", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_5_straggler.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 6. can the RAN identify the straggler from what it observes?
    BS = collections.defaultdict(list)
    for r in rows(f"{rd}/gnb/gnb_bsr.csv"):
        if r["rnti"] in rnti2cam: BS[r["rnti"]].append((int(r["wall_ns"]), r["lcg_id"], int(r["buffer_bytes"])))
    for v in BS.values(): v.sort()
    BSt = {k: [w for w, _, _ in v] for k, v in BS.items()}
    def bsr_total(c, t):   # latest report of every LCG up to t, summed (one BSR row per LCG)
        v = BS.get(C[c]["rnti"], []); i = bisect.bisect_right(BSt.get(C[c]["rnti"], []), t)
        last = {}
        for w, lcg, b in reversed(v[max(0, i - 64):i]): last.setdefault(lcg, b)
        return sum(last.values())
    def received(c, k, t): return sum(max(0, b - RTP_IP_UDP_OVERHEAD) for w, b, _ in F(c, k)["gpk"] if w <= t)
    gnb_strag = [k for k in strag if all(F(c, k)["g1"] and F(c, k)["g0"] for c in cams)]
    done_list = sorted((max(F(c, k)["g1"] for c in cams), max(cams, key=lambda c: F(c, k)["g1"])) for k in gnb_strag)
    done_t = [x for x, _ in done_list]
    names = ("BSR, all LCGs (stock)", "PDCP frontier (lowest)", "arrival order (no frame size)", "progress vs descriptor size", "last completed straggler")
    pred = {nm: collections.defaultdict(float) for nm in names}; lead = collections.defaultdict(list); cnt = collections.Counter()
    for k in gnb_strag:
        g1 = {c: F(c, k)["g1"] for c in cams}; truth = max(cams, key=g1.get); t_first = min(F(c, k)["g0"] for c in cams)
        for off in DECISION_OFFSETS_MS:
            t = t_first + off * 1_000_000
            if t >= max(g1.values()): continue                                # group already complete at the gNB
            cnt[off] += 1; lead[off].append((max(g1.values()) - t) / 1e6)
            j = bisect.bisect_right(done_t, t) - 1; last_s = done_list[j][1] if j >= 0 else None   # only outcomes known by t
            choices = {
                names[0]: {c: bsr_total(c, t) for c in cams},
                names[1]: {c: -frontier_idx(c, t) for c in cams},
                names[2]: {c: (F(c, k)["g0"] if F(c, k)["g0"] <= t else INF) for c in cams},
                names[3]: {c: -received(c, k, t) / max(1, F(c, k)["bytes"]) for c in cams},
                names[4]: {c: (1 if c == last_s else 0) for c in cams},
            }
            for nm, sc in choices.items():
                best = max(sc.values()); top = [c for c, v in sc.items() if v == best]
                pred[nm][off] += (1 / len(top)) if truth in top else 0.0    # ties = random pick
    res["6_identify"] = {"baseline_random": round(1 / len(cams), 3),
                         "accuracy": {nm: {str(o): round(v[o] / cnt[o], 3) for o in DECISION_OFFSETS_MS if cnt[o]} for nm, v in pred.items()},
                         "decisions": {str(o): cnt[o] for o in DECISION_OFFSETS_MS},
                         "lead_time_ms_median": {str(o): round(st.median(lead[o]), 1) for o in DECISION_OFFSETS_MS if lead[o]}}
    txt += ["", "## 6. can the gNB tell, while the frame is in flight, which member will be last at the gNB? (accuracy; random = "
            f"{100 / len(cams):.0f} %)  decision at t = first member's frame-k data at the gNB + offset; predictors use only data observable by t"]
    txt.append("  " + "  ".join(f"+{o} ms: n={cnt[o]}, remaining time to group completion median {res['6_identify']['lead_time_ms_median'].get(str(o), float('nan'))} ms" for o in DECISION_OFFSETS_MS))
    for nm, v in res["6_identify"]["accuracy"].items():
        txt.append(f"  {nm:32s} " + "  ".join(f"+{o} ms {100 * v[str(o)]:.1f} %" for o in DECISION_OFFSETS_MS if str(o) in v))
    fig, ax = plt.subplots(figsize=(10.5, 3.4)); wbar = 0.8 / len(pred)
    for i, (nm, v) in enumerate(pred.items()):
        ax.bar([j + i * wbar for j in range(len(DECISION_OFFSETS_MS))], [100 * v[o] / max(1, cnt[o]) for o in DECISION_OFFSETS_MS], width=wbar, color=SERIES[i], label=nm)
    ax.axhline(100 / len(cams), color=INK2, ls="--", lw=1); ax.text(-0.3, 100 / len(cams) + 1, "random", color=INK2)
    ax.set_xticks([j + 0.4 - wbar / 2 for j in range(len(DECISION_OFFSETS_MS))]); ax.set_xticklabels([f"+{o} ms" for o in DECISION_OFFSETS_MS])
    ax.set_ylabel("straggler identified (%)"); ax.set_ylim(0, 105); ax.legend(fontsize=8, loc="upper left", bbox_to_anchor=(1.01, 1.0))
    ax.set_title("predicting the straggler from gNB observations (decision time after the first member's data)", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_6_identify.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 8. IDR placement within the group
    # The group frame period realigns with the SR period (20 ms) and the UL-slot grid (2.5 ms) every PHASE_FRAMES frames,
    # so latency depends on j mod PHASE_FRAMES (j = group frame index). An IDR is compared with non-IDR frames of the same
    # phase; the result is a residual against that median (descriptive), not an estimated causal IDR penalty.
    step = BASE_FPS // fps_g
    gi = {k: k // step for k in E}                                       # group frame index
    pos = {gi[k]: i for i, k in enumerate(E)}
    idr_frames = {k for k in E if any(F(c, k)["idr"] for c in cams)}
    phase_lat = collections.defaultdict(list)
    for i, k in enumerate(E):
        if k not in idr_frames and glat[i] != INF: phase_lat[gi[k] % PHASE_FRAMES].append(glat[i])
    phase_med = {ph: st.median(v) for ph, v in phase_lat.items()}
    resid = {c: collections.defaultdict(list) for c in cams}
    for c in cams:
        for k in E:
            if F(c, k)["idr"]:
                for d in IDR_OFFSETS:
                    j = gi[k] + d
                    if j in pos and glat[pos[j]] != INF and j % PHASE_FRAMES in phase_med: resid[c][d].append(glat[pos[j]] - phase_med[j % PHASE_FRAMES])
    with_idr = [glat[pos[gi[k]]] - phase_med[gi[k] % PHASE_FRAMES] for k in idr_frames if glat[pos[gi[k]]] != INF]
    miss_idr = sum(glat[pos[gi[k]]] > D for k in idr_frames) / max(1, len(idr_frames))
    miss_phase = {ph: sum(x > D for x in v) / len(v) for ph, v in phase_lat.items()}
    ik = sorted(gi[k] for k in E if F(cams[0], k)["idr"]); gop = st.mode([b - a for a, b in zip(ik, ik[1:])]) if len(ik) > 1 else 2 * fps_g
    res["8_idr"] = {"phase_frames": PHASE_FRAMES, "phase_cycle_ms": round(PHASE_FRAMES * period, 1),
                    "group_latency_median_by_phase": {str(ph): round(m, 1) for ph, m in sorted(phase_med.items())},
                    "group_miss_rate_by_phase": {str(ph): round(v, 4) for ph, v in sorted(miss_phase.items())},
                    "idr_group_frames": len(idr_frames), "idr_frames_per_gop": round(len(idr_frames) / max(1, len(E) / gop), 2),
                    "idr_residual_vs_same_phase_ms": dist(with_idr), "idr_group_miss_rate": round(miss_idr, 4),
                    "residual_median_by_offset": {c: {str(d): round(st.median(v), 1) for d, v in sorted(resid[c].items())} for c in cams}}
    txt += ["", "## 8. IDR placement inside the group (descriptive: residual of group latency vs the same-phase non-IDR median)"]
    txt.append(f"  phase (group frame index mod {PHASE_FRAMES}, grids realign every {PHASE_FRAMES * period:.0f} ms), non-IDR group frames: median "
               + ", ".join(f"phase {ph}: {m:.1f} ms (miss {100 * miss_phase[ph]:.2f} %)" for ph, m in sorted(phase_med.items())))
    txt.append(f"  group frames containing a member IDR: {len(idr_frames)} ({res['8_idr']['idr_frames_per_gop']} per GOP), residual "
               f"{fmt(res['8_idr']['idr_residual_vs_same_phase_ms'])}; their miss rate {100 * miss_idr:.2f} %")
    for c in cams:
        m = res["8_idr"]["residual_median_by_offset"][c]
        txt.append(f"  around {c}'s IDR, residual (ms, median) by group-frame offset: " + " ".join(f"{d}:{m[d]}" for d in m))
    fig, ax = plt.subplots(1, 2, figsize=(12, 3.4), gridspec_kw={"width_ratios": [0.8, 1.4]})
    phs = sorted(phase_med); ax[0].bar([f"j mod {PHASE_FRAMES} = {ph}" for ph in phs], [phase_med[ph] for ph in phs], color=INK2)
    ax[0].set_ylabel("group latency median (ms)"); ax[0].set_title(f"frame phase vs SR / UL-slot grid ({PHASE_FRAMES * period:.0f} ms cycle, non-IDR)", fontsize=9, loc="left")
    for c in cams:
        ds = sorted(resid[c])
        if ds: ax[1].plot(ds, [st.median(resid[c][d]) for d in ds], marker="o", ms=3, color=PALETTE[c], label=f"around {c}'s IDR")
    ax[1].axhline(0, color=INK2, ls=":", lw=1); ax[1].axvline(0, color=INK2, lw=0.6)
    ax[1].set_xlabel("group-frame offset from the member's IDR"); ax[1].set_ylabel("residual (ms)\nvs same-phase non-IDR median"); ax[1].legend(fontsize=8)
    ax[1].set_title("group latency around a member's IDR (descriptive)", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_8_idr.png", dpi=130); plt.close(fig)
    write(out, gid, res, txt)


if __name__ == "__main__":
    main()
