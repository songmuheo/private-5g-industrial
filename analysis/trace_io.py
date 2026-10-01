"""Small helpers to read the CSV traces written by the apps and the gNB tracer.

Every trace file ends with '# ...' footer lines (row count, clock_domain, overflow). Rows are
returned as dicts of typed values; footers are returned separately.
"""
from __future__ import annotations

import csv
import pathlib
from typing import Iterator


def read_trace(path: pathlib.Path) -> tuple[list[dict], dict]:
    """Return (rows, footer). Numeric columns are converted to int/float when possible."""
    rows: list[dict] = []
    footer: dict = {}
    if not path.exists():
        return rows, footer
    with path.open() as f:
        header = None
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        footer[k] = v
                continue
            if header is None:
                header = line.split(",")
                continue
            vals = line.split(",")
            if len(vals) != len(header):
                footer.setdefault("malformed_rows", 0)
                footer["malformed_rows"] += 1
                continue
            rows.append({k: _num(v) for k, v in zip(header, vals)})
    return rows, footer


def _num(v: str):
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v


def percentiles(xs: list[float], ps=(50, 90, 99)) -> dict:
    if not xs:
        return {f"p{p}": None for p in ps}
    s = sorted(xs)
    out = {}
    for p in ps:
        k = max(0, min(len(s) - 1, int(round(p / 100 * (len(s) - 1)))))
        out[f"p{p}"] = s[k]
    out["min"] = s[0]
    out["max"] = s[-1]
    out["n"] = len(s)
    return out


def iter_error_sidecars(root: pathlib.Path) -> Iterator[pathlib.Path]:
    # os.walk(followlinks=True): a sub-run's gnb/ is a symlink to the session's gNB traces, and Path.rglob does not
    # descend into directory symlinks (Python < 3.13) -> an overflow sidecar there would be missed
    import os
    for dp, _, fn in os.walk(root, followlinks=True):
        for n in fn:
            if n.endswith(".ERROR"): yield pathlib.Path(dp) / n

def run_window(rd):
    """[start - 1 s, max(start + duration + 5 s, last packet sent by a DECLARED camera + 5 s)] in wall ns, or None without
    experiment.json. The end follows the recorded execution of the scenario's own cameras (their tx-rtp log; receiver
    rx-rtp as fallback): gstreamer/webrtc senders count their duration from application start, which can be later than
    the scheduled start. Files of cameras not in the scenario are ignored."""
    import csv as _csv, json as _json, os as _os
    p = f"{rd}/experiment.json"
    if not _os.path.exists(p): return None
    e = _json.load(open(p)); t0 = int(e["start_time_epoch"] * 1e9); end = t0 + (int(e["scenario"].get("duration_s", 300)) + 5) * 1_000_000_000
    for cam in e["scenario"].get("cams", {}):
        for f, col in ((f"{rd}/senders/{cam}/app/{cam}-tx-rtp.csv", "log_wall_ns"), (f"{rd}/app/{cam}-rx-rtp.csv", "log_wall_ns")):
            if not _os.path.exists(f): continue
            last = None
            with open(f) as fh:
                for r in _csv.DictReader(l for l in fh if not l.startswith("#")):
                    if r.get(col): last = r[col]
            if last: end = max(end, int(last) + 5_000_000_000); break   # the sender's own log when present
    return (t0 - 1_000_000_000, end)
