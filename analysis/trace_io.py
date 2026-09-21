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
    yield from root.rglob("*.ERROR")
