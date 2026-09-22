# Setup and run — three roles, one repository

```
UE laptop ── USB ── Pixel ~~5G NR n78~~ USRP B210 ── gNB PC (srsRAN gNB + tracer, Open5GS) ── internet ── receiver host
video_sender                                          results/<run>/gnb/                                video_receiver + signaling relay
results/<run>/app/ (tx-*)                                                                               results/<run>/app/ (rx-*)
```

All hosts: `git clone --recurse-submodules --shallow-submodules <repo>`; sync wall clocks with chrony
against one server (cross-host latencies use `*_wall_ns`; record `chronyc tracking` with each run).

## gNB PC (Ubuntu 22.04, UHD installed, docker)

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
`--max-bitrate-kbps`, `--start-bitrate-kbps`, `--low-latency-playout` (receiver), and the logging
knob `--stats-period-ms` (1 s getStats sampling; 0 disables).

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
