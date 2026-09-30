# webrtc/ — the libwebrtc transport (frozen)

The original sender / receiver of this testbed: stock libwebrtc M120 (`branch-heads/6099`, commit
`b0b827e0…`, zero patches) with GoogCC congestion control and app-side real-time tracing through official
extension points. It produced every run in `results/2026092[1-9]-*` and the findings in `docs/NOTES.md` up
to 2026-09-29 (GoogCC × RAN coupling). Frozen on 2026-09-30 (tag `webrtc-baseline-2026-09-30` = the last
commit with this code at the repo root); the active transport is `gstreamer/`.

Why frozen: the target scenario (`docs/SCENARIO_EDGE_PROFILES.md`) needs a sender whose fps / resolution /
bitrate are set from outside and never change on their own; libwebrtc changes all three by itself (audit in
§6.1 there). This tree is kept runnable so the GoogCC behaviour can be reproduced or compared later.

**No code is shared with `gstreamer/`.** The two trees share only the run-directory / trace-file contract
(`docs/TRACE_SCHEMA.md`), the RAN (`ran/`, `patches/srsran_gnb/`, `scripts/run/`), the clock-sync scripts
(`scripts/setup/`), `video/assets/` and `analysis/`. Changes here must not leak into the active tree.

## Layout

| path | what |
|---|---|
| `apps/common/` | `trace_ring.h` (lock-free rings), `webrtc_session.h` (PeerConnectionFactory with the injected observers), `webrtc_tracing.h` (RtcEventLog sink, encoder/decoder/GoogCC wrappers), `video_source.h` (capture grid), `signaling_client.h`, `app_util.h` |
| `apps/sender/`, `apps/receiver/` | `video_sender.cc`, `video_receiver.cc` (conductor.cc flow) |
| `apps/signaling/` | TCP JSON-lines relay (SDP exchange) |
| `run_sender.sh`, `run_receiver.sh`, `run_experiment.sh`, `experiments/*.json` | per-terminal scripts and the one-command multi-UE orchestrator |
| `scripts/build_libwebrtc.sh`, `scripts/build_apps.sh`, `libwebrtc.lock` | stock libwebrtc fetch/build (depot_tools), apps build with libwebrtc's bundled clang + libc++ |
| `scripts/local_apps.sh` | app phase of the shared one-PC code test (`scripts/run/run_local_e2e.sh --tree webrtc`) |
| `patches/libwebrtc/README.md` | the extension points used (no patches) |
| `libwebrtc/` (ignored) | the 20 GB checkout or a symlink to one; `build/` (ignored) the binaries |

## Build and run

```bash
make webrtc-build-libwebrtc          # once (hours), or: ln -s <existing checkout> webrtc/libwebrtc
make webrtc-build-apps               # -> webrtc/build/apps/video_{sender,receiver}
make run-local TREE=webrtc           # one-PC code test (srsUE over ZeroMQ)
./webrtc/run_experiment.sh webrtc/experiments/demo-local.json    # loopback smoke test, 2 senders, 12 s
./webrtc/run_experiment.sh webrtc/experiments/2ue-720p30.json    # OTA (gNB PC), laptops run ~/<repo>/webrtc/run_sender.sh
make ota TREE=webrtc                 # gNB PC one-shot with this tree's receivers
```

Working directory of every script is the repo root (`results/`, `video/assets`, `scripts/setup` are shared);
binaries, relay and experiments are taken from this tree. Verified after the move (2026-09-30): demo-local
PASS, `run-local` PASS (`results/20260930-114022-webrtc-moved`).

Defaults of `run_sender.sh`: H264 1280x720@30, `--degradation maintain_resolution`, `--start-bitrate-kbps auto`,
Kendo view K for cam K. Everything else: file headers and `docs/TRACE_SCHEMA.md` (webrtc-only files:
`*-tx-cc.csv`, `*-tx-events.csv`, `*-rx-events.csv`, `*-tx-encoder-rates.csv`, `*-tx/rx-stats.jsonl`).
