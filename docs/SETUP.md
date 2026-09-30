# Setup and run — three roles, one repository

> **Transport trees (2026-09-30).** Commands below use the active tree `gstreamer/` (fixed-profile sender:
> `--bitrate-kbps`, `--gop`; no `--degradation`/`--start-bitrate-kbps`). The frozen `webrtc/` tree has the same
> scripts under `webrtc/` (`webrtc/run_sender.sh 10.53.1.1 ...`, `make webrtc-build-apps`, `make run-local TREE=webrtc`);
> the sections on start bitrate, degradation and ICE apply to that tree only. Both trees share `results/`,
> `video/assets`, the sync LAN and `analysis/`.

```
UE laptop ── USB ── Pixel ~~5G NR n78~~ USRP B210 ── gNB PC (srsRAN gNB + tracer, Open5GS) ── internet ── receiver host
video_sender                                          results/<run>/gnb/                                video_receiver + signaling relay
results/<run>/app/ (tx-*)                                                                               results/<run>/app/ (rx-*)
```

All hosts: `git clone --recurse-submodules --shallow-submodules <repo>`. Clock sync and synchronised
starts: see "Clock sync and synchronised start" below.

## Laptop checklist (once per laptop, at the laptop)

K = the camera id this laptop plays (`cam0` -> K=0). Do these in order, in a terminal on the laptop.

```bash
# 0. cables: USB to the Pixel (tethering on), Ethernet to the sync switch (ipTIME H6008)
# 1. code + binaries
git clone https://github.com/songmuheo/private-5g-industrial.git && cd private-5g-industrial      # or: git pull
mkdir -p gstreamer/build/apps && scp songmu@192.168.77.1:private-5g-industrial/gstreamer/build/apps/video_sender build/apps/  # the sender binary (built on the gNB PC)
# 2. clock-sync LAN + chrony (asks for sudo; must be run here, not over SSH)
scripts/setup/sync_lan_client.sh K                 # wired port auto-detected -> 192.168.77.1K, chrony -> gNB PC
# 3. video asset for this camera (415 MB from the gNB PC over the sync LAN)
P5G_ASSET_HOST=songmu@192.168.77.1 video/fetch_asset.sh --cam K
# 4. checks
scripts/setup/sync_check.sh 0.2                    # GO (give chrony 1-2 minutes after step 2)
ip route show default                              # exactly one line, via the phone tether
```

Then, from the gNB PC, once per laptop: `ssh-copy-id songmu@192.168.77.1K` and `ssh songmu@192.168.77.1K true`.

After that a laptop needs nothing per experiment: phone attached, laptop on and awake, cable in. Everything
else is driven from the gNB PC by run_experiment.sh. If the gNB PC's gstreamer/build/apps/video_sender changes (rebuild),
repeat step 1's scp. If a laptop changes camera id, repeat steps 2-3 with the new K (and update the scenario).

## One command: run_experiment.sh

```bash
./run_gnb_core.sh 5ue                              # terminal 1 (stays in the foreground)
./gstreamer/run_experiment.sh gstreamer/experiments/5ue-720p30.json    # terminal 2: everything else
```

The scenario JSON holds per-camera width / height / fps / start_kbps / max_kbps / source, the hosts
(`user@192.168.77.1{K}` over the sync LAN), the sync bound and the timing. The orchestrator copies it to
`<run>/scenario.json`, writes `<run>/experiment.json` (resolved start time, git HEAD, host status), starts the
receivers, checks each laptop (repo HEAD, asset, chrony offset), launches every sender with `--start-at T`
(T = now + start_delay_s), pulls the sender traces into `<run>/senders/camK/app`, stops the receivers and runs
verify + report. Requirements: SSH keys to the laptops (`ssh-copy-id songmu@192.168.77.1K`), laptops pulled and
with their asset (`video/fetch_asset.sh --cam K`). `gstreamer/experiments/demo-local.json` runs the whole flow on one PC
over loopback (no RAN) as a smoke test.

## Manual path: three scripts, one per terminal / machine

```bash
./run_gnb_core.sh [label]       # gNB PC, terminal 1: core + gNB (foreground) + JSON metrics -> results/<ts>-<label>/{gnb,core}
./gstreamer/run_receiver.sh [-n N]        # gNB PC (or internet host), terminal 2: relay :8765 + N video_receivers (recv0..) -> same run's app/ (via results/CURRENT)
./gstreamer/run_sender.sh 10.53.1.1       # UE laptop, after the phone attached: video_sender (Kendo view K for camK, 720p30, 300 s) -> results/<ts>-sender-<stream>/app
```

Each script owns its run directory, so nothing depends on shell variables shared between terminals
(a `--trace-dir $RD/app` typed where `RD` is unset becomes `/app`; the apps now abort instead of
running without traces). Ctrl-C in terminal 1 flushes the gNB traces, saves the core log and takes
the core down. `make ota` / `make ota-stop` remain as the all-in-one background variant
(`scripts/run/ota_restart.sh`).

## Several UEs at once

One relay, one `video_receiver` **per UE**, one stream id per UE. One script starts all of them:

```bash
./gstreamer/run_receiver.sh -n 3          # relay + recv0, recv1, recv2 in one terminal; Ctrl-C stops all (traces flush first)
```

Their output is shown live (tail of `app/receiver-recvK.log`). Extra flags after `-n N` go to every
receiver. (Separate terminals with `--receiver-id recvK` still work; the relay is reused.)

On each UE laptop, point the sender at its own receiver and give the stream a distinct name:

```bash
./gstreamer/run_sender.sh 10.53.1.1 --to recv1 --stream-id cam1      # -> results/<ts>-sender-cam1/app (cam1-tx-*, sender-cam1.log)
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
python3 gstreamer/apps/control/control_server.py --host 0.0.0.0 --port 8765 &          # open TCP 8765 inbound
gstreamer/build/apps/video_receiver --control-host 127.0.0.1 --control-port 8765 --session s1 \
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
video/fetch_asset.sh --list                               # what the gNB PC has; P5G_ASSET_HOST=user@host to change the source
video/prepare_test_sequence.sh crowd_run 1280 720        # alternative: download + convert a xiph test sequence (1.5 GB)
video/prepare_test_sequence.sh --from file.y4m NAME 1280 720   # alternative: convert a local y4m
gstreamer/build/apps/video_sender --control-host <receiver-public-ip> --control-port 8765 --session s1 \
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

Cross-host columns (`*_wall_ns`, `abs_capture_ntp_ms`) are only comparable to the accuracy of the hosts' clock
sync. Do **not** sync over the 5G link: NTP assumes symmetric delay and our UL is 5-30 ms slower than the DL,
so the offset would be biased by half of that and would drift with the load. Use a separate wired LAN.

**Sync LAN (recommended, sub-ms):** an unmanaged gigabit switch (e.g. ipTIME H6008) with the gNB PC's spare
onboard port `enp4s0` and each laptop's wired port (onboard RJ45 or a USB Ethernet adapter). No router, no DHCP:
static addresses 192.168.77.1 (gNB PC) and 192.168.77.1K (laptop camK). chrony over such a LAN gives tens to a
few hundred us (software timestamps; none of our NICs has a PTP hardware clock). Wi-Fi to the same switch/AP
works too but expect 0.5-2 ms.

```bash
# gNB PC (once; re-run after reboot for the firewall part)
scripts/setup/sync_lan_server.sh                 # enp4s0 = 192.168.77.1/24, chrony server, firewall on that port
# laptops: nothing separate. run_sender.sh does it at start (P5G_SYNC=auto): the first time it calls
#   scripts/setup/sync_lan_client.sh <K> (K from --stream-id camK; auto-detects the wired port, 192.168.77.1K,
#   installs chrony -> gNB PC; asks for sudo once), every time it runs scripts/setup/sync_check.sh and prints GO/NO-GO.
#   NO-GO aborts only with --start-at. P5G_SYNC_MAX_MS=0.2 tightens the bound on wired; P5G_SYNC=off disables.
```

Why the firewall: the receiver (libwebrtc) gathers ICE host candidates on every interface of the gNB PC, and
the laptops can reach the sync-LAN address directly, so ICE would select that low-RTT path and the video would
leave the 5G link. `sync_lan_server.sh` admits only NTP (UDP 123) and SSH on `enp4s0`. After a run,
`analysis/exp_run_report.py` section "A2. ICE path" shows the selected candidate pair per stream; it must be
`10.53.1.1 <- 10.45.x`. The apps have no interface-selection option, so this check is the guarantee.

The laptops' default route must stay on the phone tether (the sync connection is created with
`ipv4.never-default yes`; `ip route show default` must list only the tether). The run scripts record
`chronyc tracking` at start (`gnb/clock.txt`, `app/clock-<stream>.txt`), so every run carries its own bound.

Synchronised start: type the same absolute time on every laptop, a minute ahead:

```bash
./gstreamer/run_sender.sh 10.53.1.1 --to recv0 --stream-id cam0 --start-at 18:30:00     # laptop 1
./gstreamer/run_sender.sh 10.53.1.1 --to recv1 --stream-id cam1 --start-at 18:30:00     # laptop 2 ... 5
```

Each script waits until that second (sub-10 ms alignment once chrony reports < 1 ms), then starts
video_sender; the actual start instant is logged in `app/clock-<stream>.txt`. Alternative without typing on
five machines: from the gNB PC over the sync LAN, `for K in 0 1 2 3 4; do ssh 192.168.77.1$K "cd
private-5g-industrial/gstreamer && ./run_sender.sh 10.53.1.1 --to recv$K --stream-id cam$K --start-at $T" & done`.

## Teardown

```bash
pkill -TERM -x video_sender;  pkill -TERM -x video_receiver     # flush traces; then stop the gNB
```
All logs are continuous; note the measurement window (wall-clock start/end) in your run notes or
`run.json` and cut the window when analysing.

Copy `app/` from both hosts and `gnb/`, `core/` from the gNB PC into one `results/<run>/` and run
`make verify RD=results/<run>` (completeness check only). Downlink experiments swap the two apps.

## Building the apps (any Ubuntu 22.04 machine)

Active tree: `make deps` (adds the GStreamer 1.20 dev packages and plugins) then `make build-apps` →
`gstreamer/build/apps/video_{sender,receiver}` (system g++, dynamically linked against the distribution's
GStreamer; every host needs the same packages, which the preflight of `run_experiment.sh` should be extended to
check). `gstreamer/build/apps/BUILD_INFO.txt` records core/plugin/libx264 versions; the sender's `config:` line
repeats them in every run.

Frozen tree: `make webrtc-build-libwebrtc` fetches/builds stock libwebrtc M120 into `webrtc/libwebrtc` (hours),
or symlink an existing pristine checkout there; `make webrtc-build-apps` → `webrtc/build/apps/` (static libc++).

## Code test without radios (one PC)

`make build-ue-sim && make run-local DURATION=30` runs Open5GS, the gNB with the ZeroMQ profile
(`ran/gnb/configs/gnb_zmq_local.yml`, band 3 FDD 15 kHz — srsUE's only numerology), srsUE in netns
`ue1`, the sender inside the UE namespace and the receiver on the host, then `verify_run.py`. Same
binaries and scripts as the testbed; only the radio and the UE differ. Results are for checking that
the code and logging work, not for measurements.
