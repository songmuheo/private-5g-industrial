"""Completeness check of one run directory — real-time logs only, nothing is post-processed.

  .venv/bin/python analysis/verify_run.py results/<run>

FAIL (exit 1) when an existing trace is empty, lacks its `# rows=` footer (process killed before its
traces were flushed) or a `.ERROR` sidecar exists (ring overflow / write failure). Prints row counts, event-type counts, RTP sent/received/lost per SSRC (sequence-number set
difference of the two ledgers when both sides are present) and per-RNTI grant counts, so a run can be
sanity-checked at a glance. Writes the same numbers to results/<run>-analysis/summary.json — BESIDE the run
directory, never inside it (CLAUDE.md rule 3: a run directory holds only what the running processes wrote).
"""
from __future__ import annotations


def analysis_dir(rd):
    """results/<run>-analysis/ next to the (resolved) run directory: derived files never go into the run itself."""
    from pathlib import Path
    rd = Path(rd).resolve()
    out = rd.parent / (rd.name + "-analysis")
    out.mkdir(parents=True, exist_ok=True)
    return out

import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from trace_io import iter_error_sidecars, read_trace  # noqa: E402

APP_TRACES = ("tx-frames", "tx-encoded", "tx-encoder-rates", "tx-cc", "tx-rtp", "tx-rtcp", "tx-events",
              "rx-frames", "rx-decoded", "rx-rtp", "rx-rtcp", "rx-events")
GNB_TRACES = ("gnb_sched_dl", "gnb_sched_ul", "gnb_ul_crc", "gnb_dl_harq_ack", "gnb_bsr", "gnb_sr",
              "gnb_csi", "gnb_mac_ul_pdu", "gnb_rlc_ul", "gnb_pdcp_ul", "gnb_pdcp_dl")
GNB_FILES = ("gnb.log", "gnb_stdout.log", "gnb_metrics.jsonl", "gnb_mac.pcap", "gnb_ngap.pcap", "gnb_n3_gtpu.pcap")


def line_count(f: pathlib.Path) -> int:
    return sum(1 for _ in f.open(errors="ignore")) if f.exists() else 0


def main(run_dir: str) -> int:
    rd = pathlib.Path(run_dir)
    fails: list[str] = []
    summary: dict = {"run_dir": str(rd), "streams": {}, "gnb": {}}

    def fail(msg: str) -> None:
        fails.append(msg)
        print(f"[FAIL] {msg}")

    for s in iter_error_sidecars(rd):
        fail(f"error sidecar {s.relative_to(rd)}: {s.read_text().strip()}")

    # --- application traces (sender *-tx-*, receiver *-rx-*; either or both may be present) ------
    # A sender trace is looked up in app/ (one-PC runs) or in senders/<stream>/app/ (run_experiment.sh collects the
    # laptops' traces there), so a multi-UE run is verified end to end: footers on both sides and the RTP join.
    app = rd / "app"
    def tx_dir(stream: str) -> pathlib.Path:
        d = rd / "senders" / stream / "app"
        return d if (d / f"{stream}-tx-frames.csv").exists() else app
    streams = set()
    if app.exists():
        streams |= {p.name[: -len(f"-{side}-frames.csv")] for side in ("tx", "rx") for p in app.glob(f"*-{side}-frames.csv")}
    streams |= {p.parent.parent.name for p in rd.glob("senders/*/app/*-tx-frames.csv")}
    streams = sorted(streams)
    if not streams:
        fail("no <stream>-{tx,rx}-frames.csv under app/ or senders/*/app/")
    for s in streams:
        st: dict = {"tx_dir": str(tx_dir(s).relative_to(rd))}
        for name in APP_TRACES:
            f = (tx_dir(s) if name.startswith("tx-") else app) / f"{s}-{name}.csv"
            rows, footer = read_trace(f)
            st[name] = {"rows": len(rows), "footer": footer}
            if f.exists() and not rows:
                fail(f"{f.name} is empty")
            if f.exists() and rows and "rows" not in footer:
                fail(f"{f.name} has no '# rows=' footer: the process did not shut down cleanly (killed / SIGPIPE), tail lost")
            if name.endswith("events") and rows:
                kinds = collections.Counter(r["event"] for r in rows)
                st[name]["kinds"] = dict(kinds)
                print(f"       {f.name}: " + ", ".join(f"{k}={v}" for k, v in sorted(kinds.items())))
        st["tx-stats.jsonl"] = line_count(tx_dir(s) / f"{s}-tx-stats.jsonl")
        st["rx-stats.jsonl"] = line_count(app / f"{s}-rx-stats.jsonl")
        tx, _ = read_trace(tx_dir(s) / f"{s}-tx-rtp.csv")
        rx, _ = read_trace(app / f"{s}-rx-rtp.csv")
        # (ssrc, seq, rtp_ts) as the packet key: the 16-bit sequence number wraps every ~65 k packets (a 300 s
        # 720p run sends ~100 k), rtp_ts disambiguates the wraps
        sent = collections.defaultdict(set)
        got = collections.defaultdict(set)
        for p in tx:
            if p["dir"] == "out":
                sent[p["ssrc"]].add((p["seq"], p["rtp_ts"]))
        for p in rx:
            if p["dir"] == "in":
                got[p["ssrc"]].add((p["seq"], p["rtp_ts"]))
        st["rtp"] = {}
        if tx and rx:
            for ssrc, seqs in sent.items():
                lost = len(seqs - got.get(ssrc, set()))
                st["rtp"][str(ssrc)] = {"sent": len(seqs), "received": len(got.get(ssrc, ())), "lost": lost}
                print(f"       {s} ssrc={ssrc} sent={len(seqs)} received={len(got.get(ssrc, ()))} lost={lost}")
        print(f"       {s} rows: " + ", ".join(f"{n}={st[n]['rows']}" for n in APP_TRACES if st[n]["rows"])
              + f"; stats.jsonl tx={st['tx-stats.jsonl']} rx={st['rx-stats.jsonl']}" + (f"  (tx from {st['tx_dir']})" if st['tx_dir'] != 'app' else ""))
        summary["streams"][s] = st

    # --- gNB traces --------------------------------------------------------------------------
    gnb = rd / "gnb"
    g = summary["gnb"]
    if gnb.exists():
        for name in GNB_TRACES:
            rows, footer = read_trace(gnb / f"{name}.csv")
            g[name] = {"rows": len(rows), "footer": footer}
            if (gnb / f"{name}.csv").exists() and not rows:
                print(f"[WARN] {name}.csv has no rows")
            if rows and "rows" not in footer:
                fail(f"{name}.csv has no '# rows=' footer: the gNB did not shut down cleanly, tail lost")
        for name in GNB_FILES:
            f = gnb / name
            g[name] = {"bytes": f.stat().st_size if f.exists() else 0}
            if name.endswith((".log", ".jsonl")):
                g[name]["lines"] = line_count(f)
        per_rnti: dict = collections.defaultdict(collections.Counter)
        for d in ("ul", "dl"):
            for r in read_trace(gnb / f"gnb_sched_{d}.csv")[0]:
                c = per_rnti[r["rnti"]]
                c[f"{d}_grants"] += 1
                c[f"{d}_tbs_bytes"] += r["tbs_bytes"]
                c[f"{d}_retx"] += 0 if r["new_data"] else 1
        g["per_rnti"] = {hex(k): dict(v) for k, v in per_rnti.items()}
        crc = read_trace(gnb / "gnb_ul_crc.csv")[0]
        if crc:
            g["ul_bler"] = sum(1 for r in crc if not r["crc_ok"]) / len(crc)
        print("       gnb rows: " + ", ".join(f"{n[4:]}={g[n]['rows']}" for n in GNB_TRACES))
        print("       gnb files: " + ", ".join(f"{n}={g[n].get('lines', g[n]['bytes'])}" for n in GNB_FILES if g[n]["bytes"]))
        for rnti, c in g["per_rnti"].items():
            print(f"       rnti={rnti} UL grants={c.get('ul_grants', 0)} ({c.get('ul_tbs_bytes', 0) / 1e6:.2f} MB, "
                  f"retx {c.get('ul_retx', 0)}) DL grants={c.get('dl_grants', 0)} ({c.get('dl_tbs_bytes', 0) / 1e6:.2f} MB, "
                  f"retx {c.get('dl_retx', 0)})" + (f"; UL BLER={g['ul_bler']:.4f}" if "ul_bler" in g else ""))
    for rel in ("core/open5gs.log", "ue/ue.log", "ue/ue_metrics.csv"):
        f = rd / rel
        if f.exists():
            summary[rel] = line_count(f)
            print(f"       {rel}: {summary[rel]} lines")

    out = analysis_dir(rd)
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str))
    print(f"[verify_run] {'FAIL' if fails else 'PASS'} ({len(fails)} failures) -> {out / 'summary.json'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
