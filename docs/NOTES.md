# Engineering / experiment log (append-only)

## 2026-09-21 — bring-up and validation of the logging pipeline (no radio yet)

**Setup used for validation (kept in the repo as the code-test path, `make run-local`).** The chain was
exercised on one PC with srsUE (srsRAN_4G 25.10, 5G SA over ZeroMQ, band 3 FDD 15 kHz) in place of the Pixel/B210 link:
Open5GS → srsRAN gNB (tracer) → srsUE → `video_sender` in the UE netns → `video_receiver` on the host.
Final validation run: `results/20260921-200856-verify` (30 s window, 720p H.264, synthetic pattern).

**Result.** All files present, no ring overflow, `verify_run.py` PASS.

| item | value |
|---|---|
| RTP packets sent / received | 7502 / 7502 (0 lost), RTX SSRC 21/21 |
| frames captured / encoded / decoded | 1127 / 1123 / 1122 (4 dropped by the encoder at start-up, 1 cut at shutdown) |
| gNB rows | sched_ul 12261, sched_dl 2509, ul_crc 12261 (BLER 1.1 %), bsr 5123, sr 397, csi 2430, rlc_ul 25879, pdcp_ul 7772 |
| events | tx: bwe_delay 434, bwe_loss 95, probe_created 7, probe_success 16, ice_pair_config 21; rx: 112 |
| getStats samples | tx 37, rx 40 lines; gnb_metrics.jsonl 136 pushes; open5gs.log 280 lines |

Observations (stock behaviour, useful to know when reading traces):
* libwebrtc starts at 300 kbps: the encoder drops the first frames and the resolution goes
  1280→320 within 4 frames, then ramps back to 720p over ~20 s (`tx-encoded.csv` width/height,
  `tx-stats.jsonl` `qualityLimitationReason=bandwidth`).
* e2e ≈ RAN delay (UL grant cycle) + stock jitter buffer (~20 ms, `inbound-rtp.jitterBufferDelay`);
  tail = HARQ retransmissions (`gnb_sched_ul.new_data=0`).
* `frame_decoded` RtcEventLog events are not emitted by M120 (0 rows); `rx-frames.csv` covers it.
* `ul_rsrp_dbfs` = 0 and PUSCH-carried HARQ-ACK `pucch_sinr_db` = NaN in the ZMQ PHY.

**Changes made after review.**
* Removed everything not needed for the USRP/Pixel scenario (offline join / post-processing, runtime
  image, loopback-apps script, duplicate docs). srsUE + ZMQ profile + `run_local_e2e.sh` are kept as the
  code-test path only.
* Tracer rings changed from whole-run linear buffers (≈2.6 GB resident in the gNB) to fixed circular
  rings (~6 MB per trace) flushed every 500 ms; overflow still counted.
* Added official sources: `RtcEventLog` event types (GoogCC/ICE/DTLS) → `-events.csv`; periodic
  W3C getStats → `-stats.jsonl`; srsRAN JSON metrics (remote control) → `gnb_metrics.jsonl`;
  Open5GS log; stock gNB pcaps opt-in (`P5G_GNB_PCAP=1`, off by default to keep the gNB host pure).

**Later the same day.** Merged `apps/common/` into 6 headers (no behaviour change). Added
`-rx-decoded.csv` (decoder-factory wrapper: jitter-buffer exit / decode done / QP per frame). Removed
the SIGUSR1/2 getStats snapshots and `tx-source.txt` (redundant with `-stats.jsonl` and `-tx-frames.csv`).
Re-validated: `results/20260921-205116-decoded` PASS (rx-decoded 824 rows = rx-frames 824).

**Open.** Cross-host clock-sync procedure not yet exercised.

## 2026-09-21 — first over-the-air runs (B210, Pixel phones)

* USB 2.0 link: immediate `RF overflow` / `PRACH late` every slot at 20 MHz — unusable. USB 3.0: 0 RF
  failures over >10 min. `ru_sdr.expert_cfg.tx_mode: same-port` (RF A TX/RX only) works.
* Pixels attach: PUSCH SINR 28–36 dB, CQI 13–15. SIM IMSIs are `00101<MSIN>` (not the 99970 prefix of
  the SIM vendor CSV); SIM …187860 has a different OPc than the vendor CSV (taken from the old OAI DB).
* Phones' APN profile "OAI-SA" = DNN `oai` → Open5GS rejected PDU sessions (`DNN Not Supported`) until
  the DNN was added to the subscriptions (`scripts/run/core_add_dnn.sh`, now run by `start_core.sh`).

## 2026-09-22 — UE-side "no ping reply" investigation
* Capture on the core bridge while a tethered laptop pinged: UE traffic (10.45.1.11/.12) did reach the
  host, but only DNS to 8.8.8.8 — no ICMP toward 10.53.1.1 at all. So the RAN/UPF path is fine and the
  missing pieces were (a) no internet NAT for the UE subnet on the gNB host and (b) most likely the
  laptop routing the ping over Wi-Fi instead of the tethered link (check with `ip route get 10.53.1.1`).
* Fix for (a): `start_core.sh` now enables ip_forward and adds
  `iptables -t nat -A POSTROUTING -s 10.45.0.0/16 -o <default NIC> -j MASQUERADE` (removed on `down`).
  Docker's own FORWARD rules (`-i br-<p5g_ran> -j ACCEPT`, conntrack ESTABLISHED back) already permit it.
* Added `-tx-encoder-rates.csv` (VideoEncoder::SetRates hook in the existing encoder wrapper): the
  target bitrate the stock rate controller hands the encoder, per change instead of the 1 s getStats
  sample. Loopback check (15 s, 720p H.264): 27 rows; start 282 kbps target / 235 kbps allocated,
  ramp to 2.0 Mbps (default max) while `bandwidth_allocation_bps` reached 3.79 Mbps — i.e. the
  encoder, not the network estimate, was the limit on loopback. OTA gNB/core/receiver untouched.
* Added `-tx-cc.csv`: GoogCC's final output per `NetworkControlUpdate` (target/stable rate, RTT, loss
  ratio, pacer rate, cwnd, trigger) through the official `network_controller_factory` injection point
  wrapping the stock GoogCC factory. Finding: PeerConnectionFactory only honours the injected factory
  behind field trial `WebRTC-Bwe-InjectedCongestionController` (first run logged "Creating fallback
  congestion controller", 0 rows); the trial is read at that single site only, so it is enabled on the
  sender when the trace is on. Loopback 15 s: 832 rows (547 process-interval, 272 TWCC feedback);
  target 300 kbps -> 3.65 Mbps, stable 0.99 Mbps, pacer 4.02 Mbps, encoder target capped at 2.0 Mbps.
  `NetworkEstimate::bandwidth` is deprecated in M120 and always infinite (-1 in the file).
* Verification status of the two new sender traces (2026-09-22): loopback only (sender+receiver on
  the gNB PC, private signaling port; 20 s 720p H.264): `verify_run.py` PASS, RTP 3023/3023 + RTX 44/44
  lost 0, encoded 592 / decoded 591, no `.ERROR` sidecars, libwebrtc log confirms "Creating overridden
  congestion controller". The full RAN code-test chain (`make run-local`, srsUE over ZMQ) was NOT re-run
  because it would stop the live OTA core; run it at the next OTA stop. Stock check: srsRAN_Project
  d2f4b70, srsRAN_4G 6bcbd9e, libwebrtc b0b827e0 (= libwebrtc.lock) all `status --porcelain` clean.

## 2026-09-22 — overhead audit of all logging code (webrtc hooks, trace rings, gNB tracer + 6 patches)
Read every hot-path hook end to end. Confirmed: both sides build Release (gNB with -march=native); no
allocation, lock or I/O on any media / RAN thread; every wrapper forwards the stock call first and
records afterwards; the RtcEvent objects on the RTP path are allocated by stock libwebrtc regardless of
the sink (RtcEventLogNull), so the per-packet delta is two vDSO clock reads + a 48-byte row copy.
Changes made to shave the remaining cost (both `apps/common/trace_ring.h` and `p5g_gnb_tracer.h`):
* ring slot = {row, ready flag} (was two separate arrays -> two cache lines per write);
* capacity rounded to a power of two, slot index by mask (was two 64-bit divisions per write);
* producer counter, flusher counter and overflow counter on separate cache lines;
* gNB ring depths sized per event rate (16384 packet/slot-rate, 4096 report-rate): ~4 MB total instead
  of ~70 MB, so the working set stays cache-resident;
* PDCP head copy (patch 0006) via segment-wise memcpy instead of a per-byte byte_buffer iterator;
* GoogCC wrapper returns the update by NRVO (NetworkControlUpdate has no move ctor; by-value passing
  copied it twice per call);
* sender `--abs-capture-time 1|0` (default 1): the one on-wire deviation from stock (+12 B on the
  first packet of each frame) is now switchable.
Left as is, deliberately: two clocks per row (design); getStats every 1 s (`--stats-period-ms`, 0 = off;
one network-thread hop per second, standard W3C practice); the injected-CC field trial (pure gate).
Re-verified after the changes: loopback 20 s PASS (RTP 2993/2993 + RTX 28/28, 0 lost, no .ERROR);
gNB rebuilt cleanly with the regenerated patch (binary not yet run: the OTA gNB is still the old build,
kept alive as `build/srsran_gnb/apps/gnb/gnb.running-*`; delete it after the next restart).
Not measured: a trace on/off A/B of latency. The hooks have no off switch by design (pure observation);
adding one would be the next step if a quantitative bound is wanted.
