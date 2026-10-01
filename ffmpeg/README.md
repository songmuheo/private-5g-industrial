# ffmpeg/ — SMEC-style transport: pre-encoded H.264 → RTP/UDP (FFmpeg libraries)

The third, fully independent transport tree. A **pre-encoded** Annex B H.264 file (one or more bitrate rungs of
the same content) is sent as RTP over UDP on a fixed capture grid; no encoder runs on the sender, no congestion
control, no pacer, no retransmission. This is how SMEC / ARMA / Tutti stream (`docs/reference_code/
smec-edge-applications`, `streamer.cpp`): libavformat's `rtp` muxer fed with file packets, paced by the
application. What it gives the RAN study that the live-encoder trees cannot: **bit-identical frames across runs,
cameras and hosts** (the burst pattern is a property of the file), no encoder latency or CPU dependence on the
laptops, and per-frame sizes known a priori (the RAN-facing descriptor of docs/SCENARIO_EDGE_PROFILES.md §3).
The edge changes the bitrate by selecting another rung (`profile` message), applied at the next IDR — the
discrete-profile model of AWStream / ASTRA / VideoEdge.

**No code is shared with `gstreamer/` or `webrtc/`.** Shared by contract only: the run-directory / trace-file
layout (`docs/TRACE_SCHEMA.md` §0-a), the control-channel message protocol (a copy of the gstreamer one), the
RAN scripts, `scripts/setup/`, `video/assets/`, `analysis/`, `results/`.

## Layout

| path | what |
|---|---|
| `apps/common/rtp_util.h` | trace rows, RTP/RTCP header parsing, Annex B access-unit indexer (AUD-delimited), RFC 6184 depacketizer (single NAL / STAP-A / FU-A) |
| `apps/common/{trace_ring,app_util,control_client}.h`, `json.hpp` | independent copies |
| `apps/sender/video_sender.cc` | mmap'ed rungs → per grid slot one AU → libavformat `rtp` muxer → our AVIO callback → our UDP socket (each packet logged as it is sent); RTCP SR from the muxer forwarded; rung switch at IDR on `profile` |
| `apps/receiver/video_receiver.cc` | our UDP socket with `SO_TIMESTAMPNS` (kernel arrival per packet) → depacketizer → libavcodec H.264 (1 thread, low delay) → traces; no RTCP sent back (as SMEC) |
| `apps/control/control_server.py` | control channel (copy of gstreamer's protocol: ports, `stream-start`/`stream-ack`, `profile`) |
| `apps/tests/depack_test.cc` | asserted cases against the rtp muxer: byte-identical round trip + ts progression, mid/tail/head packet loss, duplicate, reorder, two AUs ended by one packet, sequence wraparound. Run on every build |
| `scripts/prepare_video.sh` | **run once on the gNB PC**: MOT17-02 (MOTChallenge, 600 frames, the SMEC/ARMA sequence) and the Kendo views → rungs `video/assets/<src>_<WxH>_<fps>_<kbps>k.h264` (x264: AUD per AU, headers on IDR, GOP 60, scenecut off, **HRD-CBR** so bytes/frame track the rung). Each rung is encoded to a temp file, checked (frame count, IDR every GOP) and renamed; `h264_ladders.txt` is the manifest that lets a rung be reused only with identical settings |
| `scripts/prepare_client.sh <K>` | **run once on each laptop**: packages, build, rsync of the rungs from the gNB PC over the sync LAN, sync-LAN/chrony, checks |
| `run_sender.sh`, `run_receiver.sh`, `run_experiment.sh`, `experiments/*.json`, `scripts/local_apps.sh` | same shape as the other trees; cams carry `fps`, `source` (name prefix), `rungs` (kbps list), `kbps` (start rung) |

## Build and run

```bash
make deps && make build-apps TREE=ffmpeg                     # gNB PC (also runs the depacketizer round-trip test)
ffmpeg/scripts/prepare_video.sh                              # gNB PC, once: MOT17 download (5.9 GB, cached) + Kendo, all rungs
ffmpeg/scripts/prepare_client.sh K                           # each laptop, once
make run-local TREE=ffmpeg                                   # one-PC code test (srsUE)
./ffmpeg/run_experiment.sh ffmpeg/experiments/demo-local.json     # loopback, 2 senders, 12 s
./ffmpeg/run_experiment.sh ffmpeg/experiments/2ue-720p30.json     # OTA: both UEs MOT17-02, rungs 500..4000, start 2500
./ffmpeg/run_experiment.sh ffmpeg/experiments/2ue-720p30-fit.json # OTA: cam0 2500 / cam1 1000 (profile fitted to the links)
printf '{"type":"profile","session":"s1","stream":"cam1","bitrate_kbps":1000}\n' | nc -q1 10.53.1.1 8765   # edge: switch cam1's rung
```

## What is fixed, and how (verified 2026-09-30)

| | mechanism | check |
|---|---|---|
| resolution, GOP, bytes per frame | the file: identical on every host and run (HRD-CBR: 2500k rung = 10417 B/frame ± the IDR) | `tx-encoded.bytes` |
| fps | the capture grid (absolute `clock_nanosleep`); a slot missed behind a blocked `sendto` is written with `to_encoder=0` | `tx-frames` == `tx-encoded` |
| bitrate | the rung; changed only by `profile` at an IDR (`tx-encoder-rates` row per switch) | loopback: 10417 → 4167 B/frame at frame 120 after a `profile` at t=4 s |
| frame identity | wire `rtp_ts` = the muxer's random base + 90 kHz pts; the sender learns the base from its first packet and logs wire values → `tx-frames`/`gnb_pdcp_ul`/`rx-frames` join directly | run-local: 840/840 frames joined at the gNB and the app |
| delay decomposition | capture → AU handed to muxer (`tx-encoded`, ≈0) → last packet sent (`tx-rtp`) → kernel arrival (`rx-rtp`, SO_TIMESTAMPNS) → decode (`rx-decoded`) → app (`rx-frames`) | loopback capture→app 2.7 ms median |

Loss: the receiver counts per AU the missing sequence numbers (`lost_packets`; RFC 3550 A.1 rules separate loss from
late/duplicate packets and sequence resets), the discarded partial NALs (`lost_fragments`: an FU-A whose end or start
was lost is dropped, never handed to the decoder as a half NAL) and damage (`aus_damaged`); incomplete AUs are still
decoded from their whole NALs (frames may be corrupt, exactly as in SMEC's receiver), AUs with nothing left are
counted (`aus_empty`) and skipped. There is no RTCP RR, NACK or FEC — a lost packet stays lost, and the traces say
which one. A datagram the sender's kernel refused is a local failure (`send_failures`, ERROR log), not path loss.

Timing contract: `run_experiment.sh` picks one wall-clock epoch T for the run; every sender connects, negotiates and
then captures at T + k/fps (`--start-at-epoch`), slot k carrying source frame (k + phase) mod N, so capture instants
of all cameras are aligned (chrony, ~50 µs) and pts 0 = T on every host; the RTCP SR epoch is the same T; `--duration`
ends at T + duration on every host. `phase_slots` per camera (`--phase-slots`) staggers the IDR bursts: with phase 0
everywhere all cameras send their IDR in the same slot every GOP (worst case for the RAN); the 2-UE scenarios use
0 and 30 (GOP 60), so IDRs alternate every second. A phased camera begins at its first IDR slot (cam1: slot 30), and
a sender that starts after T enters at the next IDR slot of its timeline.

**Aligned frame numbers, staggered IDRs (`idr_origin`, the 5-UE design).** `prepare_video.sh --source mot17-03` encodes
the longest MOT17 sequence (1500 frames, 50 s, static) once per camera, rotated so camera K's file starts at content
frame P_K = 3K. In a scenario, `"source": "mot17-03_1280x720_30", "idr_origin": P` selects `mot17-03-oP_*` and passes
`--content-origin P`: every camera sends content frame k mod 1500 in slot k (same frame number at the same instant,
`tx-frames.src_frame_idx` = content frame), while camera K's IDRs fall on content frames P_K + 60m (100 ms apart for
3-frame spacing). 1500 is a multiple of the GOP, so the loop seam adds no IDR. A camera starts at its first IDR slot
(P_K, at most 0.4 s after T); measure from T + 5 s. Scenarios: `5ue-720p30.json`, `2ue-720p30-mot03.json`,
`demo-local-5cam.json`.

**Bitrate in the profile.** Each camera's profile is (1280x720, 30 fps, bitrate): `kbps` in the scenario is the start
bitrate, and `{"type":"profile","stream":"camK","bitrate_kbps":B}` switches to rung B at that camera's next IDR (with
staggered IDRs the cameras switch at their own IDR instants). Pre-encoded means B must be one of the rungs (default
500/1000/1500/2500/4000; another value = one more `--rungs` encode). Derived files (verify summary, report, graphs, INVALID)
go to `results/<run>-analysis/`, never into the run directory.
