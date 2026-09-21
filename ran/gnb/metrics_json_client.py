#!/usr/bin/env python3
"""Subscribe to the srsRAN gNB remote-control WebSocket and append its JSON metrics to a file.

srsRAN_Project >= 25.x publishes the per-UE / per-cell metrics (the same data as the stdout table)
as JSON over the remote-control server when `remote_control: enabled: true` and
`remote_control: metrics: enable_json: true` are set (apps/services/remote_control). Protocol:
send {"cmd": "metrics_subscribe"}; every metrics period a JSON text message arrives.

  ran/gnb/metrics_json_client.py --url ws://127.0.0.1:8001 --out results/<run>/gnb/gnb_metrics.jsonl

Each output line: {"recv_wall_ns": <int>, "recv_mono_ns": <int>, "metrics": <srsRAN JSON>}.
"""
import argparse
import asyncio
import json
import sys
import time

import websockets


async def run(url: str, out_path: str, timeout_s: float) -> int:
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            async with websockets.connect(url, open_timeout=3) as ws:
                await ws.send(json.dumps({"cmd": "metrics_subscribe"}))
                print(f"[metrics_json_client] subscribed at {url} -> {out_path}", file=sys.stderr, flush=True)
                with open(out_path, "a") as f:
                    async for msg in ws:
                        try:
                            payload = json.loads(msg)
                        except json.JSONDecodeError:
                            payload = {"raw": msg}
                        if isinstance(payload, dict) and payload.get("cmd") == "metrics_subscribe":
                            continue  # command acknowledgement
                        f.write(json.dumps({"recv_wall_ns": time.time_ns(),
                                            "recv_mono_ns": time.monotonic_ns(),
                                            "metrics": payload}) + "\n")
                        f.flush()
                return 0
        except (OSError, websockets.exceptions.WebSocketException) as e:
            if time.monotonic() > deadline:
                print(f"[metrics_json_client] giving up: {e}", file=sys.stderr)
                return 1
            await asyncio.sleep(1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="ws://127.0.0.1:8001")
    ap.add_argument("--out", required=True)
    ap.add_argument("--connect-timeout", type=float, default=60.0, help="seconds to keep retrying the connection")
    a = ap.parse_args()
    try:
        sys.exit(asyncio.run(run(a.url, a.out, a.connect_timeout)))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
