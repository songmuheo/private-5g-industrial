# Reference code of related systems (read-only)

`fetch.sh` clones the repositories below into this directory at the commits pinned in `reference_code.lock`
(shallow, blobless). The clones are git-ignored; only `fetch.sh`, the lock and this file are versioned.
They are never built or run from here — they are read to know how these systems stream and schedule, so
that our comparisons and citations rest on code, not on paper text. Paper-level notes: `docs/RELATED_SYSTEMS.md`.

```bash
docs/reference_code/fetch.sh          # clone / update to the pinned commits
docs/reference_code/fetch.sh --pin    # rewrite the lock from the clones' HEADs (after a deliberate update)
```

| name | repository | pinned | role |
|---|---|---|---|
| `artic` | github.com/pku-netvideo/Artic | 5d8f47a (2026-09-08) | Artic (SIGCOMM'26): ReCapABR (razor-based sender), ZeCoStream (ROI encoding pipeline), prompts |
| `devibench` | github.com/pku-netvideo/DeViBench | cab75d3 | Artic's degraded-video understanding benchmark |
| `smec-edge-applications` | github.com/smec-project/edge-applications | b66409c | SMEC / ARMA / Tutti / default clients and servers (video-od, video-sr, transcoding, file-transfer) |
| `smec-client-prober` | github.com/smec-project/client-prober | 4517d81 | UE-side prober (RTT / probe feedback the request header carries) |
| `smec-edge-manager` | github.com/smec-project/edge-manager | 0923768 | GPU/CPU compute scheduler at the edge |
| `smec-measurement`, `smec-auto-evaluation` | github.com/smec-project/… | ab61082, ef885cf | measurement and evaluation scripts |
| `smec-srsRAN_Project` | github.com/smec-project/srsRAN_Project | 5b94d2b8a (2026-09-13) | srsRAN fork with the SMEC / ARMA / Tutti UL scheduler policies + the three edge controllers |

## How they stream (code-level, 2026-09-30)

### SMEC / ARMA / Tutti (`smec-edge-applications/<policy>/video-od/client/src/streamer.cpp`)

One program for all three policies. **No encoder in the loop**: FFmpeg `libavformat` reads a pre-encoded H.264 file
(`avformat_open_input`), converts to Annex B (`h264_mp4toannexb`) and remuxes each packet to an `rtp://host:port`
output context (`avformat_alloc_output_context2(..., "rtp", ...)`, `pkt_size 1316`, `max_interleave_delta 0`,
`flush_packets 1`), sleeping until the packet's PTS (`av_usleep(pts_time - now)`). So resolution, fps, bitrate and
GOP are those of the file; nothing adapts. No congestion control, no pacer (a frame leaves as one RTP burst), no
RTCP handling on the client, no NACK/RTX. The SDP is written to `stream-info/rtp_uplink.sdp` for the receiver
(FFmpeg RTP demux on the server, `video-od/server/main.cpp`, 10 ports 19000-19090, 100 ms SLO registered with the
edge scheduler, frames dropped on its signal). "Dynamic mode" (`-d`) is an on/off pattern (random 0-4 s pauses).

What rides with the video (the application ↔ RAN control plane):
* **per-frame request header inside the H.264 stream** as an SEI (`sei_handler.cpp`, `AddCustomPayloadToPacket`):
  `{client_key, request_id, probe_id, send_timestamp}` filled by `lib/client` `FillAndReportRequest()`, which also
  reports the request to the local prober over UDP (`ReportRequest`), plus a separate RTP packet carrying the send
  timestamp (`CreateTimestampPacket`). The server reads the SEI, so the edge knows request id + send time per frame
  without a side channel on the media path.
* **ARMA only**: one UDP datagram per frame to the RAN `{ue_rnti, request_index, frame_size, timestamp_us}`
  (`arma/.../streamer.cpp:315-334`) — the "per-frame notify".
* **Edge → gNB priority**: the controllers (`smec-srsRAN_Project/{smec,arma,tutti}_controller/`, Python) send
  `struct {uint32 rnti; double priority; uint8 is_reset}` over **UDP to port 5555** of the gNB
  (`network_handler.py:219`); the gNB's `scheduler_time_pf` runs a socket thread (`handle_priority_messages`,
  `scheduler_time_pf.cpp:750`) that stores `ul_priorities[rnti]`.
* **gNB → edge metrics**: `lib/scheduler/ue_scheduling/scheduler_metrics_sender.cpp` sends binary UDP messages
  `[type][rnti][field1][field2]` (PRB allocations, BSR, ...) to 127.0.0.1 (`metrics_processor.py:56`).
* **SMEC priority rule** (`priority_manager.py:290-305`): per UE, the oldest outstanding request's remaining time
  `r` (ms, from the registered SLO and the request timestamps) gives `priority = 1 / (r² + ε)` while `r > 0`, and a
  huge value (`1/ε − r`) once the deadline has passed; requests are retired by watching the UE's **BSR decrease**
  (`update_bsr_state`), i.e. the gNB's BSR is the only signal of how much of a frame has left the UE.
* **In the scheduler** (`scheduler_time_pf.cpp:520-641`, `compute_ul_prio`): `"smec"`: `ul_prio = deadline_priority`
  if one exists else the PF metric; `"tutti"`: `ul_prio = pf_weight + deadline_priority`; `"arma"`: the UE with the
  largest `priority × estimated_rate` ("VA metric") is served first (`ul_sched`, lines 280-330), SR-pending UEs
  before that. Nothing else changes: still dynamic grants driven by SR/BSR, no configured grants, no pre-grants.

Take-away for our design (docs/SCENARIO_EDGE_PROFILES.md §4-5): SMEC's path (c) is exactly "priority per UE
computed at the edge from per-request deadlines, pushed by UDP into the PF metric", with the BSR as the RAN's only
view of the stream. It carries no periodicity, no burst size (except ARMA's per-frame notify), no group.

### Artic (`artic/`)

* The production prototype (paper) used the commercial Agora SDK; **the release is a simulator**:
  `ReCapABR/` = a copy of **razor** (yuanrongxi/razor, a C port of WebRTC's congestion control: GCC delay/loss
  estimators, BBR, pacer `pacing/pace_sender.c`, FEC `sim_transport/sim_fec.c`, NACK, over UDP through
  `sim_transport`). `sim_test/sim_sender/sim_sender_test.c` sends **synthetic frames**: every `1000/frame_rate` ms
  a frame of `final_bitrate/8 × interval` bytes (header = size, index, timestamp; keyframe flag every 4 s) via
  `sim_send_video()`; the receiver (`sim_receiver_test.c`) reads frames back and logs timestamps. No codec, no
  resolution; **fps fixed**, bitrate = min(CC suggestion, confidence cap) with `ARTIC/SET_MARGIN_BW/DO_CAP_BITRATE`
  compile-time modes.
* **ReCapABR rule** (`confidence.cpp AdjustBitrate`): from an offline table `confidence.csv` (model confidence per
  frame at 200/400/610/910/1710/3150 kbps): if the current bitrate is above the CC's suggestion → cap to CC; if the
  model's confidence at the current bitrate ≥ threshold (0.8) → **keep, do not increase** even though the CC would;
  otherwise step to the lowest tier whose confidence clears the threshold. I.e. the *consumer's* accuracy saturation
  bounds the bitrate — a profile decided by the model, applied on top of GCC/BBR.
* **ZeCoStream** (`ZeCoStream/*.py`): offline pipeline — sample frames, get the model's ROI boxes, convert to Kvazaar
  ROI maps, encode HEVC with per-region QP (`gen_kvazaar_roi.py`, `batch_process_robust.py`). Not a live streamer.
* Evaluation: Mahimahi replay of 5G uplink traces (paper); nothing RAN-aware.

Take-away: Artic's transport is WebRTC-shaped (GCC/BBR + pacer + FEC/NACK) with fps fixed and a consumer-side cap;
it maps onto our `--cc gcc` condition plus an upper bound from the edge (`gcc_max_kbps`), which is how a
"model-saturation cap" would be expressed in this testbed.
