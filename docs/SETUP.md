# Setup and run — three roles, one repository

```
UE laptop ── USB ── Pixel ~~5G NR n78~~ USRP B210 ── gNB PC (srsRAN gNB + tracer, Open5GS) ── internet ── receiver host
video_sender                                          results/<run>/gnb/                                video_receiver + signaling relay
results/<run>/app/ (tx-*)                                                                               results/<run>/app/ (rx-*)
```

All hosts: `git clone --recurse-submodules --shallow-submodules <repo>`. Clock sync and synchronised
starts: see "Clock sync and synchronised start" below.

## Quick path: three scripts, one per terminal / machine

```bash
./run_gnb_core.sh [label]       # gNB PC, terminal 1: core + gNB (foreground) + JSON metrics -> results/<ts>-<label>/{gnb,core}
./run_receiver.sh [-n N]        # gNB PC (or internet host), terminal 2: relay :8765 + N video_receivers (recv0..) -> same run's app/ (via results/CURRENT)
./run_sender.sh 10.53.1.1       # UE laptop, after the phone attached: video_sender (Kendo view K for camK, 720p30, 300 s) -> results/<ts>-sender-<stream>/app
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
every non-default value and its reason in docs/RAN_CONFIG.md).

**Antennas (B210):** one on RF A `TX/RX` (DL transmit) and one on RF A `RX2` (UL receive); RF B unused.
Do not use one antenna on `TX/RX` with `tx_mode: same-port`: the ATR switching makes the PUSCH after
every DL transmission fail (docs/RAN_CONFIG.md). Keep every phone >= 1-2 m from the RX2 antenna so the
received UL powers stay within ~15 dB of each other (near-far).

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
# once per laptop: raw I420 content into video/assets/ (git-ignored). Standard content = Nagoya "Kendo" multi-view
# (7 synchronized cameras of one scene): laptop camK plays kendo_viewK (1280x720 30 fps, 10 s looped, 415 MB).
# run_sender.sh maps --stream-id camK to kendo_viewK automatically; fallbacks: fade_walk > crowd_run > FourPeople > pattern.
video/fetch_asset.sh                                      # all six views (2.5 GB) from the gNB PC over the lab LAN (rsync/scp), ~1-2 min
video/fetch_asset.sh --cam 3                              # only view 3 (the laptop that runs --stream-id cam3)
video/prepare_kendo.sh "0 1 2 3 4 5"                      # or: download the Kendo zip from Nagoya Univ. (1.67 GB) and transcode yourself;
                                                          #     byte-identical to the copies (sha256 checked)
video/fetch_asset.sh fade_walk_1280x720_30fps_300s_i420.yuv   # optional: 300 s continuous footage (12.4 GB) for no-loop runs
video/prepare_fade_walk.sh                                # or: download the same CC-BY clip from YouTube (yt-dlp, ~270 MB) and convert; result is
                                                          #     equivalent but not bit-identical to the copy -> use one method on all laptops.
                                                          #     As of 2026-09-28 YouTube 403s clients without a PO token (see the script header);
                                                          #     fetch_asset.sh is the reliable path.
video/fetch_asset.sh --list                               # what the gNB PC has; P5G_ASSET_HOST=user@host to change the source
video/prepare_test_sequence.sh crowd_run 1280 720        # alternative: download + convert a xiph test sequence (1.5 GB)
video/prepare_test_sequence.sh --from file.y4m NAME 1280 720   # alternative: convert a local y4m
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

## Clock sync and synchronised start

Cross-host columns (`*_wall_ns`, `abs_capture_ntp_ms`) are only comparable to the accuracy of the hosts'
clock sync. chrony over a LAN/Wi-Fi path gives a few ms (tens of us on wired LAN); over the 5G link
itself the UL/DL delay asymmetry biases the offset by up to ~(UL-DL)/2, i.e. 5-20 ms here, so use the
lab LAN / Wi-Fi for NTP and the phone only for the experiment traffic.

gNB PC = NTP server for the lab (it has the fixed LAN address and serves the UE subnet too):

```bash
sudo apt install -y chrony
sudo tee /etc/chrony/conf.d/p5g-server.conf >/dev/null <<'CONF'
allow 10.45.0.0/16        # UE laptops via the 5G path (fallback)
allow 163.152.193.0/24    # lab LAN / Wi-Fi (preferred path)
local stratum 8           # keep serving if the upstream is unreachable
CONF
sudo systemctl restart chrony && chronyc tracking
```

UE laptop (client; a second interface on the lab Wi-Fi/LAN is preferred, the phone stays the default route):

```bash
sudo apt install -y chrony
sudo tee /etc/chrony/conf.d/p5g-client.conf >/dev/null <<'CONF'
server 163.152.193.99 iburst minpoll 3 maxpoll 5 prefer   # gNB PC over LAN/Wi-Fi
server 10.53.1.1 iburst minpoll 4 maxpoll 6               # gNB PC over the 5G path (fallback)
makestep 1 3
CONF
sudo systemctl restart chrony; sleep 30; chronyc tracking; chronyc sources -v
```

`chronyc tracking` "System time" is the residual offset; wait until it is < 1 ms (LAN) before a run. The
run scripts record `chronyc tracking` at start (`gnb/clock.txt`, `app/clock-<stream>.txt`).

Synchronised start: type the same absolute time on every laptop, a minute ahead:

```bash
./run_sender.sh 10.53.1.1 --to recv0 --stream-id cam0 --start-at 18:30:00     # laptop 1
./run_sender.sh 10.53.1.1 --to recv1 --stream-id cam1 --start-at 18:30:00     # laptop 2 ... 5
```

Each script waits until that second (sub-10 ms alignment once chrony reports < 1 ms), then starts
video_sender; the actual start instant is logged in `app/clock-<stream>.txt`. Alternative without typing on
five machines: from the gNB PC, `for h in $LAPTOPS; do ssh $h "cd private-5g-industrial && ./run_sender.sh
10.53.1.1 --to recvK --stream-id camK --start-at $T" & done` (needs SSH access to the laptops over the
LAN). An in-band barrier through the signaling relay (start on "N senders registered") would need a
change in `apps/signaling` + `video_sender`; not implemented.

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
