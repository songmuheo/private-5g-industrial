# private-5g-industrial

Research testbed for **video transmission over a private 5G NR SA network** (srsRAN gNB on a USRP
B210, Open5GS core, Pixel phones as UEs) with cross-layer, real-time tracing: every video frame, every
RTP/RTCP packet, every congestion-control decision and every gNB scheduling / HARQ / RLC / PDCP event
is logged as it happens, with a common time base. All third-party code runs **stock**; the only
changes are logging hooks.

```
UE laptop ── USB ── Pixel ~~~ NR n78 TDD ~~~ B210 ── srsRAN gNB (+tracer) ── Open5GS ── internet ── receiver
video_sender                                        gNB PC                                   video_receiver
                                                                                              signaling relay
```

| part | what | where |
|---|---|---|
| gNB | srsRAN_Project `release_25_10`, native build, UHD | `third_party/srsRAN_Project` (submodule, stock) + `patches/srsran_gnb/` (tracer) + `ran/gnb/` |
| 5G core | Open5GS 2.7.0, srsRAN's docker recipe, on the gNB PC | `ran/core/` |
| video apps | libwebrtc M120 stock (no patches) — sender / receiver with app-side tracing, TCP-JSON signaling relay | `apps/` |
| check | completeness check of a run's real-time logs | `analysis/verify_run.py` |
| code test | srsUE (srsRAN_4G, 5G SA over ZeroMQ) so the whole chain can be exercised on one PC without radios — not part of the measurement testbed | `third_party/srsRAN_4G` + `ran/ue_sim/` + `scripts/run/run_local_e2e.sh` |

Docs: [docs/SETUP.md](docs/SETUP.md) (per-machine commands), [docs/TRACE_SCHEMA.md](docs/TRACE_SCHEMA.md)
(every output file and column), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (design and decisions),
[docs/NOTES.md](docs/NOTES.md) (log).

## Run (one script per terminal / machine)

```bash
./run_gnb_core.sh            # gNB PC: Open5GS + srsRAN gNB (tracer) + JSON metrics   -> results/<ts>-ota/{gnb,core}
./run_receiver.sh            # receiver host: signaling relay + video_receiver          -> results/<run>/app (rx-*)
./run_sender.sh 10.53.1.1    # UE laptop: video_sender (720p30 H264, fixed resolution)  -> results/<ts>-sender/app (tx-*)
```

## Build

```bash
git clone --recurse-submodules --shallow-submodules <repo> && cd private-5g-industrial
make deps            # Ubuntu 22.04 packages
make build-gnb       # gNB + tracer patches  (gNB PC)
make build-libwebrtc # stock libwebrtc M120 (hours) — or symlink an existing checkout to third_party/libwebrtc
make build-apps      # video_sender / video_receiver (copy build/apps/ to the laptops / receiver host)
make build-ue-sim    # code test only: srsUE for the ZeroMQ loopback
make run-local       # code test: core + gNB(zmq) + srsUE + sender(UE netns) -> receiver(host), then verify
```

## What a run produces (`results/<run>/`)

```
gnb/  gnb_sched_ul.csv gnb_sched_dl.csv   per-slot grants per UE (PRB, MCS, TBS, HARQ id, retx)
      gnb_ul_crc.csv gnb_dl_harq_ack.csv  HARQ outcomes (+ PUSCH SINR / TA)
      gnb_bsr.csv gnb_sr.csv gnb_csi.csv  UE buffer status, scheduling requests, CQI/RI
      gnb_mac_ul_pdu.csv gnb_rlc_ul.csv   MAC PDUs, RLC PDU/SDU/reassembly events
      gnb_pdcp_ul.csv gnb_pdcp_dl.csv     IP packets leaving/entering the RAN, RTP header parsed (join key to app ledgers)
      gnb.log gnb_stdout.log gnb_metrics.jsonl   stock srsRAN log, 1 s metrics table, JSON metrics   [+ pcaps with P5G_GNB_PCAP=1]
app/  <stream>-tx-frames.csv -tx-encoded.csv -tx-encoder-rates.csv -tx-cc.csv -tx-rtp.csv -tx-rtcp.csv -tx-events.csv -tx-stats.jsonl   sender
      <stream>-rx-frames.csv -rx-decoded.csv -rx-rtp.csv -rx-rtcp.csv -rx-events.csv -rx-stats.jsonl       receiver
core/ open5gs.log
```

Everything is written in real time by the running processes; nothing is derived afterwards.
`make verify RD=results/<run>` only checks completeness (rows, overflow sidecars, RTP seq loss).

## Layout

```
apps/common/    app_util.h trace_ring.h webrtc_tracing.h webrtc_session.h video_source.h signaling_client.h (+ json.hpp, nlohmann)
apps/sender/ apps/receiver/ apps/signaling/
ran/gnb/        gnb_b210_n78_tdd_20mhz.yml (testbed), gnb_zmq_local.yml (code test), metrics_json_client.py
ran/ue_sim/     srsUE config for the code test
ran/core/       docker-compose.yml, open5gs.env, subscriber_db.csv (git-ignored; keys) / subscriber_db.example.csv
patches/        srsran_gnb/ (tracer header + 6 patches), libwebrtc/ (README: no patches needed)
scripts/build/  build_srsran_gnb.sh build_libwebrtc.sh build_apps.sh
run_gnb_core.sh run_receiver.sh run_sender.sh   (repo root: one per terminal / machine)
scripts/run/    ota_restart.sh (gNB PC one-shot, background) start_core.sh core_add_dnn.sh run_gnb.sh run_ue_sim.sh run_local_e2e.sh (last two: code test)
analysis/       verify_run.py trace_io.py
docker/         libwebrtc build toolchain image
video/          test-sequence download/convert
third_party/    srsRAN_Project, srsRAN_4G (submodules, stock), libwebrtc (fetched; pinned by libwebrtc.lock)
```
