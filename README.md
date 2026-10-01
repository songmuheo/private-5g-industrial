# private-5g-industrial

Research testbed for **video transmission over a private 5G NR SA network** (srsRAN gNB on a USRP
B210, Open5GS core, Pixel phones as UEs) with cross-layer, real-time tracing: every video frame, every
RTP/RTCP packet and every gNB scheduling / HARQ / RLC / PDCP event is logged as it happens, with a
common time base. All third-party code runs **stock**; the only changes are logging hooks.

Two transport trees, sharing no code: **`gstreamer/`** (active) — a fixed-profile camera model
(resolution / fps / bitrate set from outside, GStreamer + x264, plain RTP, no congestion control), for the
scenario in `docs/SCENARIO_EDGE_PROFILES.md`; **`webrtc/`** (frozen) — the libwebrtc M120 + GoogCC sender
that produced the runs up to 2026-09-29 (tag `webrtc-baseline-2026-09-30`). They share only the run-directory /
trace-file contract (`docs/TRACE_SCHEMA.md`) and the RAN.

```
UE laptop ── USB ── Pixel ~~~ NR n78 TDD ~~~ B210 ── srsRAN gNB (+tracer) ── Open5GS ── receiver host (gNB PC / internet)
<tree>/video_sender                                 gNB PC                                <tree>/video_receiver + control server
```

| part | what | where |
|---|---|---|
| gNB | srsRAN_Project `release_25_10`, native build, UHD | `third_party/srsRAN_Project` (submodule, stock) + `patches/srsran_gnb/` (tracer) + `ran/gnb/` |
| 5G core | Open5GS 2.7.0, srsRAN's docker recipe, on the gNB PC | `ran/core/` |
| video apps (active) | GStreamer 1.20 (distribution packages) + x264: fixed-profile sender / receiver with app-side tracing, TCP-JSON control channel | `gstreamer/` |
| video apps (frozen) | libwebrtc M120 stock (no patches) + GoogCC — sender / receiver with app-side tracing, TCP-JSON signaling relay | `webrtc/` |
| check | completeness check of a run's real-time logs | `analysis/verify_run.py` |
| code test | srsUE (srsRAN_4G, 5G SA over ZeroMQ) so the whole chain can be exercised on one PC without radios — not part of the measurement testbed | `third_party/srsRAN_4G` + `ran/ue_sim/` + `scripts/run/run_local_e2e.sh` |

Docs: [docs/SETUP.md](docs/SETUP.md) (per-machine commands, clock sync, synchronised start),
[docs/RAN_CONFIG.md](docs/RAN_CONFIG.md) (every gNB setting and the measurement behind it),
[docs/TRACE_SCHEMA.md](docs/TRACE_SCHEMA.md) (every output file and column), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
(design and decisions), [docs/NOTES.md](docs/NOTES.md) (log),
[docs/SCENARIO_EDGE_PROFILES.md](docs/SCENARIO_EDGE_PROFILES.md) (target scenario: edge-issued camera profiles and profile-aware RAN scheduling).

## Run

One command from the gNB PC (after `./run_gnb_core.sh` is up): receivers, preflight of every laptop over the
sync LAN, per-camera settings from a JSON scenario, synchronised start, trace collection, verify and report.

```bash
./gstreamer/run_experiment.sh gstreamer/experiments/5ue-720p30.json   # -> results/<run>/{scenario.json,experiment.json,app,senders/camK}, results/<run>/analysis/report.txt
./gstreamer/run_experiment.sh gstreamer/experiments/demo-local.json   # smoke test on one PC (senders + receivers over loopback, no RAN)
./webrtc/run_experiment.sh webrtc/experiments/2ue-720p30.json         # the frozen tree, same shape (needs webrtc/build/apps)
```

Laptops need a one-time preparation only (docs/SETUP.md "Laptop checklist": clone, sender binary, sync-LAN +
chrony setup, camera asset, SSH key from the gNB PC); after that nothing is typed on them per experiment.

Manual equivalent (one script per terminal / machine):

```bash
./run_gnb_core.sh [label]                              # gNB PC: Open5GS + srsRAN gNB (tracer) + JSON metrics -> results/<ts>-<label>/{gnb,core}
./gstreamer/run_receiver.sh -n 5                       # gNB PC: control server + 5 video_receivers (recv0..4) -> results/CURRENT/app (rx-*)
./gstreamer/run_sender.sh 10.53.1.1 --to recv0 --stream-id cam0 [--start-at 18:30:00]   # each UE laptop: Kendo view K for camK, 720p30 2500 kbps, 300 s -> results/<ts>-sender-cam0/app (tx-*)
```

`--start-at` makes every laptop start at the same wall-clock second (clocks synced with chrony, see
docs/SETUP.md "Clock sync"). After a run, copy each laptop's `results/*-sender-cam*/` next to the gNB PC's
run directory and use `analysis/exp_run_report.py <run> 5` (app + gNB) and `analysis/exp_ran_audit.py <run>`
(RAN health, grants, near-far, capacity).

## RAN configuration

`ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml`, fully explained in [docs/RAN_CONFIG.md](docs/RAN_CONFIG.md).
The short version:

- B210 with **TX on RF A `TX/RX` and RX on RF A `RX2`** (srsRAN's default port mapping, `tx_mode` continuous),
  1 TX / 1 RX, `tx_gain 80` / `rx_gain 40`, 23.04 MS/s sc12. The one-antenna `same-port` mode is not used:
  its TX/RX switching made every PUSCH that followed a DL transmission fail (78 % vs 9 %).
- n78, 20 MHz, 30 kHz, **TDD DDDSU** (2.5 ms; special slot 6 DL / 4 guard / 4 UL symbols).
- UL link adaptation like a production gNB: OLLA target BLER 10 %, SNR-offset range 20 dB, step 0.02 dB
  (srsRAN defaults 1 % / 5 dB / 0.001 dB could not leave MCS 27).
- Everything else is the srsRAN default (256QAM tables, open-loop power control, RLC AM timers, time_qos
  scheduler); the file lists each replaced default and the measurement behind the change.
- Measured baseline (5 UEs): UL CRC failure 7-11 %, SR->grant 3 ms, HARQ completion p99 12-22 ms, 0 RLF /
  0 RF real-time events. Remaining limits: near-far (30 dB spread between UEs), PUCCH margin, grant padding.

## Build

```bash
git clone --recurse-submodules --shallow-submodules <repo> && cd private-5g-industrial
make deps            # Ubuntu 22.04 packages (srsRAN build deps + GStreamer dev/plugins)
make build-gnb       # gNB + tracer patches  (gNB PC)
make build-apps      # gstreamer/build/apps/video_{sender,receiver} (system g++; copy to the laptops / receiver host)
make build-ue-sim    # code test only: srsUE for the ZeroMQ loopback
make run-local       # code test: core + gNB(zmq) + srsUE + sender(UE netns) -> receiver(host), then verify   [TREE=webrtc for the frozen tree]
make webrtc-build-libwebrtc webrtc-build-apps   # frozen tree only: stock libwebrtc M120 (hours; or symlink a checkout to webrtc/libwebrtc)
```

## What a run produces (`results/<run>/`)

```
gnb/  gnb_sched_ul.csv gnb_sched_dl.csv   per-slot grants per UE (PRB, MCS, TBS, HARQ id, retx)
      gnb_ul_crc.csv gnb_dl_harq_ack.csv  HARQ outcomes (+ PUSCH SINR / TA)
      gnb_bsr.csv gnb_sr.csv gnb_csi.csv  UE buffer status, scheduling requests, CQI/RI
      gnb_mac_ul_pdu.csv gnb_rlc_ul.csv   MAC PDUs, RLC PDU/SDU/reassembly events
      gnb_pdcp_ul.csv gnb_pdcp_dl.csv     IP packets leaving/entering the RAN, RTP header parsed (join key to app ledgers)
      gnb.log gnb_stdout.log gnb_metrics.jsonl   stock srsRAN log, 1 s metrics table, JSON metrics   [+ pcaps with P5G_GNB_PCAP=1]
app/  <stream>-tx-frames.csv -tx-encoded.csv -tx-encoder-rates.csv -tx-rtp.csv -tx-rtcp.csv -tx-stats.jsonl   sender  (+ -tx-cc.csv -tx-events.csv: webrtc tree only)
      <stream>-rx-frames.csv -rx-decoded.csv -rx-rtp.csv -rx-rtcp.csv -rx-stats.jsonl                        receiver (+ -rx-events.csv: webrtc tree only)
core/ open5gs.log
```

Everything is written in real time by the running processes; nothing is derived afterwards.
`make verify RD=results/<run>` only checks completeness (rows, overflow sidecars, RTP seq loss).

## Layout

```
gstreamer/      ACTIVE transport tree (README there): apps/{common,sender,receiver,control}, run_{sender,receiver,experiment}.sh,
                experiments/, scripts/{build_apps,local_apps,fetch_gst_examples}.sh, gstreamer.lock; build/ and gstreamer-src/ ignored
webrtc/         FROZEN libwebrtc tree (README there): apps/{common,sender,receiver,signaling}, run_*.sh, experiments/,
                scripts/{build_libwebrtc,build_apps,local_apps}.sh, patches/libwebrtc/, libwebrtc.lock; libwebrtc/ and build/ ignored
ran/gnb/        gnb_b210_n78_tdd_20mhz.yml (testbed), gnb_zmq_local.yml (code test), metrics_json_client.py
ran/ue_sim/     srsUE config for the code test
ran/core/       docker-compose.yml, open5gs.env, subscriber_db.csv (git-ignored; keys) / subscriber_db.example.csv
patches/        srsran_gnb/ (tracer header + 6 patches)
scripts/build/  build_srsran_gnb.sh build_srsran_ue_sim.sh
run_gnb_core.sh (gNB PC, foreground RAN)   — the app scripts live in the transport trees
scripts/run/    ota_restart.sh (gNB PC one-shot, background; P5G_TREE) start_core.sh core_add_dnn.sh run_gnb.sh run_ue_sim.sh
                run_local_e2e.sh (code test; --tree, delegates the app phase to <tree>/scripts/local_apps.sh)
scripts/setup/  sync_lan_server.sh sync_lan_client.sh sync_check.sh (wired clock-sync LAN: chrony + firewall)
analysis/       verify_run.py trace_io.py exp_run_report.py (app + gNB report) exp_ran_audit.py (RAN audit)
docker/         libwebrtc build toolchain image (webrtc tree)
video/          fetch_asset.sh (copy assets from the gNB PC), prepare_kendo.sh (Nagoya multi-view Kendo, one view per camera), prepare_test_sequence.sh (xiph sequences)
third_party/    srsRAN_Project, srsRAN_4G (submodules, stock)
```
