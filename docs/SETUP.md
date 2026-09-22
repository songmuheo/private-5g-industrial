# Setup and run — three roles, one repository

```
UE laptop ── USB ── Pixel ~~5G NR n78~~ USRP B210 ── gNB PC (srsRAN gNB + tracer, Open5GS) ── internet ── receiver host
video_sender                                          results/<run>/gnb/                                video_receiver + signaling relay
results/<run>/app/ (tx-*)                                                                               results/<run>/app/ (rx-*)
```

All hosts: `git clone --recurse-submodules --shallow-submodules <repo>`; sync wall clocks with chrony
against one server (cross-host latencies use `*_wall_ns`; record `chronyc tracking` with each run).

## Quick path: three scripts, one per terminal / machine

```bash
./run_gnb_core.sh [label]       # gNB PC, terminal 1: core + gNB (foreground) + JSON metrics -> results/<ts>-<label>/{gnb,core}
./run_receiver.sh [-n N]        # gNB PC (or internet host), terminal 2: relay :8765 + N video_receivers (recv0..) -> same run's app/ (via results/CURRENT)
./run_sender.sh 10.53.1.1       # UE laptop, after the phone attached: video_sender -> results/<ts>-sender-<stream>/app
```

Each script owns its run directory, so nothing depends on shell variables shared between terminals
(a `--trace-dir $RD/app` typed where `RD` is unset becomes `/app`; the apps now abort instead of
running without traces). Ctrl-C in terminal 1 flushes the gNB traces, saves the core log and takes
the core down. `make ota` / `make ota-stop` remain as the all-in-one background variant
(`scripts/run/ota_restart.sh`).

## Several UEs at once

One relay, one `video_receiver` **per UE**, one stream id per UE. One script starts all of them:

```bash
./run_receiver.sh -n 3          # relay + recv0, recv1, recv2 in one terminal; Ctrl-C stops all (traces flush first)
```

Their output is shown live (tail of `app/receiver-recvK.log`). Extra flags after `-n N` go to every
receiver. (Separate terminals with `--receiver-id recvK` still work; the relay is reused.)

On each UE laptop, point the sender at its own receiver and give the stream a distinct name:

```bash
./run_sender.sh 10.53.1.1 --to recv1 --stream-id cam1      # -> results/<ts>-sender-cam1/app (cam1-tx-*, sender-cam1.log)
```

A second sender reusing a stream id is refused by the receiver ("duplicate offer for stream ... ignored")
and flagged by the relay ("re-registered while another connection holds that stream id").

Why one process per UE: the decoder-factory wrapper that writes `-rx-decoded.csv` cannot tell which
PeerConnection a decoder belongs to, so a receiver refuses a second stream (logged as "offer for
stream ... refused"). Everything else is multiplexed by the relay. All receivers write into the same
run's `app/`, one file set per stream (`cam0-rx-*`, `cam1-rx-*`, ...), so per-UE analysis is a prefix.

Per-UE attribution in the gNB traces: `gnb_sched_*`, `gnb_ul_crc`, `gnb_dl_harq_ack`, `gnb_bsr`,
`gnb_sr`, `gnb_csi`, `gnb_mac_ul_pdu` carry `ue_index` and `rnti`; `gnb_rlc_ul` and `gnb_pdcp_*` carry
`ue_index`, and the PDCP rows also carry the UE IP (`src_ip` / `dst_ip`, static per SIM in
`subscriber_db.csv`) and the RTP `ssrc`. Use `ue_index` to join RLC/PDCP rows with the MAC/scheduler
rows, the UE IP to map to a phone, and the `ssrc` to map to the app ledgers. `rnti` and `ue_index`
change when a UE re-attaches, the static IP does not. `verify_run.py` already prints grants per RNTI.

## gNB PC (Ubuntu 22.04, UHD installed, docker) — step by step


```bash
make deps && make build-gnb                 # ~15 min; needs third_party/srsRAN_Project (make submodules)
# SIMs: one row per phone in ran/core/subscriber_db.csv (git-ignored; template subscriber_db.example.csv):
#       IMSI 00101<MSIN>, K, OPc, AMF 8000, 5QI 9, static IP. Phones keep their existing APN profile:
#       start_core.sh adds the DNNs in P5G_UE_DNNS (default "oai") to every subscription.
make core-up                                # Open5GS at 10.53.1.2, WebUI :9999, route 10.45.0.0/16 via core
RD=results/$(date +%Y%m%d-%H%M%S)-<label>; mkdir -p $RD/gnb $RD/core
scripts/run/run_gnb.sh b210_n78_tdd_20mhz $RD/gnb | tee $RD/gnb/gnb_stdout.log     # tracer -> gnb_*.csv
.venv/bin/python ran/gnb/metrics_json_client.py --out $RD/gnb/gnb_metrics.jsonl &   # official JSON metrics
# ... run ...  Ctrl-C the gNB (traces flush on SIGTERM), then:
docker logs p5g_open5gs > $RD/core/open5gs.log
```

Options: `P5G_GNB_PCAP=1 scripts/run/run_gnb.sh ...` also writes stock MAC / NGAP / N3 GTP-U pcaps
(extra CPU/disk load on the gNB host, off by default). `P5G_GNB_TRACE_FLUSH_MS` (default 500).
Profile: `ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml` (from srsRAN's `gnb_rf_b200_tdd_n78_20mhz.yml`;
gains / `clock: external` per hardware).

## Internet host (receiver + signaling relay)

```bash
sudo apt install -y libx11-6 libxext6 libxdamage1 libxfixes3 libxcomposite1 libxrandr2 libxtst6 python3
python3 apps/signaling/signaling_server.py --host 0.0.0.0 --port 8765 &          # open TCP 8765 inbound
build/apps/video_receiver --signaling-host 127.0.0.1 --signaling-port 8765 --session s1 \
    --receiver-id recv0 --trace-dir $RD/app [--ice-servers stun:<stun-host>:3478]
```

UDP from the internet must reach this host for RTP (open the ephemeral UDP range, or run your own
STUN server and pass it to both sides).

## UE laptop (sender, USB-tethered to a Pixel registered on the cell)

```bash
video/prepare_test_sequence.sh crowd_run 1280 720        # once: raw I420 content into video/assets/
build/apps/video_sender --signaling-host <receiver-public-ip> --signaling-port 8765 --session s1 \
    --stream-id cam0 --to recv0 --trace-dir $RD/app \
    --yuv video/assets/crowd_run_1280x720_30fps_i420.yuv --width 1280 --height 720 --fps 30 \
    --codec H264 [--ice-servers stun:<stun-host>:3478] [--duration 90]
```

Defaults are stock libwebrtc behaviour. The only deviations are explicit flags: `--degradation`,
`--max-bitrate-kbps`, `--start-bitrate-kbps`, `--abs-capture-time 0`, `--low-latency-playout`
(receiver), and the logging knob `--stats-period-ms` (1 s getStats sampling; 0 disables).

`--degradation` sets the W3C `RTCDegradationPreference` on the video sender (`RtpParameters.
degradation_preference`): `stock` (BALANCED: resolution and frame rate both adapt to the estimate),
`maintain_resolution` (resolution fixed at `--width x --height`, frame rate adapts),
`maintain_framerate`, `disabled` (neither adapts; the encoder only raises QP and drops frames when the
target bitrate is too low). Verified on loopback: `stock` encoded 320x180 .. 640x360 during the first
12 s, `maintain_resolution` and `disabled` stayed at 1280x720 for every frame.

`--start-bitrate-kbps N|auto|stock` sets GoogCC's initial rate (`PeerConnection::SetBitrate`, W3C-less
libwebrtc API `BitrateSettings.start_bitrate_bps`). Stock is 300 kbps for every resolution
(`api/transport/bitrate_settings.h`); with BALANCED degradation libwebrtc hides that by encoding the
first seconds at a lower resolution, so once the resolution is pinned the start rate must be set.
`auto` derives it from libwebrtc's own rules instead of a guess (the derivation is printed to sender.log):

| step | rule (M120 source) | 1280x720 H264 30 fps |
|---|---|---|
| base | `ResolutionBitrateLimits::min_start_bitrate_bps`, "recommended minimum bitrate to start encoding", default table per codec family (`rtc_base/experiments/encoder_info_settings.cc`), smallest row covering the pixel count (`video_encoder.cc GetEncoderBitrateLimitsForResolution`) | 900 kbps |
| fps | table assumes 30 fps; scaled by fps/30 = constant bits per pixel per frame (first-order approximation) | x1 |
| floor | `VideoStreamEncoder::DropDueToSize`: frames are discarded while target < 500 kbps above 640x480 (< 300 kbps above 320x240) | 500 kbps |
| cap | `--max-bitrate-kbps`, else `GetMaxDefaultVideoBitrateKbps` (600/1700/2000/2500 kbps by size class) | 2500 kbps |

Table rows (min_start / max, kbps): H264+VP8 270p 200/500, 360p 300/800, 540p 500/1500, 720p 900/2500;
VP9 120/300, 190/420, 350/1000, 480/1500; AV1 176/384, 256/512, 384/1024, 576/1536. Above 720p the 720p
row is extrapolated by pixel ratio (our extension). Note that for a single H264 stream libwebrtc does not
apply this table at run time (OpenH264 reports no limits; defaults apply to simulcast only), which is why
the value has to come from the command line.

Measured effect on loopback (720p30 H264, `maintain_resolution`, first second after the first encoded frame):
stock 300 kbps start -> 16 frames dropped by the encoder, mean QP 34, 620 kbps encoded;
`auto` (900 kbps) -> 2 frames dropped, mean QP 24, 1.37 Mbps encoded; both converge to QP 13 / 2.4 Mbps by 8 s.
For a bandwidth-limited uplink also set `--max-bitrate-kbps` below the measured UL capacity
(`gnb_metrics.jsonl` ul_brate, `gnb_sched_ul.csv` tbs_bytes) so `auto`'s cap follows the link, not the table.

## Teardown

```bash
pkill -TERM -x video_sender;  pkill -TERM -x video_receiver     # flush traces; then stop the gNB
```
All logs are continuous; note the measurement window (wall-clock start/end) in your run notes or
`run.json` and cut the window when analysing.

Copy `app/` from both hosts and `gnb/`, `core/` from the gNB PC into one `results/<run>/` and run
`make verify RD=results/<run>` (completeness check only). Downlink experiments swap the two apps.

## Building the apps (any Ubuntu 22.04+ machine with the libwebrtc checkout)

`make build-libwebrtc` fetches/builds stock libwebrtc M120 into `third_party/libwebrtc` (hours), or
symlink an existing pristine checkout there. `make build-apps` then produces `build/apps/video_sender`
and `video_receiver` (static libc++, need only glibc ≥ 2.35 + X11 client libs); copy them to the other
hosts.

## Code test without radios (one PC)

`make build-ue-sim && make run-local DURATION=30` runs Open5GS, the gNB with the ZeroMQ profile
(`ran/gnb/configs/gnb_zmq_local.yml`, band 3 FDD 15 kHz — srsUE's only numerology), srsUE in netns
`ue1`, the sender inside the UE namespace and the receiver on the host, then `verify_run.py`. Same
binaries and scripts as the testbed; only the radio and the UE differ. Results are for checking that
the code and logging work, not for measurements.
