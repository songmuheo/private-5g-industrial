#!/usr/bin/env python3
"""Multi-camera fusion analysis of one run (docs/SCENARIO_EDGE_PROFILES.md §5.6, items 1-6 and 8).

  .venv/bin/python analysis/fusion_report.py results/<run> [--group g1=cam0,cam1:100] [--warmup-s 5]

A fusion group G is a set of cameras whose frames with the SAME frame number are consumed by one inference; the
inference for frame k can start only when every member's frame k has arrived, before the group deadline D (ms after
capture). The group comes from the run's scenario.json `"groups": [{"id", "cams", "deadline_ms"}]` (the edge's
descriptor; later carried by the profile) or from --group (overrides). Frame number = grid_slot: in the ffmpeg
tree every camera captures slot k at the same instant and sends content frame k (src_frame_idx == grid_slot with
--content-origin), so any camera subset can be evaluated as a group offline.

Time points per camera i and frame k (all wall clock, chrony across hosts):
  capture   tx-frames.capture_wall_ns                 (laptop)
  sent      last tx-rtp row of the frame              (laptop, just before sendto)
  gNB first/last   first / marker packet of the frame in gnb_pdcp_ul (RAN-side completion: what the gNB observes)
  app       rx-frames.recv_wall_ns                    (receiver, decoded)
Items 1-2 use the app time (end-to-end, what the inference waits for); items 3, 4, 6 use the gNB time (what the RAN
can observe and act on). Outputs (derived, CLAUDE.md rule 3): <run>/analysis/fusion_<gid>.txt, fusion_<gid>.json,
graphs/fusion_<gid>_*.png.
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
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]          # non-camera series (predictors, causes), same palette order
plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
                     "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "legend.frameon": False})

SPREAD_MEANINGFUL_MS = 5.0      # a straggler "counts" when it arrives this much after the first member (item 5, 6)
DECISION_OFFSETS_MS = (0, 5, 10)  # item 6: RAN decision instants after the first member's frame-k data reaches the gNB
IDR_OFFSETS = range(-3, 11)     # item 8: frames around a member's IDR
PHASE_FRAMES = 3                # 30 fps vs 20 ms SR period vs 2.5 ms UL slot: the grids realign every 100 ms = 3 frames


def rows(path):
    with open(path) as f:
        yield from csv.DictReader(l for l in f if not l.startswith("#"))


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p * (len(v) - 1)))))] if v else float("nan")


def dist(v):
    return {"n": len(v), "median": round(st.median(v), 2) if v else None, "mean": round(st.mean(v), 2) if v else None,
            "p90": round(pct(v, .9), 2), "p99": round(pct(v, .99), 2), "max": round(max(v), 2) if v else None}


def fmt(d):
    return f"median {d['median']:.1f}  p90 {d['p90']:.1f}  p99 {d['p99']:.1f}  max {d['max']:.1f}  (n={d['n']})" if d["n"] else "n=0"


# ------------------------------------------------------------------------------------------------ loading
def load(rd, cams, warmup_s):
    exp = json.load(open(f"{rd}/experiment.json"))
    T = int(round(exp["start_time_epoch"] * 1e9))
    dur = int(exp["scenario"].get("duration_s", 300))
    C = {}
    for c in cams:
        a = f"{rd}/senders/{c}/app"
        enc = {r["rtp_ts"]: r for r in rows(f"{a}/{c}-tx-encoded.csv")}
        sent = collections.defaultdict(int); ssrc = None
        for r in rows(f"{a}/{c}-tx-rtp.csv"):
            ssrc = ssrc or r["ssrc"]; w = int(r["log_wall_ns"])
            if w > sent[r["rtp_ts"]]: sent[r["rtp_ts"]] = w
        fr = {}
        for r in rows(f"{a}/{c}-tx-frames.csv"):
            if r["to_encoder"] != "1": continue
            ts = r["rtp_ts"]; e = enc.get(ts)
            fr[int(r["grid_slot"])] = dict(ts=ts, cap=int(r["capture_wall_ns"]), src=int(r["src_frame_idx"]), sent=sent.get(ts),
                                           idr=(e is not None and e["is_idr"] == "1"), bytes=int(e["bytes"]) if e else 0)
        app = {r["rtp_ts"]: int(r["recv_wall_ns"]) for r in rows(f"{rd}/app/{c}-rx-frames.csv") if r["ssrc"] == ssrc}
        C[c] = dict(ssrc=ssrc, frames=fr, app=app)
    by_ssrc = {v["ssrc"]: c for c, v in C.items()}
    pk = {c: collections.defaultdict(list) for c in cams}   # (wall, sdu_bytes) per rtp_ts, in arrival order
    ue = {}
    for r in rows(f"{rd}/gnb/gnb_pdcp_ul.csv"):
        if r["rtp_like"] != "1": continue
        c = by_ssrc.get(r["rtp_ssrc"])
        if c is None: continue
        ue[c] = r["ue_index"]
        pk[c][r["rtp_ts"]].append((int(r["wall_ns"]), int(r["sdu_bytes"])))
    sched = list(rows(f"{rd}/gnb/gnb_sched_ul.csv"))
    rnti = {}
    for r in sched: rnti.setdefault(r["ue_index"], r["rnti"])
    for c in cams:
        if c not in ue: sys.exit(f"{c}: no RTP rows at the gNB PDCP for ssrc {C[c]['ssrc']} (not an OTA run?)")
        C[c]["ue"] = ue[c]; C[c]["rnti"] = rnti.get(ue[c])
        for k, f in C[c]["frames"].items():
            p = pk[c].get(f["ts"])
            f["g0"] = p[0][0] if p else None; f["g1"] = max(w for w, _ in p) if p else None; f["gpk"] = p or []
            f["app"] = C[c]["app"].get(f["ts"])
    lo = T + int(warmup_s * 1e9); hi = T + dur * 1_000_000_000
    common = sorted(set.intersection(*(set(C[c]["frames"]) for c in cams)))
    K = [k for k in common if lo <= min(C[c]["frames"][k]["cap"] for c in cams) < hi
         and all(C[c]["frames"][k]["app"] and C[c]["frames"][k]["g1"] for c in cams)]
    return T, dur, C, K, sched


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


def analyse_group(rd, g, warmup_s, out, gdir):
    gid, cams, D = g["id"], list(g["cams"]), float(g["deadline_ms"])
    T, dur, C, K, sched = load(rd, cams, warmup_s)
    F = lambda c, k: C[c]["frames"][k]
    res = {"group": gid, "cams": cams, "deadline_ms": D, "warmup_s": warmup_s, "frames": len(K)}
    txt = [f"# fusion analysis {rd}  group {gid} = {'+'.join(cams)}  deadline {D:.0f} ms  (frames k with capture >= T+{warmup_s:g} s: {len(K)})",
           f"# frame number = grid_slot; content frames identical across members: "
           f"{all(len({F(c, k)['src'] for c in cams}) == 1 for k in K)}"]
    cap = {k: min(F(c, k)["cap"] for c in cams) for k in K}

    # ---------------------------------------------------------------- 1. group latency and deadline satisfaction
    lat = {c: [(F(c, k)["app"] - cap[k]) / 1e6 for k in K] for c in cams}
    glat = [max(lat[c][i] for c in cams) for i in range(len(K))]
    sec1 = {"per_camera": {c: dict(dist(lat[c]), deadline_met=round(sum(x <= D for x in lat[c]) / len(K), 4)) for c in cams},
            "group": dict(dist(glat), deadline_met=round(sum(x <= D for x in glat) / len(K), 4))}
    sec1["group"]["independence_prediction"] = round(math.prod(sec1["per_camera"][c]["deadline_met"] for c in cams), 4)
    subsets = {}
    for n in range(2, len(cams) + 1):
        ok = []
        for S in itertools.combinations(cams, n):
            ok.append(sum(max(lat[c][i] for c in S) <= D for i in range(len(K))) / len(K))
        subsets[n] = {"subsets": len(ok), "deadline_met_mean": round(st.mean(ok), 4), "worst": round(min(ok), 4)}
    sec1["by_group_size"] = subsets
    gl_g = [max(F(c, k)["g1"] for c in cams) - cap[k] for k in K]
    sec1["group_at_gnb"] = dist([x / 1e6 for x in gl_g])
    miss_bins = collections.defaultdict(lambda: [0, 0])
    for i, k in enumerate(K):
        b = int((cap[k] - T) / 10e9); miss_bins[b][1] += 1; miss_bins[b][0] += glat[i] > D
    res["1_group_latency"] = sec1
    txt += ["", f"## 1. group latency (capture -> last member decoded) and deadline D = {D:.0f} ms"]
    for c in cams: txt.append(f"  {c:6s} {fmt(sec1['per_camera'][c])}  met {100 * sec1['per_camera'][c]['deadline_met']:.2f} %")
    txt.append(f"  GROUP  {fmt(sec1['group'])}  met {100 * sec1['group']['deadline_met']:.2f} %  "
               f"(if members were independent: {100 * sec1['group']['independence_prediction']:.2f} %)")
    txt.append(f"  group completion at the gNB (last member's last packet): {fmt(sec1['group_at_gnb'])}")
    for n, v in subsets.items(): txt.append(f"  |G| = {n}: {v['subsets']} subsets, deadline met mean {100 * v['deadline_met_mean']:.2f} %, worst {100 * v['worst']:.2f} %")
    worst_bins = sorted(((v[0] / v[1], b) for b, v in miss_bins.items() if v[1]), reverse=True)[:3]
    txt.append("  worst 10 s bins (miss rate): " + ", ".join(f"t={10 * b}-{10 * b + 10} s {100 * r:.1f} %" for r, b in worst_bins))

    fig, ax = plt.subplots(1, 2, figsize=(12, 3.8), gridspec_kw={"width_ratios": [1, 1.6]})
    for c in cams:
        v = sorted(lat[c]); ax[0].plot(v, [i / len(v) for i in range(len(v))], color=PALETTE[c], lw=1.6, label=c)
    v = sorted(glat); ax[0].plot(v, [i / len(v) for i in range(len(v))], color=INK, lw=2, label=f"group {gid} (max)")
    ax[0].axvline(D, color=INK2, ls="--", lw=1); ax[0].text(D, 0.5, f" D={D:.0f} ms", color=INK2)
    ax[0].set_xscale("log"); ax[0].set_xlabel("capture -> decoded (ms, log)"); ax[0].set_ylabel("CDF"); ax[0].legend(loc="upper left")
    ax[0].set_title(f"latency CDF: group met {100 * sec1['group']['deadline_met']:.1f} % (independent: {100 * sec1['group']['independence_prediction']:.1f} %)", fontsize=9, loc="left")
    t = [(cap[k] - T) / 1e9 for k in K]
    ax[1].plot(t, glat, color=INK, lw=0.5, alpha=0.6, label="group latency per frame")
    ax[1].axhline(D, color=INK2, ls="--", lw=1)
    ms = [(tt, y) for tt, y in zip(t, glat) if y > D]
    if ms: ax[1].scatter(*zip(*ms), s=10, color=PALETTE["cam7"], zorder=3, label=f"misses ({len(ms)})")
    ax[1].set_xlabel("time since T (s)"); ax[1].set_ylabel("group latency (ms)"); ax[1].legend(loc="upper left")
    ax2 = ax[1].twinx(); ax2.grid(False)
    bs = sorted(miss_bins); ax2.step([10 * b + 5 for b in bs], [100 * miss_bins[b][0] / miss_bins[b][1] for b in bs], where="mid", color=INK2, lw=1)
    ax2.set_ylabel("miss rate per 10 s (%)", color=INK2); ax2.spines["right"].set_visible(True)
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_1_group_latency.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 2. intra-group spread and waiting
    arr = {k: {c: F(c, k)["app"] for c in cams} for k in K}
    spread = [(max(arr[k].values()) - min(arr[k].values())) / 1e6 for k in K]
    wait = [sum(max(arr[k].values()) - v for v in arr[k].values()) / 1e6 for k in K]
    corr = st.correlation(spread, glat) if len(K) > 2 else float("nan")
    res["2_spread"] = {"spread": dist(spread), "waiting_sum": dist(wait), "corr_spread_group_latency": round(corr, 3),
                       "waiting_share_of_member_time": round(sum(wait) / sum(sum((arr[k][c] - cap[k]) / 1e6 for c in cams) for k in K), 4),
                       "spread_gt_33ms": round(sum(x > 33.3 for x in spread) / len(K), 4)}
    txt += ["", "## 2. intra-group arrival spread (last member - first member) and waiting time (sum over members of time spent waiting for the last)"]
    txt.append(f"  spread   {fmt(res['2_spread']['spread'])};  > 1 frame period (33 ms): {100 * res['2_spread']['spread_gt_33ms']:.2f} % of frames")
    txt.append(f"  waiting  {fmt(res['2_spread']['waiting_sum'])};  share of all member latency spent waiting: {100 * res['2_spread']['waiting_share_of_member_time']:.1f} %")
    txt.append(f"  correlation(spread, group latency) = {corr:.2f}")
    fig, ax = plt.subplots(1, 2, figsize=(11, 3.4))
    v = sorted(spread); ax[0].plot(v, [i / len(v) for i in range(len(v))], color=INK, lw=1.8)
    ax[0].axvline(33.3, color=INK2, ls=":", lw=1); ax[0].text(33.3, 0.05, " 1 frame period", color=INK2)
    ax[0].set_xscale("symlog", linthresh=1); ax[0].set_xlabel("arrival spread within the group (ms)"); ax[0].set_ylabel("CDF")
    ax[0].set_title("how far apart the members' frame k arrive", fontsize=9, loc="left")
    ax[1].scatter(spread, glat, s=3, color=INK, alpha=0.3); ax[1].axhline(D, color=INK2, ls="--", lw=1)
    ax[1].set_xlabel("spread (ms)"); ax[1].set_ylabel("group latency (ms)"); ax[1].set_title(f"spread vs group latency (r = {corr:.2f})", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_2_spread.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 3. frontier lag (gNB view)
    # frontier f_i(t) = highest k such that every frame <= k of member i (within K) has fully reached the gNB
    ev = sorted((F(c, k)["g1"], c, k) for c in cams for k in K)
    pos = {c: 0 for c in cams}; done = {c: set() for c in cams}; Ks = K
    front = {c: Ks[0] - 1 for c in cams}
    gap_t = []          # (t, gap) after each event
    for w, c, k in ev:
        done[c].add(k)
        while pos[c] < len(Ks) and Ks[pos[c]] in done[c]: front[c] = Ks[pos[c]]; pos[c] += 1
        gap_t.append((w, max(front.values()) - min(front.values()), min(front, key=front.get)))
    tw = collections.Counter()
    for (w0, gp, _), (w1, _, _) in zip(gap_t, gap_t[1:]): tw[gp] += (w1 - w0)
    total = sum(tw.values())
    episodes = []; start = None
    for w, gp, _ in gap_t:
        if gp >= 2 and start is None: start = w
        if gp < 2 and start is not None: episodes.append((w - start) / 1e6); start = None
    res["3_frontier"] = {"time_share_by_gap_frames": {str(k): round(v / total, 4) for k, v in sorted(tw.items())},
                         "time_share_gap_ge_2": round(sum(v for k, v in tw.items() if k >= 2) / total, 4),
                         "episodes_gap_ge_2": dist(episodes), "max_gap": max(tw)}
    txt += ["", "## 3. frontier lag at the gNB: gap = max_i f_i(t) - min_i f_i(t) (frames), f_i = last frame fully received from member i"]
    txt.append("  share of time by gap: " + ", ".join(f"{k}: {100 * v / total:.1f} %" for k, v in sorted(tw.items())))
    txt.append(f"  episodes with a member >= 2 frames behind: {len(episodes)}, duration {fmt(dist(episodes)) if episodes else 'n=0'} ms")
    fig, ax = plt.subplots(1, 2, figsize=(12, 3.4), gridspec_kw={"width_ratios": [1.8, 1]})
    # per second: the largest gap reached, and the share of time a member was >= 2 frames behind
    smax = collections.defaultdict(int); s2 = collections.defaultdict(int)
    for (w0, gp, _), (w1, _, _) in zip(gap_t, gap_t[1:]):
        sec = int((w0 - T) / 1e9); smax[sec] = max(smax[sec], gp)
        if gp >= 2: s2[sec] += w1 - w0
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

    # ---------------------------------------------------------------- 4. resources spent ahead of the group frontier
    # At each UL grant to member u (scheduler decision time): member j is "behind with data" when its gNB frontier is the
    # group minimum, strictly below u's, and its next frame has already been handed to the UE (sent <= t). PRBs given to
    # u then serve frames the group cannot use yet -> the share a frontier-aware scheduler could re-assign (upper bound).
    ftimes = {c: [F(c, k)["g1"] for k in K] for c in cams}            # completion times in frame order
    fprefix = {c: [] for c in cams}
    for c in cams:
        m = 0
        for x in ftimes[c]: m = max(m, x); fprefix[c].append(m)       # prefix max -> frontier index via bisect
    sent_t = {c: [F(c, k)["sent"] or 0 for k in K] for c in cams}
    def frontier_idx(c, t): return bisect.bisect_right(fprefix[c], t) - 1   # index into K of the last fully received frame
    rnti2cam = {C[c]["rnti"]: c for c in cams}
    lo, hi = cap[K[0]], cap[K[-1]] + 200_000_000
    cls = collections.Counter(); per_s = collections.defaultdict(collections.Counter)
    for r in sched:
        w = int(r["wall_ns"]); c = rnti2cam.get(r["rnti"])
        if c is None or not lo <= w < hi: continue
        prb = int(r["rb_count"]); fi = {x: frontier_idx(x, w) for x in cams}; Fmin = min(fi.values())
        behind = [j for j in cams if j != c and fi[j] == Fmin and fi[j] < fi[c] and fi[j] + 1 < len(K) and 0 < sent_t[j][fi[j] + 1] <= w]
        kind = "ahead_while_behind" if behind else ("frontier" if fi[c] == Fmin else "ahead_no_pending")
        cls[kind] += prb; per_s[int((w - T) / 1e9)][kind] += prb
    tot = sum(cls.values())
    res["4_ahead_resources"] = {"prb_share": {k: round(v / tot, 4) for k, v in cls.items()},
                                "prb_total": tot}
    txt += ["", "## 4. UL PRBs given to a member that is AHEAD of the group frontier while another member is behind with data waiting"]
    txt.append("  " + ", ".join(f"{k}: {100 * v / tot:.1f} %" for k, v in cls.most_common()) + f"  (of {tot} PRBs)")
    txt.append("  ahead_while_behind = what a frontier-aware scheduler could re-assign to the straggler (upper bound: some of it is HARQ retx)")
    fig, ax = plt.subplots(figsize=(12, 3.2))
    secs = sorted(per_s); share = [100 * per_s[s]["ahead_while_behind"] / max(1, sum(per_s[s].values())) for s in secs]
    ax.bar(secs, share, width=1.0, color=INK2); ax.set_xlabel("time since T (s)"); ax.set_ylabel("% of UL PRBs")
    ax.set_title(f"PRBs spent ahead of the group frontier while a member was behind with data: {100 * cls['ahead_while_behind'] / tot:.1f} % overall", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_4_ahead_prbs.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 5. straggler identity, persistence, cause
    strag = {}
    for k in K:
        c_last = max(cams, key=lambda c: arr[k][c]); sp = (max(arr[k].values()) - min(arr[k].values())) / 1e6
        if sp > SPREAD_MEANINGFUL_MS: strag[k] = c_last
    share = collections.Counter(strag.values()); n_s = len(strag)
    runs = collections.defaultdict(list); prev = None; run = 0
    for k in sorted(strag):
        if prev is not None and strag[k] == strag.get(prev) and k == prev + 1: run += 1
        else:
            if prev is not None: runs[strag[prev]].append(run)
            run = 1
        prev = k
    if prev is not None: runs[strag[prev]].append(run)
    # causes (straggler vs the other members, same frame); RAN events from the gNB traces
    def idx(rs, key):
        d = collections.defaultdict(list)
        for r in rs: d[key(r)].append(r)
        for v in d.values(): v.sort(key=lambda r: int(r["wall_ns"]))
        return {k: (v, [int(r["wall_ns"]) for r in v]) for k, v in d.items()}
    SU = idx(sched, lambda r: r["rnti"])
    CR = idx(rows(f"{rd}/gnb/gnb_ul_crc.csv"), lambda r: r["rnti"])
    RL = idx((r for r in rows(f"{rd}/gnb/gnb_rlc_ul.csv") if r["event"] == "reassembly_expired"), lambda r: r["ue_index"])
    def win(I, key, a, b):
        v, ts = I.get(key, ([], [])); return v[bisect.bisect_left(ts, a):bisect.bisect_right(ts, b)]
    shared = collections.Counter((r["sfn"], r["slot"], int(r["wall_ns"]) // 1_000_000) for r in sched)
    def feats(c, k):
        f = F(c, k); a, b = f["sent"], f["g1"]; g = win(SU, C[c]["rnti"], a, b)
        return dict(ul_wait=(f["g0"] - a) / 1e6, drain=(f["g1"] - f["g0"]) / 1e6, retx=sum(r["new_data"] == "0" for r in g),
                    crc_fail=sum(r["crc_ok"] == "0" for r in win(CR, C[c]["rnti"], a, b + 6e6)),
                    rlc=len(win(RL, C[c]["ue"], a, b + 30e6)), idr=f["idr"],
                    shared=(sum(shared[(r["sfn"], r["slot"], int(r["wall_ns"]) // 1_000_000)] > 1 for r in g) / len(g)) if g else 0.0)
    cause = collections.Counter(); cause_big = collections.Counter()
    for k, s in strag.items():
        fs = feats(s, k); fo = [feats(c, k) for c in cams if c != s]
        o_retx = max(x["retx"] for x in fo); o_rlc = max(x["rlc"] for x in fo)
        o_wait = st.mean(x["ul_wait"] for x in fo); o_drain = st.mean(x["drain"] for x in fo)
        if fs["rlc"] > o_rlc: why = "RLC recovery (HARQ gave up)"
        elif fs["retx"] > o_retx: why = "HARQ retransmission"
        elif fs["idr"] and not any(F(c, k)["idr"] for c in cams if c != s): why = "IDR (larger frame)"
        elif fs["ul_wait"] - o_wait >= fs["drain"] - o_drain: why = "access wait (SR/BSR -> first grant)"
        else: why = "drain (fewer PRBs / shared slots)"
        cause[why] += 1
        if (max(arr[k].values()) - min(arr[k].values())) / 1e6 > 33.3: cause_big[why] += 1
    res["5_straggler"] = {"meaningful_threshold_ms": SPREAD_MEANINGFUL_MS, "frames_with_straggler": n_s,
                          "share": {c: round(share[c] / n_s, 4) for c in cams} if n_s else {},
                          "mean_run_length": {c: round(st.mean(runs[c]), 2) if runs[c] else None for c in cams},
                          "iid_expected_run_length": {c: round(1 / (1 - share[c] / n_s), 2) if n_s and share[c] < n_s else None for c in cams},
                          "cause": dict(cause), "cause_spread_gt_33ms": dict(cause_big)}
    txt += ["", f"## 5. stragglers (frames whose last member arrives > {SPREAD_MEANINGFUL_MS:.0f} ms after the first: {n_s} of {len(K)})"]
    for c in cams:
        txt.append(f"  {c}: straggler in {100 * share[c] / max(1, n_s):.1f} %  mean run {res['5_straggler']['mean_run_length'][c]} frames "
                   f"(i.i.d. expectation {res['5_straggler']['iid_expected_run_length'][c]})")
    txt.append("  cause (all stragglers): " + ", ".join(f"{k} {100 * v / max(1, n_s):.1f} %" for k, v in cause.most_common()))
    nb = sum(cause_big.values())
    txt.append(f"  cause (spread > 33 ms, n={nb}): " + ", ".join(f"{k} {100 * v / max(1, nb):.1f} %" for k, v in cause_big.most_common()))
    fig, ax = plt.subplots(1, 3, figsize=(13, 3.4), gridspec_kw={"width_ratios": [0.7, 1, 1.6]})
    ax[0].bar(cams, [100 * share[c] / max(1, n_s) for c in cams], color=[PALETTE[c] for c in cams]); ax[0].set_ylabel("% of straggler frames")
    ax[0].set_title("who is last", fontsize=9, loc="left")
    for c in cams:
        if runs[c]:
            h = collections.Counter(runs[c]); xs = sorted(h); ax[1].plot(xs, [h[x] for x in xs], marker="o", ms=3, color=PALETTE[c], label=c)
    ax[1].set_yscale("log"); ax[1].set_xlabel("consecutive frames with the same straggler"); ax[1].set_ylabel("count"); ax[1].legend()
    ax[1].set_title("persistence (long runs = structural)", fontsize=9, loc="left")
    labels = [k for k, _ in cause.most_common()]
    ax[2].barh(labels, [100 * cause[k] / max(1, n_s) for k in labels], color=INK2, label="all")
    ax[2].barh(labels, [100 * cause_big[k] / max(1, nb) for k in labels], color="none", edgecolor=INK, hatch="//", label="spread > 33 ms")
    ax[2].invert_yaxis(); ax[2].set_xlabel("% of straggler frames"); ax[2].legend(loc="lower right"); ax[2].set_title("why the straggler was late", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_5_straggler.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 6. can the RAN identify the straggler from what it observes?
    BS = idx((r for r in rows(f"{rd}/gnb/gnb_bsr.csv") if r["rnti"] in rnti2cam), lambda r: r["rnti"])
    def last_bsr(c, t):
        v, ts = BS.get(C[c]["rnti"], ([], [])); i = bisect.bisect_right(ts, t) - 1; return int(v[i]["buffer_bytes"]) if i >= 0 else 0
    def progress(c, k, t):    # share of frame k's bytes already at the gNB (needs the frame size: descriptor)
        f = F(c, k); tot_b = sum(b for _, b in f["gpk"]) or 1; return sum(b for w, b in f["gpk"] if w <= t) / tot_b
    pred = {name: collections.defaultdict(float) for name in ("BSR (stock: largest buffer)", "PDCP frontier (lowest)", "frame progress (RTP + size)", "last straggler (persistence)")}
    lead = collections.defaultdict(list); cnt = collections.Counter(); prev_s = None
    for k in sorted(strag):
        truth = max(cams, key=lambda c: F(c, k)["g1"])                       # RAN-side truth: last to complete at the gNB
        t_first = min(F(c, k)["g0"] for c in cams)
        for off in DECISION_OFFSETS_MS:
            t = t_first + off * 1_000_000
            if t >= max(F(c, k)["g1"] for c in cams): continue               # group already complete: nothing to decide
            cnt[off] += 1; lead[off].append((max(F(c, k)["g1"] for c in cams) - t) / 1e6)
            choices = {
                "BSR (stock: largest buffer)": {c: last_bsr(c, t) for c in cams},
                "PDCP frontier (lowest)": {c: -frontier_idx(c, t) for c in cams},
                "frame progress (RTP + size)": {c: -progress(c, k, t) for c in cams},
                "last straggler (persistence)": {c: (1 if c == prev_s else 0) for c in cams},
            }
            for name, sc in choices.items():
                best = max(sc.values()); top = [c for c, v in sc.items() if v == best]
                pred[name][off] += (1 / len(top)) if truth in top else 0.0    # ties = random pick
        prev_s = truth
    res["6_identify"] = {"baseline_random": round(1 / len(cams), 3),
                         "accuracy": {n: {str(o): round(v[o] / cnt[o], 3) for o in DECISION_OFFSETS_MS if cnt[o]} for n, v in pred.items()},
                         "decisions": {str(o): cnt[o] for o in DECISION_OFFSETS_MS},
                         "lead_time_ms_median": {str(o): round(st.median(lead[o]), 1) for o in DECISION_OFFSETS_MS if lead[o]}}
    txt += ["", "## 6. can the gNB tell, while the frame is still in flight, which member will be last? (accuracy; random = "
            f"{100 / len(cams):.0f} %)  decision at t = first member's frame-k data at the gNB + offset"]
    txt.append("  " + "  ".join(f"+{o} ms: n={cnt[o]}, remaining time to group completion median {res['6_identify']['lead_time_ms_median'].get(str(o), float('nan'))} ms" for o in DECISION_OFFSETS_MS))
    for n, v in res["6_identify"]["accuracy"].items():
        txt.append(f"  {n:32s} " + "  ".join(f"+{o} ms {100 * v[str(o)]:.1f} %" for o in DECISION_OFFSETS_MS if str(o) in v))
    fig, ax = plt.subplots(figsize=(10, 3.4)); wbar = 0.8 / len(pred)
    for i, (n, v) in enumerate(pred.items()):
        ax.bar([j + i * wbar for j in range(len(DECISION_OFFSETS_MS))], [100 * v[o] / max(1, cnt[o]) for o in DECISION_OFFSETS_MS], width=wbar, color=SERIES[i], label=n)
    ax.axhline(100 / len(cams), color=INK2, ls="--", lw=1); ax.text(-0.3, 100 / len(cams) + 1, "random", color=INK2)
    ax.set_xticks([j + 0.4 - wbar / 2 for j in range(len(DECISION_OFFSETS_MS))]); ax.set_xticklabels([f"+{o} ms" for o in DECISION_OFFSETS_MS])
    ax.set_ylabel("straggler identified (%)"); ax.set_ylim(0, 105); ax.legend(fontsize=8, loc="upper left", bbox_to_anchor=(1.01, 1.0))
    ax.set_title("predicting the straggler from gNB observations (decision time after the first member's data)", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_6_identify.png", dpi=130); plt.close(fig)

    # ---------------------------------------------------------------- 8. IDR placement within the group
    # Confounder: the frame period (33.3 ms) realigns with the SR period (20 ms) and the UL slot grid (2.5 ms) only every
    # 100 ms = PHASE_FRAMES frames, so latency depends on k mod PHASE_FRAMES. IDR effects are therefore measured against
    # non-IDR frames of the SAME phase (and the phase effect itself is reported).
    pos_k = {k: i for i, k in enumerate(K)}
    idr_of = {k: [c for c in cams if F(c, k)["idr"]] for k in K}
    idr_frames = {k for k, v in idr_of.items() if v}
    phase_lat = collections.defaultdict(list)
    for k in K:
        if k not in idr_frames: phase_lat[k % PHASE_FRAMES].append(glat[pos_k[k]])
    phase_med = {ph: st.median(v) for ph, v in phase_lat.items()}
    excess = {c: collections.defaultdict(list) for c in cams}           # group latency minus same-phase non-IDR median
    for c in cams:
        for k in K:
            if F(c, k)["idr"]:
                for d in IDR_OFFSETS:
                    if k + d in pos_k: excess[c][d].append(glat[pos_k[k + d]] - phase_med[(k + d) % PHASE_FRAMES])
    with_idr = [glat[pos_k[k]] - phase_med[k % PHASE_FRAMES] for k in idr_frames]
    miss_idr = sum(glat[pos_k[k]] > D for k in idr_frames) / max(1, len(idr_frames))
    miss_same_phase = {ph: sum(x > D for x in v) / len(v) for ph, v in phase_lat.items()}
    idr_per_gop = len(idr_frames) / max(1, len(K) / 60)
    res["8_idr"] = {"phase_frames": PHASE_FRAMES,
                    "group_latency_median_by_phase_k_mod": {str(ph): round(m, 1) for ph, m in sorted(phase_med.items())},
                    "group_miss_rate_by_phase": {str(ph): round(v, 4) for ph, v in sorted(miss_same_phase.items())},
                    "idr_frames_per_gop": round(idr_per_gop, 2), "idr_group_frames": len(idr_frames),
                    "idr_excess_over_same_phase_ms": dist(with_idr), "idr_group_miss_rate": round(miss_idr, 4),
                    "excess_median_by_offset": {c: {str(d): round(st.median(v), 1) for d, v in sorted(excess[c].items())} for c in cams}}
    txt += ["", "## 8. IDR placement inside the group (members' IDRs are staggered in this run)"]
    txt.append(f"  phase effect (k mod {PHASE_FRAMES}, 100 ms realignment of frame / SR / UL-slot grids), non-IDR group frames: median "
               + ", ".join(f"phase {ph}: {m:.1f} ms (miss {100 * miss_same_phase[ph]:.2f} %)" for ph, m in sorted(phase_med.items())))
    txt.append(f"  group frames containing a member IDR: {len(idr_frames)} ({idr_per_gop:.1f} per GOP), latency excess over same-phase "
               f"non-IDR frames {fmt(res['8_idr']['idr_excess_over_same_phase_ms'])}; miss rate {100 * miss_idr:.2f} %")
    for c in cams:
        m = res["8_idr"]["excess_median_by_offset"][c]
        txt.append(f"  around {c}'s IDR, group latency excess (ms, median) by frame offset: " + " ".join(f"{d}:{m[d]}" for d in m))
    txt.append("  staggered IDRs inside a group put an IDR into |G| group frames per GOP; aligned IDRs into 1 (but all members' IDR"
               " bursts in the same slot) - compare with the excess above")
    fig, ax = plt.subplots(1, 2, figsize=(12, 3.4), gridspec_kw={"width_ratios": [0.8, 1.4]})
    phs = sorted(phase_med); ax[0].bar([f"k mod {PHASE_FRAMES} = {ph}" for ph in phs], [phase_med[ph] for ph in phs], color=INK2)
    ax[0].set_ylabel("group latency median (ms)"); ax[0].set_title("frame phase vs SR / UL-slot grid (non-IDR frames)", fontsize=9, loc="left")
    for c in cams:
        ds = sorted(excess[c]); ax[1].plot(ds, [st.median(excess[c][d]) for d in ds], marker="o", ms=3, color=PALETTE[c], label=f"around {c}'s IDR")
    ax[1].axhline(0, color=INK2, ls=":", lw=1); ax[1].axvline(0, color=INK2, lw=0.6)
    ax[1].set_xlabel("frame offset from the member's IDR"); ax[1].set_ylabel("group latency excess (ms)\nvs same-phase non-IDR median"); ax[1].legend(fontsize=8)
    ax[1].set_title("does a member's IDR delay the group frame (and the frames after it)?", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{gdir}/fusion_{gid}_8_idr.png", dpi=130); plt.close(fig)

    with open(f"{out}/fusion_{gid}.json", "w") as f: json.dump(res, f, indent=2)
    with open(f"{out}/fusion_{gid}.txt", "w") as f: f.write("\n".join(txt) + "\n")
    print("\n".join(txt)); print(f"\n[fusion_report] -> {out}/fusion_{gid}.txt, .json, graphs/fusion_{gid}_*.png")


if __name__ == "__main__":
    main()
