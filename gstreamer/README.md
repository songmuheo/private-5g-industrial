# gstreamer/ — the active transport: fixed-profile RTP/H.264 over GStreamer

Sender / receiver for the scenario in `docs/SCENARIO_EDGE_PROFILES.md`: a camera whose **resolution, fps
and bitrate are set from outside and never change on their own**. GStreamer 1.20 from the distribution
(no source build, no patches), x264enc (zerolatency, ABR+VBV, deterministic GOP), rtph264pay/rtpbin, plain
UDP; no congestion controller, no pacer, no frame dropper (x264 has no frame-skip mode). Every trace of the
webrtc tree that has a counterpart here keeps its file name and columns, so `analysis/` reads both.

**No code is shared with `webrtc/`** (the frozen libwebrtc tree). Shared: the run-directory / trace-file
contract (`docs/TRACE_SCHEMA.md`), the RAN and core (`ran/`, `patches/srsran_gnb/`, `scripts/run/`), the
clock-sync scripts (`scripts/setup/`), `video/assets/`, `analysis/`, `results/`.

## Layout

| path | what |
|---|---|
| `apps/common/gst_util.h` | trace rows (`tx-encoded`, `tx-encoder-rates`, `*-rtp`, `*-rtcp`, `rx-decoded`), RTP/RTCP parsing, `rtp_ts` arithmetic of rtph264pay, per-frame side info as `GstReferenceTimestampMeta`, provenance |
| `apps/common/video_source.h` | capture grid (absolute-time `clock_nanosleep`) pushing zero-copy buffers into `appsrc`; one `tx-frames` row per slot |
| `apps/common/trace_ring.h`, `app_util.h`, `control_client.h`, `json.hpp` | lock-free rings, clocks/log/CLI, TCP JSON-lines client (independent copies) |
| `apps/sender/video_sender.cc` | `appsrc ! x264enc ! rtph264pay ! rtpbin` → udpsink; RTCP in/out on one socket; `profile` messages change `bitrate` while PLAYING |
| `apps/receiver/video_receiver.cc` | `udpsrc ! rtpbin ! rtph264depay ! avdec_h264 ! fakesink`; one stream per process; frame side info travels as metas from the depayloader to the sink |
| `apps/control/control_server.py` | control channel: receiver ports → sender, `stream-start` (SSRC, profile) → receiver, `profile` → sender |
| `run_sender.sh`, `run_receiver.sh`, `run_experiment.sh`, `experiments/*.json` | same shape as the webrtc tree; cams carry `width/height/fps/kbps[/gop/vbv_ms]/source` |
| `scripts/build_apps.sh`, `scripts/local_apps.sh`, `scripts/fetch_gst_examples.sh`, `gstreamer.lock` | build (system g++, pkg-config), app phase of `make run-local`, upstream examples at tag 1.20.3 (`gstreamer-src/`, ignored) |

## Build and run

```bash
make deps                          # incl. libgstreamer1.0-dev, plugins base/good/bad/ugly, libav
make build-apps                    # -> gstreamer/build/apps/video_{sender,receiver} + BUILD_INFO.txt (versions)
make run-local                     # one-PC code test (srsUE over ZeroMQ)
./gstreamer/run_experiment.sh gstreamer/experiments/demo-local.json   # loopback, 2 senders, 12 s
./gstreamer/run_experiment.sh gstreamer/experiments/2ue-720p30.json   # OTA from the gNB PC; laptops: ~/<repo>/gstreamer/run_sender.sh
make ota                           # gNB PC one-shot with this tree's receivers
gstreamer/scripts/fetch_gst_examples.sh                              # upstream examples the code cites (reference only)
```

Working directory of every script is the repo root (`results/`, `video/assets`, `scripts/setup` are shared).

## What is fixed, and how (verified 2026-09-30, `docs/NOTES.md`)

| | mechanism | check |
|---|---|---|
| resolution | `appsrc` caps pin WxH; the encoder is reconfigured only on a caps change (none during a run) | `tx-encoded.width/height` constant |
| fps | the capture grid defines the frame times; nothing downstream drops (`qos` off everywhere, `videorate` not used, `appsrc block=true` makes an overloaded encoder visible as grid gaps instead of silent drops) | `tx-frames` rows == `tx-encoded` rows (± the last frame at shutdown) |
| bitrate | x264 `pass=cbr bitrate=N vbv-buf-capacity=V`: a target ceiling the content may stay under; changeable while PLAYING (`profile` message) | `tx-encoder-rates`, `tx-encoded.bytes` |
| GOP | `key-int-max=G` + `scenecut=0:min-keyint=G` → one IDR every G frames, never content-triggered | `tx-encoded.is_idr` |
| burst shape | no pacer: an access unit leaves as one burst of RTP packets right after encoding | `tx-rtp` first→last packet of a frame |
| frame identity | `rtph264pay timestamp-offset=0` → wire `rtp_ts = PTS × 90 kHz`, computed by the grid thread before the push; sender, gNB PDCP rows and receiver log the same number (no per-SSRC offset) | join `tx-frames`/`gnb_pdcp_ul`/`rx-frames` on `rtp_ts` |

Control flow: receiver registers its RTP/RTCP ports → server tells the sender (`receiver-ready`) → sender
announces `stream-start` (SSRC, profile) → receiver opens its traces → sender goes PLAYING. The receiver's
RTCP reports go to the sender's RTCP source port (symmetric, RFC 4961). Limit: the sender address the server
sees must be the one the receiver can reach (true on the testbed: receivers on the gNB PC, UE pool routed).

Not in this tree (by design): NACK/RTX, FEC, header extensions, network-adaptive bitrate. If a RAN-aware or
GCC-like controller is wanted as an experimental condition, it sets the same `bitrate` property through the
`profile` message; fps and resolution stay caps.
