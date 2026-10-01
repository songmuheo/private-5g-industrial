# Architecture

## 1. Topology

```
        UE site                              gNB PC (ran/, patches/, results/)                    receiver host (gNB PC 10.53.1.1 or internet)
┌─────────────┐ USB ┌───────┐ NR n78 TDD ┌──────┐ UHD ┌───────────────────────────┐ N2/N3 ┌──────────────┐ NAT ┌────────────────────┐
│ laptop      │─────│ Pixel │ ~~~~~~~~~ │ B210 │─────│ srsRAN gNB + p5g tracer   │───────│ Open5GS 5GC  │─────│ video_receiver     │
│ video_sender│     │ (UE)  │           └──────┘     │ -> results/<run>/gnb      │       │ (docker)     │     │ control server     │
└─────────────┘     └───────┘                        └───────────────────────────┘       └──────────────┘     └────────────────────┘
```

### Transport trees (2026-09-30)

The sender/receiver pair comes from one of two self-contained trees that **share no code**:

| | `gstreamer/` (active) | `webrtc/` (frozen, tag `webrtc-baseline-2026-09-30`) |
|---|---|---|
| model | camera with a fixed profile: resolution / fps / bitrate set from outside (edge), never changed by the stack | interactive RTC endpoint: GoogCC decides bitrate, libwebrtc adapts fps / resolution / drops frames |
| stack | GStreamer 1.20 (distribution packages) + x264enc (zerolatency, ABR+VBV, fixed GOP), rtph264pay/rtpbin, UDP; no CC, no pacer, no NACK | libwebrtc M120 stock, official extension points for tracing, GoogCC, pacer, NACK/RTX, ICE/DTLS/SRTP |
| control | TCP JSON `control_server.py`: receiver ports, `stream-start` (SSRC, profile), `profile` push → sender `bitrate` | TCP JSON `signaling_server.py`: SDP relay |
| frame identity | wire `rtp_ts = PTS × 90 kHz` (timestamp-offset 0) — identical in `tx-frames`, `gnb_pdcp_ul`, `rx-frames` | wire = sender `rtp_ts` + random K per SSRC; receiver estimates the sender value from abs-capture-time |
| why | `docs/SCENARIO_EDGE_PROFILES.md`: the edge-issued-profile scenario needs a deterministic sender; §6.1 lists the ten places where libwebrtc changes the profile on its own | produced every run up to 2026-09-29 and the GoogCC × RAN findings in NOTES; kept runnable for comparison |

Shared between the trees (by contract, not by code): `results/<run>/` layout and footers, the trace file
names/columns (`docs/TRACE_SCHEMA.md` §0-a), `run_gnb_core.sh` / `scripts/run/*` (RAN, core, `results/CURRENT`),
`scripts/setup/*` (clock sync), `video/assets`, `analysis/`. The shared RAN scripts take the tree as a parameter
(`make ... TREE=`, `run_local_e2e.sh --tree`, `P5G_TREE`) and call `<tree>/scripts/local_apps.sh start|stop` /
`<tree>/run_receiver.sh`.

* Core is co-located with the gNB (docker bridge `10.53.1.0/24`; gNB N2/N3 on `10.53.1.1`, Open5GS
  `10.53.1.2`, UE pool `10.45.0.0/16` NATed to the internet).
* Receiver and signaling relay are on the internet side; the UE only needs outbound connectivity. ICE
  works through the UPF NAT because the UE side initiates the checks; `--ice-servers stun:…` adds
  server-reflexive candidates if the internet host is itself behind NAT.
* Downlink experiments swap the two apps; tracing is symmetric.

## 2. What is logged where (all real time)

```
sender                                   gNB (uplink)                                    receiver
capture ─► encode ─► RTP out ─► [UE stack+air] ─► MAC PDU ─► RLC SDU ─► PDCP SDU ─► [core+internet] ─► RTP in ─► decode ─► app
tx-frames  tx-encoded(+rates) tx-cc tx-rtp      sched_ul/ul_crc  mac_ul_pdu rlc_ul   pdcp_ul                          rx-rtp    rx-decoded rx-frames
tx-events (GoogCC: delay/loss estimate, probes, ALR; ICE/DTLS)        rx-events                          tx/rx-stats.jsonl (1 s getStats)
```

* Frame identity = RTP timestamp (set by the source from the capture grid). The receiver sees
  `rtp_ts + K` (per-SSRC random offset, RFC 3550) and logs `sender_rtp_ts_est` live from the
  abs-capture-time extension so rows can be matched by eye.
* Packet identity = `(ssrc, seq)`: the gNB PDCP hook parses the RTP header of each user-plane SDU
  (clear even with SRTP) → joins app ledgers to RAN events without touching the UE.
* Per-UE RAN attribution: `gnb_sched_{ul,dl}` (who got which PRBs/MCS/TBS in which slot),
  `gnb_ul_crc` / `gnb_dl_harq_ack`, `gnb_bsr` / `gnb_sr`, `gnb_csi`, `gnb_rlc_ul`.
* Official extras: srsRAN JSON metrics (remote-control WebSocket), stdout metrics table, gNB log,
  Open5GS log; optional stock pcaps (`P5G_GNB_PCAP=1`).

## 3. Design decisions

| decision | chosen | alternatives / why not |
|---|---|---|
| RAN | srsRAN_Project 25.10, native, UHD | OAI (earlier work): srsRAN's COTS-UE + B210 path is the documented tutorial; docker gNB rejected (USB/UHD passthrough) |
| Core | Open5GS via srsRAN's docker recipe, on the gNB PC | any 5GC was acceptable; this one is what the srsRAN COTS-UE tutorial pairs with |
| gNB tracing | build-time patches, fixed circular rings, background writer | JSON metrics alone (1 s aggregates) too coarse; pcap lacks scheduler decisions; `dprintf` in hooks blocks RT threads; linear "whole-run" buffers cost GBs of RSS |
| App transport (2026-09-30) | GStreamer fixed-profile sender (`gstreamer/`), libwebrtc frozen in `webrtc/` | keeping libwebrtc as a "profile executor" means overriding ~6 internal mechanisms from outside (docs/SCENARIO_EDGE_PROFILES.md §6.1: FrameDropper trial, OpenH264 skip hard-coded on, bitrate adjuster on, pacer emergency stop, DISABLED degradation, CC injection) and re-auditing them per version; FFmpeg (SMEC/ARMA path) has no runtime profile change and no RTP session; GStreamer pins WxH@fps by caps, x264 never skips, `bitrate` is changeable while PLAYING, rtpbin gives RTCP and, later, RTSP/analytics-pipeline compatibility. Two trees instead of a `--transport` switch: different toolchains (libwebrtc's libc++ vs system g++) and the wish to keep the old stack runnable without coupling |
| GStreamer source | distribution packages, upstream examples fetched read-only (`gstreamer.lock` @ 1.20.3) | a `third_party/gstreamer` submodule would add a meson build of the monorepo on every host for zero patches; versions are pinned by `make deps` + `BUILD_INFO.txt` + the sender's `config:` line instead |
| libwebrtc tracing (webrtc tree) | official extension points, zero patches | patching `RtcEventLog` (earlier work) modifies third-party code; injecting our `RtcEventLog` gives ns timestamps and all event types |
| Encoded-frame hook (webrtc tree) | `VideoEncoderFactory` wrapper | `FrameTransformer` makes the M120 send path asynchronous |
| Control channel | TCP JSON-lines server per tree (SDP relay / port + profile exchange) | SFU / WebSocket stacks add hops and dependencies; P2P is the topology under study |
| App toolchain | gstreamer: system g++ + pkg-config; webrtc: libwebrtc's bundled clang + libc++ (static) | system libstdc++ is ABI-incompatible with libwebrtc's `use_custom_libcxx=true`, which is also why the two trees cannot share one CMake project |
| Code test | srsUE (srsRAN_4G) over ZeroMQ in netns `ue1`, `run_local_e2e.sh` | kept only to exercise code + logging on one PC; srsUE is 15 kHz FDD only, so the profile differs from the n78 TDD testbed profile |

## 4. Known limits

* RTP `in` timestamps are after the socket read (gstreamer: `udpsrc` output; webrtc: after SRTP decryption), not kernel arrival.
* webrtc tree: libwebrtc emits no per-frame decode event through `RtcEventLog`; decode timing comes from the
  decoder-factory wrapper (`-rx-decoded.csv`). gstreamer tree: no per-frame QP from x264enc / avdec_h264 (`qp=-1`).
* gstreamer tree: the control server reports the sender's TCP peer address to the receiver for RTCP; correct when the
  receiver runs on the control host (testbed) or no NAT sits between them. Only H.264 (x264enc) is built in.
* Cross-host latencies depend on wall-clock sync.
* Phones request their own APN name as DNN; the core subscription must list it (`core_add_dnn.sh`,
  default `oai`) or the AMF rejects the PDU session ("DNN Not Supported").

## Analysis tooling
* `analysis/exp_run_report.py` (2026-09-28): offline per-run report used in docs/NOTES.md — app per stream (fps, loss, bitrate, capture->arrival, frame span) and gNB UL link (CRC failure by MCS/slot/TB size/SINR, OLLA, HARQ completion, RLC t-Reassembly expiries, BSR, PRB use). Reason: the 4-UE/5-UE diagnosis needed a time-proximity sched_ul<->ul_crc join and cross-layer alignment that verify_run.py (completeness only) does not do. Experimental (exp_ prefix); reads results, writes nothing.
* `analysis/exp_ran_audit.py` (2026-09-28): RAN-side audit per run (slot occupancy, grants/fairness, scheduling latency, reception stability, control channels, anomalies, RAN-internal delay, capacity). Reason: deciding whether the testbed RAN can be used as-is needed all gNB traces cross-read at once. Experimental; reads only.
* `docs/RAN_CONFIG.md` (2026-09-28): the gNB profile explained value by value with the measurement behind each change and the known limits; README carries the summary. `run_sender.sh --start-at` and the `clock*.txt` records exist so multi-UE runs start together and cross-host timestamps carry their sync accuracy.
* `scripts/setup/sync_lan_{server,client,check}.sh` (2026-09-29): wired clock-sync LAN (switch + spare NIC, static IPs, chrony, firewall that keeps ICE off that LAN) and a go/no-go check; `exp_run_report.py` section A2 verifies per stream that the selected ICE pair is the 5G path. Reason: sub-ms cross-host timestamps and a guarantee that media never bypasses the RAN.
* `run_experiment.sh` + `experiments/*.json` (2026-09-29): one-command experiment from the gNB PC (scenario -> receivers, laptop preflight over the sync LAN, per-camera sender settings, synchronised start, trace collection, verify, report). Reason: five laptops typed by hand gave inconsistent settings and start times and lost the sender traces; the scenario file is archived in the run. `video/prepare_fade_walk.sh` removed (YouTube PO-token policy made it non-functional; fade_walk is distributed with fetch_asset.sh).
* `analysis/exp_sender_report.py` (2026-09-29): sender + receiver + gNB on one time axis per camera (GoogCC target/RTT/overuse, encoder output, sent/received rate, wrap-aware tx->rx relative one-way delay, UL fail/MCS/grants/BSR/RLC), plus a 1 s zoom around the largest delay spike. Reason: the research question is the coupling between GoogCC decisions and RAN events; this makes the causal sequence visible per run without ad hoc scripts.
* `analysis/plot_run.py <run> [bitrate|fps|delay|all]` (2026-09-29): figures under `<run>/analysis/graphs/`. `bitrate.png`: per UE, encoder target (tx-encoder-rates = VideoEncoder::SetRates, i.e. GoogCC estimate minus packet overhead, capped at encoder max 2500 kbps) over GoogCC's estimated bandwidth (tx-cc target_bps = TargetTransferRate::target_rate). `fps.png`: per UE frames per 1 s bin at capture, encoder output and decoded delivery (gap capture→encoded = encoder-side drops, encoded→decoded = network). stable_target_bps (LinkCapacityTracker) and bwe_delay/bwe_loss sub-estimates are not drawn; see the script header for the M120 source trace. `delay.png`: per-packet one-way delay (rx wall - tx wall, wrap-aware RTP join; absolute because both hosts are chrony-synced to µs, the figure prints each camera's RMS offset and falls back to min-normalised if a laptop was not synced) above per-frame delay (capture -> last packet, capture -> delivered). Needs matplotlib in .venv (analysis/requirements.txt); one fixed hue per camera. Further graphs are added here one at a time.
* `docs/SCENARIO_EDGE_PROFILES.md` (2026-09-29): design note for the target scenario (many lightweight cameras shared by tasks, per-camera fps/resolution profiles issued by the edge, how the RAN learns and schedules them), including the audit of every libwebrtc M120 path that changes fps/resolution/bitrate on its own and the decision "keep libwebrtc as a profile executor (fixed-rate controller via the existing CC injection point), not GoogCC". Reason: the move away from GoogCC-driven senders changes the research question and the sender's role, and needs one place that states the assumptions, the testbed gaps (incl. the rule-2 conflict of a profile-aware scheduler) and the step plan. Design only; no code or run-path change.
* `gstreamer/` tree (2026-09-30): `apps/common/{gst_util,video_source,trace_ring,app_util,control_client}.h`, `apps/{sender,receiver}/video_*.cc`, `apps/control/control_server.py`, `run_*.sh`, `experiments/`, `scripts/{build_apps,local_apps,fetch_gst_examples}.sh`, `gstreamer.lock`, `README.md`. Reason: the fixed-profile sender for docs/SCENARIO_EDGE_PROFILES.md (decision table above). Verified: loopback demo-local (cam0 360 frames = 30.0 fps, cam1 180 = 15.0 fps, 0 loss) and `make run-local` (results/20260930-120754-gst-first, 837/837 frames joined across sender / gNB PDCP / receiver by `rtp_ts`).
* `webrtc/` tree (2026-09-30): the former root-level apps/, run scripts, experiments, libwebrtc build scripts and patches README moved as-is (`git mv`); `scripts/local_apps.sh` extracted from run_local_e2e.sh; `README.md`. Reason: keep the GoogCC stack runnable and separately modifiable without sharing code with the active tree.
* `scripts/run/run_local_e2e.sh --tree`, `scripts/run/ota_restart.sh` (`P5G_TREE`), Makefile `TREE=` (2026-09-30): the shared RAN scripts delegate the app phase to `<tree>/scripts/local_apps.sh` / `<tree>/run_receiver.sh`. Reason: the RAN part is transport-independent and must not know either tree's flags.
* `analysis/exp_run_report.py`, `analysis/plot_run.py` (2026-09-30): read runs of both trees — W3C stats fields only when present, received kbps from the RTP ledger, the media-path check from gNB PDCP rows alone when there is no ICE, per-frame capture time from `tx-frames` by `rtp_ts` when there is no abs-capture-time. Reason: `analysis/` is the shared consumer of the trace contract; it must not require webrtc-only files.
* `gstreamer/apps/tests/rtcp_valid_test.cc` (2026-09-30): 18 crafted RTCP compounds against `IsValidRtcpCompoundFrom`, built and run by `gstreamer/scripts/build_apps.sh` (a wrong verdict fails the build). Reason: the validator decides whether an incoming datagram may change where the receiver sends RTCP; it must reject every malformed/spoofed shape, and that is cheapest to guarantee with a test that runs on every build.
* `docs/reference_code/` (2026-09-30): `fetch.sh` + `reference_code.lock` + README; shallow, pinned, git-ignored clones of the SMEC (edge-applications, controllers, srsRAN fork, prober, edge-manager, measurement, evaluation) and Artic (Artic, DeViBench) repositories, read-only. Reason: the related-work comparisons and the control-plane design (SCENARIO §4) must rest on the actual code (how they stream, what they signal to the RAN), and the code must stay reachable at a known commit; the README holds the code-level findings.
* `ffmpeg/` tree (2026-09-30): SMEC-style transport — pre-encoded Annex B H.264 rungs (`scripts/prepare_video.sh`: MOT17-02 as in SMEC/ARMA, and the Kendo views; x264 AUD per AU, GOP 60, scenecut off, HRD-CBR) sent on the capture grid through libavformat's `rtp` muxer into our AVIO callback / UDP socket; receiver = own socket (`SO_TIMESTAMPNS`) + own RFC 6184 depacketizer + libavcodec; rung switch at IDR on a `profile` message; control protocol copied from gstreamer/. Reason: the RAN experiments need bit-identical frames across runs/cameras/hosts and per-frame sizes known a priori (SCENARIO §3 descriptor, §8 P0/P4), no encoder latency on the laptops, and comparability with SMEC/ARMA/Tutti; the live-encoder tree stays for the GCC condition and camera realism. Verified: loopback demo (361/361/361, capture→app 2.7 ms), rung switch (10417 → 4167 B/frame at the next IDR), `make run-local TREE=ffmpeg` (840/840/840, all frames joined at the gNB by `rtp_ts`). `scripts/prepare_client.sh` prepares a laptop in one command (packages, build, rungs over the sync LAN, chrony).
* `analysis/fusion_report.py <run> [--group id=camA,camB:deadline_ms]` (2026-10-01): multi-camera fusion analysis of
  docs/SCENARIO_EDGE_PROFILES.md §5.6 (items 1-6 and 8: group latency and deadline satisfaction, intra-group spread and
  waiting, frontier lag at the gNB, PRBs given to members ahead of the group frontier, straggler identity/persistence/
  cause, whether gNB observations identify the straggler, IDR placement against same-phase baselines). Groups come from
  the scenario's `groups` (the edge descriptor; later carried by the profile) or `--group`. Reason: the research
  question is group-level (an inference needs frame k from every member), which per-camera reports cannot answer.
  Outputs only under `<run>/analysis/`; `ffmpeg/run_experiment.sh` runs it when the scenario declares groups.
