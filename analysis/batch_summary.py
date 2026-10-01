#!/usr/bin/env python3
"""Summary of a batch session (ffmpeg/run_batch.sh): every run under results/<session>/runs/.

  .venv/bin/python analysis/batch_summary.py results/<session>

Reads each run's experiment.json (scenario, rotation, which phone/laptop played which camera) and analysis/fusion_<gid>.json
(analysis/fusion_report.py). Writes results/<session>/analysis/batch_summary.{txt,json} and graphs/batch_*.png:
  1. per run and group: deadline met, group latency median / p99
  2. per scenario and group across rotations: observed variation across rotations/rounds (placement AND time/round effects
     together; each rotation ran in a different round, so this is not a placement estimate)
  3. per phone x (scenario, camera profile, deadline): per-camera latency, so different deadlines/backgrounds are never pooled
"""
from __future__ import annotations

import collections, glob, json, os, statistics as st, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, INK2, GRID, SURFACE = "#1f1f1f", "#5f5f5f", "#e6e6e6", "#ffffff"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]     # validated categorical palette, fixed order
plt.rcParams.update({"font.size": 9, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
                     "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "legend.frameon": False})


def f1(x): return "n/a" if x is None else f"{x:.1f}"


def main(session):
    runs = []
    for rd in sorted(glob.glob(f"{session}/runs/*/")):
        rd = rd.rstrip("/")
        try: exp = json.load(open(f"{rd}/experiment.json"))
        except FileNotFoundError: continue
        sc = exp["scenario"]; host = {c: v["host"] for c, v in exp.get("cam_host", {}).items()}
        prof = {c: f"{v['source'].split('_', 1)[1]} {v['kbps']}k" for c, v in sc["cams"].items()}
        groups = []
        for g in sc.get("groups", []):
            f = f"{rd}/analysis/fusion_{g['id']}.json"
            if not os.path.exists(f): groups.append(dict(g, missing=True)); continue
            r = json.load(open(f)); gl = r["1_group_latency"]
            groups.append(dict(g, met=gl["group"]["deadline_met"], median=gl["group"]["median"], p99=gl["group"]["p99"],
                               per_camera=gl["per_camera"]))
        invalid = os.path.exists(f"{rd}/analysis/INVALID") or os.path.exists(f"{rd}/analysis/VERIFY_FAILED")   # excluded from comparisons
        runs.append(dict(rd=rd, name=sc["name"], rot=exp.get("rotation", 0), host=host, prof=prof, groups=groups, invalid=invalid))
    if not runs: sys.exit(f"no runs under {session}/runs/")
    out = f"{session}/analysis"; os.makedirs(f"{out}/graphs", exist_ok=True)
    txt = [f"# batch summary {session}: {len(runs)} runs"]

    txt += ["", "## 1. per run and group (deadline met = share of frames whose group arrived within D)"]
    for r in runs:
        txt.append(f"  {os.path.basename(r['rd'])}{'  [INVALID]' if r['invalid'] else ''}")
        for g in r["groups"]:
            phones = ",".join(f"{c}@{r['host'].get(c, '?').rsplit('.', 1)[-1]}" for c in g["cams"])
            if g.get("missing"): txt.append(f"     {g['id']} ({phones}) D={g['deadline_ms']:.0f}: no fusion analysis yet (run ./ffmpeg/run_batch.sh post)"); continue
            txt.append(f"     {g['id']} ({phones}) D={g['deadline_ms']:.0f}: met {100 * g['met']:.2f} %  median {f1(g['median'])}  p99 {f1(g['p99'])} ms")

    txt += ["", "## 2. per scenario and group across rotations: observed variation across rotations/rounds (placement and round/time effects are confounded)"]
    agg = collections.defaultdict(list)
    for r in runs:
        for g in r["groups"]:
            if not g.get("missing") and not r["invalid"]: agg[(r["name"], g["id"])].append((r["rot"], g["met"], g["p99"]))
    for (name, gid), v in sorted(agg.items()):
        mets = [m for _, m, _ in v]
        txt.append(f"  {name:34s} {gid}: met " + "  ".join(f"r{rot} {100 * m:.2f} %" for rot, m, _ in sorted(v))
                   + (f"   (range {100 * (max(mets) - min(mets)):.2f} pp)" if len(v) > 1 else ""))

    txt += ["", "## 3. per phone (host) x (scenario, camera profile, deadline): per-camera latency median / p99 ms, deadline met"]
    cell = collections.defaultdict(list)
    for r in runs:
        if r["invalid"]: continue
        for g in r["groups"]:
            if g.get("missing"): continue
            for c, s in g["per_camera"].items():   # key includes scenario and deadline: never pool different success criteria
                cell[(r["host"].get(c, "?"), f"{r['name'].split('-')[1]}:{r['prof'][c]} D{g['deadline_ms']:.0f}")].append((s["median"], s["p99"], s["deadline_met"]))
    hosts = sorted({h for h, _ in cell}); profs = sorted({p for _, p in cell})
    def mean_or_none(v): v = [x for x in v if x is not None]; return st.mean(v) if v else None
    for h in hosts:
        txt.append(f"  {h}: " + "   ".join(f"{p}: {f1(mean_or_none(x[0] for x in cell[(h, p)]))}/{f1(mean_or_none(x[1] for x in cell[(h, p)]))} "
                                           f"({100 * st.mean(x[2] for x in cell[(h, p)]):.1f} %, n={len(cell[(h, p)])})" for p in profs if (h, p) in cell))

    # graphs: group deadline met by scenario/group and rotation; per-camera median by phone x profile
    if not agg:
        txt.append("  (no fusion results yet: graphs skipped)")
        json.dump({"runs": runs}, open(f"{out}/batch_summary.json", "w"), indent=2, default=str)
        open(f"{out}/batch_summary.txt", "w").write("\n".join(txt) + "\n"); print("\n".join(txt)); return
    fig, ax = plt.subplots(figsize=(max(6, 1.1 * len(agg) + 2), 3.6))
    keys = sorted(agg); rots = sorted({rot for v in agg.values() for rot, _, _ in v})
    for j, rot in enumerate(rots):
        xs, ys = [], []
        for i, k in enumerate(keys):
            for rr, m, _ in agg[k]:
                if rr == rot: xs.append(i + (j - (len(rots) - 1) / 2) * 0.15); ys.append(100 * m)
        ax.scatter(xs, ys, s=28, color=SERIES[j % len(SERIES)], label=f"rotation {rot}", zorder=3)
    ax.set_xticks(range(len(keys))); ax.set_xticklabels([f"{n.split('-')[1]}:{g}" for n, g in keys], rotation=0)
    ax.set_ylabel("group deadline met (%)"); ax.legend(fontsize=8); ax.set_title("group deadline met per scenario:group and rotation", fontsize=9, loc="left")
    fig.tight_layout(); fig.savefig(f"{out}/graphs/batch_group_met.png", dpi=130); plt.close(fig)
    if cell:
        fig, ax = plt.subplots(figsize=(max(6, 1.4 * len(profs) + 2), 0.5 * len(hosts) + 1.6))
        im = [[(mean_or_none(x[0] for x in cell[(h, p)]) if (h, p) in cell else None) for p in profs] for h in hosts]
        im = [[float("nan") if x is None else x for x in row] for row in im]
        m = ax.imshow(im, cmap="Blues", aspect="auto")
        for i in range(len(hosts)):
            for j in range(len(profs)):
                if (hosts[i], profs[j]) in cell and im[i][j] == im[i][j]: ax.text(j, i, f"{im[i][j]:.0f}", ha="center", va="center", color=INK if im[i][j] < 60 else SURFACE, fontsize=8)
        ax.set_xticks(range(len(profs))); ax.set_xticklabels(profs, fontsize=7, rotation=30, ha="right"); ax.set_yticks(range(len(hosts))); ax.set_yticklabels(hosts, fontsize=8)
        ax.grid(False); fig.colorbar(m, ax=ax, label="per-camera latency median (ms)")
        ax.set_title("phone x (scenario:profile, deadline): per-camera latency median", fontsize=9, loc="left")
        fig.tight_layout(); fig.savefig(f"{out}/graphs/batch_phone_profile.png", dpi=130); plt.close(fig)

    json.dump({"runs": runs}, open(f"{out}/batch_summary.json", "w"), indent=2, default=str)
    open(f"{out}/batch_summary.txt", "w").write("\n".join(txt) + "\n")
    print("\n".join(txt)); print(f"\n[batch_summary] -> {out}/batch_summary.txt, .json, graphs/batch_*.png")


if __name__ == "__main__":
    main(sys.argv[1].rstrip("/") if len(sys.argv) > 1 else sys.exit("usage: batch_summary.py results/<session>"))
