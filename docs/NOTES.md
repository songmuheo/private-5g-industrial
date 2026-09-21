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

**Open.** Radio profile untested with hardware; SIM list `ran/P-Sim.csv` is PLMN 999/70 (configs are
001/01); cross-host clock-sync procedure not yet exercised.
