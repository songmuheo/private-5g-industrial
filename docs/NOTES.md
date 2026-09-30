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

## 2026-09-22 — fixed resolution + derived start bitrate
* `--degradation disabled` added (W3C RTCDegradationPreference; DISABLED is libwebrtc-internal). Loopback:
  stock encoded 320x180..640x360 in the first 12 s; maintain_resolution / disabled stayed 1280x720.
* `--start-bitrate-kbps auto`: libwebrtc min_start table (per codec, smallest covering row) x fps/30,
  floored by the DropDueToSize thresholds, capped by the max bitrate. 720p30 H264 -> 900 kbps; 60 fps -> 1800.
  Loopback A/B (maintain_resolution, first second): stock 300 kbps start drops 16 frames at QP 34 vs
  auto 900 kbps drops 2 frames at QP 24; identical steady state (QP 13, 2.4 Mbps) from ~8 s.
* Resolved: the 2.0 Mbps "720p cap" seen in earlier stock runs was the 960x540 cap — with BALANCED
  degradation those runs were still encoding 960x540 at the end (tx-encoded width/height), and
  GetMaxDefaultVideoBitrateKbps gives 2000 kbps for <=960x540. Fixed-720p runs reach the 2500 kbps cap.

## 2026-09-22 — final review pass (defaults-run and example-conformance criteria)
Checked every app source, script, config and doc against two criteria: (a) everything meaningful runs
with default flags, (b) the base code stays as close to the framework examples as the tracing allows.
* Sender/receiver need only `--signaling-host` (and `--trace-dir`) to run: H264 720p30 synthetic
  pattern, stock degradation, stock 300 kbps start, abs-capture-time on, 1 s getStats. Verified with a
  flags-free loopback run (PASS, 0 lost).
* PeerConnection flow matches conductor.cc (Unified Plan, CreatePeerConnectionOrError, AddTrack,
  SetRemote -> CreateAnswer); the deliberate differences are the modular factory (for the observer
  injections), non-trickle ICE (complete SDP after gathering) and the lambda SDP observers, all noted
  in the file headers. Capturer matches test_video_capturer.cc's OnFrame path (VideoAdapter ->
  optional I420 ScaleFrom -> broadcaster), plus one trace row per slot and OnDiscardedFrame() on drops.
* gNB config vs srsRAN's gnb_rf_b200_tdd_n78_20mhz.yml differs only in AMF/N2 addresses, same-port
  TX mode, stdout/info logging and remote-control JSON metrics; the header comment now says exactly
  that (it wrongly claimed a tac change).
* Cleanups: unused capture timing counters (SourceTiming) removed; unused <vector> includes removed;
  stale comments fixed; both apps now log one "config:" provenance line (the run's flags were not
  recorded anywhere since tx-source.txt was dropped); explicit Close() of all sender traces;
  `make ota` / `make ota-stop` wrap ota_restart.sh; README/SETUP list it.

## 2026-09-22 — first OTA video session, and why app/ was empty
* First over-the-air WebRTC session with a Pixel: gNB PDCP UL shows 7311 RTP packets 10.45.1.12 ->
  10.53.1.1 and 936 RTCP packets back; the core log shows `DNN[oai] IPv4[10.45.1.12]`. The phone's own
  background traffic (DNS/QUIC to Google, TCP) shares the bearer and is visible in gnb_pdcp_*.csv.
* The receiver had been started with `--trace-dir $RD/app` in a terminal where RD was unset -> `/app`;
  the trace rings logged "cannot create" and the app kept running with tracing disabled, so rx-* was
  lost. Two fixes: a trace file that cannot be created is now fatal (trace_ring.h), and three root
  scripts (run_gnb_core.sh / run_receiver.sh / run_sender.sh) own their run directories and share
  them through results/CURRENT instead of shell variables.
* At session end the metrics table showed PUSCH 100 % NOK with SINR -25 dB for a few seconds before
  UEContextReleaseRequest: the UE had already left (DTX), not a link problem during the session.

## 2026-09-22 — end-to-end exercise of the three run scripts on the real gNB, and measured overhead
Ran ./run_gnb_core.sh (B210 gNB + Open5GS), ./run_receiver.sh and ./run_sender.sh (sender on the gNB PC
against the relay address 10.53.1.1; the radio hop needs the UE laptop) twice, stopping with Ctrl-C.
* Bug found and fixed: `app 2>&1 | tee log` + Ctrl-C kills tee first, the app's next stderr write hits
  the closed pipe (SIGPIPE) and the app dies mid-shutdown -> rx-* and gnb_* traces ended without their
  `# rows=` footer (tail of the last flush period lost). Fix: `| (trap '' INT; exec tee ...)` in all
  three scripts. verify_run.py now FAILs on a trace without footer. Second run: every footer present,
  verify PASS (RTP 5342/5342 lost 0), core log saved, core/route/NAT removed, files chowned.
* A Pixel was attached during the runs (its own traffic): run test2 shows rnti 0x4602 with DL 47 MB /
  UL 6 MB, UL BLER 63 % (2116 retx of 3901 grants) — the phone at that spot had a poor uplink; the
  trace set captures exactly this kind of event per grant (gnb_sched_ul new_data=0, gnb_ul_crc).
* Measured hook overhead (this PC, Release build):
  - two clock reads (mono+wall) 27 ns; TraceRing::Write into a fresh (cold) slot 11 ns; two writers
    contending 58 ns/write; realistic 3 kHz writes with the flusher draining: mean 29 ns, worst 254 ns
    (microbenchmark against apps/common/trace_ring.h).
  - per-thread CPU of the running apps over 25 s (720p30 H264 loopback): sender total 4.44 s CPU
    (encoder thread 4.13 s), its 7 flusher threads (nice 19) 0.00 s; receiver total 0.94 s, 3 flusher
    threads 0.00 s (tick resolution 10 ms). Tracing cost is below the accounting resolution.

## 2026-09-22 — multi-UE operation
* Decision: one video_receiver process per UE behind one relay (run_receiver.sh reuses a relay already
  listening on 8765). Reason: LedgerVideoDecoderFactory holds one decoded-frame trace and decoders are
  created without a stream identity, so two streams in one process would mix -rx-decoded rows. The
  receiver now refuses a second stream with an explicit message instead of silently mixing.
* Senders: `--to recvN --stream-id camN`; all trace files are stream-prefixed, gNB rows are keyed by
  ue_index/rnti (MAC/scheduler) and ue_index + UE IP + RTP ssrc (RLC/PDCP).
* Verified two receivers (recv0/recv1) behind one relay with two senders (cam0/cam1) on loopback:
  28 trace files, one set per stream, SSRCs fully separated per file, verify PASS for both; receiver
  and sender logs named per id (receiver-recvN.log, sender-camN.log; run dir results/<ts>-sender-<stream>)
  so nothing is overwritten when several instances share a directory or start within one second.
  A duplicate --stream-id is now logged by the relay and refused by the receiver instead of being
  silently dropped.
* max bitrate: the apps set none unless --max-bitrate-kbps is given; the 2.5 Mbps ceiling seen on a
  fixed 720p stream (outbound-rtp.targetBitrate = 2500000 while bandwidth_allocation was 4.8 Mbps) is
  libwebrtc's own GetMaxDefaultVideoBitrateKbps for >960x540 — stock behaviour, left as is.
* run_receiver.sh -n N starts the relay and N receivers (recv0..recvN-1) from one terminal, output
  tailed live, Ctrl-C stops receivers first (footers written) then the relay. Verified with -n 3 and three
  senders: 3 x 14 trace files, all footers, verify PASS.

## 2026-09-22 — first valid 2-UE OTA run (results/2-ue-60s, copy of results/20260922-203019-2-UE)
Two Pixels, 60 s each, 720p30 H264 fixed resolution, start 900 kbps, one receiver per stream.
* Both streams complete: cam0 15113/15113 RTP, cam1 7750/7750 RTP, 0 lost; 30.0 fps delivered on both;
  every RTP packet also present in gnb_pdcp_ul (UE 10.45.1.12 -> cam0 ssrc, 10.45.1.11 -> cam1 ssrc).
* Asymmetric outcome under identical settings: cam0 reached the 2.5 Mbps encoder cap (QP 13.6, 2.29 Mbps
  average) while cam1 stayed at ~0.9 Mbps (QP 23.8, 1.11 Mbps average): GoogCC for cam1 fell to 764 kbps
  after 10 s. RAN side: UE1 (cam1) got 6391 UL grants vs 8423, mean 14.9 vs 24.3 PRBs, both ~13-14 %
  UL BLER with ~1200/800 retransmissions, PUSCH SINR ~31 dB, MCS 27 on both. The link had headroom
  (PRBs far below 51), so the cam1 limitation is GoogCC reacting to HARQ-induced delay variation, not a
  capacity shortage — a first concrete case for the cross-layer analysis.
* Cross-host timing (wall clocks, laptops not NTP-synced against the gNB PC): median one-way packet
  delay 62 ms (cam0) vs 23 ms (cam1), frame capture->app ~120 ms both; treat as offset-contaminated
  until chrony is set up. Same-host receiver internal (last packet -> app): 28 ms / 54 ms median.
* Copy was taken while receivers and gNB were still running, so no file has its footer yet
  (verify_run flags this; the live run directory gets footers at Ctrl-C).

## 2026-09-22 — 2-UE OTA run, two Pixel 7 (results/2-ue-60s-pixel7-7) vs Pixel 7 + Pixel 9 (…-pixel7-9)
Same settings (720p30 H264 fixed, start 900 kbps, 60 s, one receiver per stream). Clean stop this time:
all footers present, verify PASS, 0 RTP loss on both streams, 30.0 fps delivered on both.
* Both streams now run high: cam0 2.43 Mbps (QP 13.3, at the 2.5 Mbps cap), cam1 2.17 Mbps (QP 14.8;
  GoogCC ended at 1.46 Mbps). In the 7-9 run one stream sat at 1.11 Mbps (QP 23.8, GoogCC 0.76 Mbps).
  The low stream in 7-9 was UE 10.45.1.11 (SIM …187860); the same SIM/IP ran at 2.17 Mbps here, so the
  drop is not tied to a phone — HYPOTHESIS: GoogCC dynamics under HARQ-induced delay variation.
* RAN per UE (60 s): 8524 / 8912 UL grants, ~24 PRBs each, MCS 27 both, PUSCH SINR 31-33 dB, UL BLER
  16.2 % / 14.6 % (retx 15.8 % / 14.4 %) — the same ~15 % BLER at maximum MCS as every run so far. With
  SINR > 30 dB this looks like link adaptation sitting at its ceiling rather than a coverage problem;
  worth an experiment with a lower max PUSCH MCS or gain changes (open).
* Cross-host delays (laptop wall clocks not synced to the gNB PC): one-way median 27 / 24 ms, frame
  capture->app 79 / 91 ms — lower than the 7-9 run (62/23, 120/120) but offset-contaminated. Same-host
  receiver internal (last packet -> app) 23 / 29 ms median (7-9: 28 / 54).

## 2026-09-28 — 4-UE OTA run (results/20260928-161259-4-UE): why the senders back off
Four Pixels (can0 10.45.1.14, cam1 .11, cam2 .15, cam3 .16), 720p30 H264 fixed resolution, 60 s, one
receiver per stream, plus a fifth attached phone (.13) with background traffic. Stream id typo: recv0's
stream is `can0`. verify PASS. cam1 ended after 10.9 s: its phone stopped answering (DL HARQ-ACK all DTX
from t≈11 s, UL CRC stops), gNB RLF "100 consecutive HARQ-ACK KOs" at 16:20:24, release at 16:20:28,
no re-attach before the end. Phone-side event, not load (SINR 32 dB until the last TB).
* Delivered: can0 1303 frames (22 fps), cam2 1508 (25.5), cam3 1669 (28.1); RTP loss 0-1 packet per
  stream, framesDropped 0. Received bitrate collapsed anyway: aggregate 4.5 Mbps (first 10 s) ->
  1.0-1.3 Mbps from 40 s; cam2/cam3 1.5 -> 0.35 Mbps, can0 never above 0.5 Mbps.
* NOT the gNB PC: UHD L/O/U 0, low-PHY real-time failure 0 in the run window (1 during Ctrl-C), late
  HARQs / failed PDCCH+UCI allocs 0, MAC DL latency 10-16 us avg / <100 us max, and the grant-slot ->
  PUSCH CRC report latency is 4.09 ms median, 4.2 ms p99, 4.3 ms max in every 5 s bin of the run —
  flat, no I/Q processing backlog. (Note: joining sched_ul with ul_crc on (rnti,sfn,slot,harq) collides
  across SFN wraps every 10.24 s; use time proximity for latency joins.)
* NOT the channel for cam1/cam2/cam3: PUSCH SINR 30-35 dB, TA 0.1-0.4 us. can0's phone is different: its
  SINR fell to 9-13 dB in 0-10 s and 45-50 s (MCS 3-4, 30+ PRB per grant -> it alone filled ~60 % of the
  UL PRBs, BSR up to 77 KB, capture->arrival +100-800 ms). That is the only genuine UL congestion in the
  run and it explains the first 10 s: everyone's BSR grew (28-40 KB) and everyone backed off together.
* Phase 2 (10-60 s) has idle UL (PUSCH PRB use 2-3 of 51 per slot avg, BSR mean 0.5-2 KB, i.e. < 1 frame)
  yet the rates stay down. Cause: UL link adaptation pinned at MCS 27 (256QAM table) with the OLLA SNR
  offset saturated at its -5 dB floor (16738 grants at -5.0) -> new-data BLER 20-26 % (target 1 %),
  retransmission BLER 48 %, 4-retx HARQ processes abandoned to RLC ARQ (cam2: 46/28/21/14/6/6 per 10 s;
  t-Reassembly expiries 66/35/39/... per 5 s). Each HARQ round costs 10.0 ms (DDDDDDSUUU), so the
  HARQ completion time is 4.1 ms median but 24 ms p90 / 44 ms p99 / up to 74 ms, and RLC recoveries add
  ~100 ms. At the app this is delay dispersion, not loss: frame span (last-first packet) 10-20 ms
  median, 40-70 ms p90, 100-390 ms max; capture->arrival median flat while p90/max tails grow
  (cam3 t35-50: tail +90-240 ms while its rate fell 1.1 -> 0.3 Mbps). RLC AM hides the loss, so GoogCC's
  loss estimator sees nothing and its delay-based (trendline) estimator reads the tails as queue growth:
  decrease, no recovery, because the tails do not shrink when the rate does (BLER is per TB, MCS fixed).
* Structure of the failures (new-data MCS 27, run window): TDD slot 7 (first UL slot after the DL->UL
  switch) 23.8 % even when the UE is alone in the slot, slot 8 alone 4.9 %, slot 9 alone 17 %; 3-4 UEs
  sharing a slot 29-50 %; SINR-bin 15-25 dB TBs fail 60-68 %, 30-35 dB 24-27 %. HYPOTHESIS: 256QAM at
  MCS 27 sits at the EVM ceiling of phone PA + B210 RX (DMRS SINR over-estimates the effective SINR),
  plus a same-port TX/RX switching transient in the first UL slot. Both testable.
* Knobs for the next runs (srsRAN release_25_10 keys, du_high_config.h / cli11 schema):
  `cell_cfg.pusch.mcs_table: qam64` (removes the 256QAM floor), `cell_cfg.pusch.olla_max_snr_offset`
  (default 5 dB, raise to 10-15 so OLLA can actually reach MCS < 27), `cell_cfg.pusch.olla_target_bler`
  (default 0.01), `cell_cfg.pusch.max_nof_harq_retxs` (default 4). App side: `--max-bitrate-kbps` per
  sender below the per-UE share. Sender-side confirmation still needed: copy the laptops'
  `cam*-tx-cc.csv` / `-tx-events.csv` (bwe_delay state, target_bps) next to this run's app/.
* Config change (ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml): `tdd_ul_dl_cfg` DDDSU, period 5 slots
  (2.5 ms), S = 6 DL / 4 guard / 4 UL symbols (srsRAN's e2e DDDSU values). Validated with a no-core ZMQ
  dry run (DU started, SIB1 ms2p5 3/6/1/4, PRACH index 159 = slot 19 = U). Expected effect: UL
  opportunity every 2.5 ms instead of 5 ms and the DL->UL switch moves into S (the full UL slot is no
  longer the first slot after the switch); cost: UL share 3/10 -> 1/5 of the slots. Not yet run OTA.
* verify_run.py prints the same UL BLER (0.3020) for every RNTI — it is the global value; per-RNTI
  BLER must be computed from gnb_ul_crc.csv (to fix).

## 2026-09-28 — 5-UE OTA run with DDDSU (results/20260928-164733-5-UE)
First run with `tdd_ul_dl_cfg` DDDSU (SIB1: ms2p5, 3 DL / S 6D-4G-4U / 1 UL). Five Pixels (cam0 .14, cam1 .11,
cam2 .15, cam3 .16, cam4 .13), 720p30 H264 fixed, 60 s, one receiver each. verify PASS, no RLF, all five
streams ran the full 60 s, RTP loss 0/0/0/0/2, PLI 1 each (initial keyframe).
* Delivered fps 26.3 / 21.4 / 19.7 / 24.3 / 26.4; freezes 3 / 7 / 5 / 0 / 1. Aggregate received bitrate
  3.5 Mbps (first 5 s) -> 1.6-1.7 Mbps (15-30 s) -> 2.1-2.4 Mbps (35-60 s); per stream 250-700 kbps.
  Same shape as the 4-UE run (start high, collapse, no return to the 1.5 Mbps start), but the plateau is
  higher (2.1 vs 1.2 Mbps aggregate) with one more stream. n=1, not attributable to DDDSU yet.
* First 10 s again genuine UL queueing: BSR max 77-108 KB (0.4-0.6 s of backlog at 1.5 Mbps), PUSCH PRB
  use 56 % of the single UL slot, capture->arrival p90 +150-360 ms. After 15 s PRB use 27-33 %, BSR mean
  0.2-1.4 KB: idle link, rates still low -> same GoogCC/delay-tail mechanism as the 4-UE run.
* DDDSU effect on HARQ timing (measured): retx gap 10.0 -> 7.5 ms; HARQ completion p90 24 -> 19-21 ms,
  p99 44 -> 36.5 ms. Frame span (last-first packet of a frame) p90 40-70 ms, max 115-265 ms: unchanged.
* UL BLER got worse, not better: new-data 32 % (4-UE: 20.5 %), retx 49-63 %, per UE 39-41 % (cam4's
  phone 50 %, SINR 29 dB). MCS 27 and OLLA offset -5.0 for every UE the whole run. gNB decode latency
  4.1 / 4.2 / 4.3 ms (med/p99/max) flat -> still no processing backlog.
* Structure: failure no longer depends on how many UEs share the slot (40.6 % alone .. 46.7 % with 5),
  but grows with TB size (rb<10: 38 %, 10-19: 51 %, 20-29: 56 %, 30-39: 61 %, 40+: 65 %) — the
  per-code-block error signature of an SINR margin near the MCS-27 threshold. Retx fail more than first
  transmissions and success arrives at retx 3-4 with the same reported SINR (34/34/33/34 dB), i.e. no
  visible HARQ combining gain.
* FINDING (code): srsRAN release_25_10 default `pusch.rv_sequence` is `{0}` (PDSCH: {0,2,3,1}); every UL
  retransmission is an RV0 repeat (chase combining only). Commercial gNBs cycle RV 0/2/3/1 (incremental
  redundancy). Knob: `cell_cfg.pusch.rv_sequence: [0, 2, 3, 1]`. HYPOTHESIS: helps if the floor is
  noise-like, not if it is a distortion floor.
* HYPOTHESIS (strengthened): a receive-side transient after the same-port TX->RX switch. Ordered by time
  from the switch to the PUSCH slot: DDDSU slot 4 (~4 symbols, 0.14 ms) 32 % fail alone; 4-UE slot 7
  (0.5 ms) 24.5 %; slot 8 (1.0 ms) 4 %; slot 9 (1.5 ms, few grants) 17 %. Reported DMRS SINR is NOT
  lower in the bad slots (4-UE slot 7 p10 28 dB vs slot 8 23 dB), so the impairment is not thermal
  noise. PUCCH also slightly worse in DDDSU (HARQ-ACK DTX 6.7 % vs 4.4 %, PUCCH SINR 13.6 vs 16.1 dB).
  Test: dual-port RX (antenna on RX2, drop `tx_mode: same-port`) removes the switch; or compare
  `mcs_table: qam64` (removes the 256QAM margin) — one change per run.
* Tooling: analysis/exp_run_report.py <run> <tdd_period_slots> produces sections A (app) and B (gNB) used
  above for any run. verify_run.py per-RNTI UL BLER still prints the global value (to fix).

## 2026-09-28 — gNB profile changes for the next runs: UL OLLA like a production gNB, 1 TX / 2 RX antennas
Config only (ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml); validated with the no-core ZMQ dry run
(1 TX / 2 RX zmq channels, DU started, values echoed in the non-default configuration block).
* `cell_cfg.pusch`: `olla_target_bler 0.1` (was 0.01), `olla_max_snr_offset 20` (was 5),
  `olla_snr_inc_step 0.02` (was 0.001). Why 20 dB: srsRAN's UL SNR->MCS thresholds are ZMQ/AWGN-calibrated
  (mcs_calculator.cpp: MCS 27 needs 21.7 dB, MCS 19 = top of 64QAM 17.1 dB); from the measured 33 dB
  median / 37 dB p90 PUSCH SINR the loop needs 11-15 dB to leave MCS 27 and 16-20 dB to reach 64QAM, so
  the 5 dB default could never act (offset sat at -5.0 in every run). Why the step: the default 0.001 dB
  moves ~0.33 dB/s at the observed BLER (a 15 dB correction would take ~45 s, most of a 60 s run);
  0.02 up / 0.18 down converges in ~2 s and dithers +-0.2 dB. Target 10 % = the CQI definition
  (TS 38.214 5.2.2.1) and the usual first-transmission OLLA target. DL OLLA left at default (not under study).
* Antennas: RF A TX/RX (DL TX + UL RX) and RF B TX/RX (UL RX only): `nof_antennas_dl 1`, `nof_antennas_ul 2`,
  `tx_mode same-port` kept (it only selects "TX/RX" as the RX antenna per channel; the actual TX<->RX switch
  is the B210 ATR on channel 0). Channel 1 never transmits, so its port is not switched. `pcap.mac_type dlt`
  added because the validator refuses >= 2 antennas with the udp pcap wrapper even with pcap disabled.
  Not 2x2 on purpose: DL rank adaptation would be a second new variable for the UL study.
* What the next run should show if the hypotheses hold: OLLA offset moving below -5 dB and MCS leaving
  27 within seconds (per-UE olla_offset / mcs in gnb_sched_ul.csv), new-data BLER settling near 10 %,
  HARQ completion p99 falling, and — from the second RX chain — higher PUSCH SINR at the same TX power.
  The switching-transient hypothesis is only partly tested (channel 0 still switches); the clean test
  remains RX on RX2 / no same-port.
* 17:12 start attempt (results/20260928-171255-5-UE, no data) failed at radio init: "B2x0 devices do not
  support different number of transmit and receive antennas" (radio_uhd). The ZMQ dry run does not catch
  this (ZMQ allows asymmetry). Fixed to `nof_antennas_dl 2 / nof_antennas_ul 2` (2x2 same-port on RF A/B
  TX/RX; DL becomes 2-port, `pdsch.max_rank: 1` available if DL must stay single-layer). run_gnb_core.sh's
  trap took the core down; no gNB/metrics/route/NAT left behind. Re-validated over ZMQ (2 TX / 2 RX).

## 2026-09-28 — 5-UE run, all five senders (results/20260928-172226-5-UE): the UL failure floor is a
## TX->RX recovery effect of the B210 same-port path, not noise, load, MCS or I/Q transport
Config: DDDSU, 2 TX / 2 RX (RF A/B TX/RX, same-port), UL OLLA target 10 % / offset cap 20 dB / step 0.02.
verify PASS, 0 RLF, all five streams 60 s. cam0-cam3 25.6-29.5 fps, 0 loss, <= 1 freeze, 330-600 kbps
each; cam4 (laptop on phone .13) 4.0 fps, 8 freezes (6.6 s), 150-360 kbps although its RAN figures equal
the others (UL fail 30 %, MCS 13, SINR 30 dB, DL retx 4 %, HARQ-ACK 89 %) -> sender/laptop side, check
sender-cam4.log (encoder fps, target bitrate). Aggregate received 2.2-2.5 Mbps (vs 2.1 previous 5-UE).
* No I/Q-transport or compute problem with 5 UEs: 0 UHD late/underflow/overflow, 0 lower-PHY real-time
  failures, PUSCH PHY processing 80 us median / 252 us p99 / 425 us max (slot 500 us), grant->CRC
  4.1 / 4.3 / 4.5 ms (med/p99/max) flat over the run, MAC latency unchanged. USB load with 2x2 at
  23.04 MS/s sc12 (~276 MB/s) is carried without overflow.
* OLLA now works as configured: offset reached -19.x dB within 10 s on every UE, MCS 5-14 instead of 27.
  BLER did not follow: 22 % new-data (target 10 %), flat from MCS 4 to 26; QPSK R<=0.3 1-9 %, QPSK R0.5
  24 %, 16/64/256QAM 27-31 % regardless of order. PHY log: failed PUSCH have LDPC iter = 6 (max) at
  DMRS SINR 32 dB, identical to the SINR of successes. An erasure of part of the slot, not an SNR margin.
* Cause found (FACT, measured): PUSCH failure depends on gNB DL transmission in the slots just before:
    PDSCH in the S slot (s-1)      -> 78.0 % fail (n=12258)   | S slot empty -> 8.9 % (n=48348)
    PDSCH only in D(s-2)           -> 39.9 %                  | D(s-3), D(s-4) -> ~baseline
    no PDSCH in the whole period   ->  6.8 % (n=38184)        | any PDSCH in the period -> 49-73 %
    per UL slot: with DL data 39-55 %, without 5-8 %.
  Presence matters, not power (S-slot PDSCH 1-10 PRB 78 %, >30 PRB 84 %): the RX needs ~1 ms after the
  end of a TX burst (S-slot PDSCH ends ~290 us before the PUSCH slot: 78 %; D(s-2) >= 790 us: 40 %;
  >= 1.3 ms: baseline). Same law explains the DDDDDDSUUU run (slot 7 worst, 9 best) and why UCI-mux,
  PRACH-slot, UE-count, TB-size and RSRP hypotheses all failed. HYPOTHESIS on the chip mechanism:
  AD9361 RX DC-offset / quadrature tracking (UHD default on) re-converging after the TX burst on the
  shared port (IQ image / DC error kills >QPSK, spares low-rate QPSK); alternatively LNA recovery.
  Two RX antennas did not help because both ports switch.
* Consequence for the study: the UL error floor is coupled to DL scheduling (RTCP/TWCC feedback,
  SIB/paging) — a testbed artefact a production gNB does not have; fix before drawing GoogCC conclusions.
* Fixes to test (one per run): (1) RX on RX2 ports with `tx_mode` default (no port switching; only
  TX->RX2 isolation matters); (2) lower `tx_gain` (80 -> 65-70) and/or `rx_gain` (40 -> 25-30) to reduce
  leakage into the RX chain; (3) `nof_dl_symbols: 0` in S (TX burst ends >= 640 us earlier) as a
  software-only mitigation. (1) is the decisive one.
* Sender traces added (results/5-ue-60s-pixel7-only/: receiver/gNB run + five *-sender-cam*/app). Operation
  check with both sides: identical sender configs (720p30 pattern, start 900 kbps auto, no cap), 1783-1791
  frames captured per sender (30 fps), every sent RTP packet found at the receiver ((ssrc,seq) join
  100 %, 0 loss), GoogCC loss estimator never engaged (loss_rate 0 throughout), GoogCC target == delay-based
  estimate at all times (the trendline detector is the binding controller; 0-9 overuse events per 5 s even
  in steady state). Delivered fps below 30 is the OpenH264 rate control skipping frames at 720p when the
  target is < ~500 kbps (captured 150 / encoded 93-149 per 5 s), not network loss.
* Relative one-way delay (tx-rtp -> rx-rtp, min-normalised, no clock sync needed): first 5 s 240-760 ms
  max on cam0-3 and 1-4.9 s on cam4 (RTT 554 ms -> 2.5 s -> 4.7 s at t=2-8 s) = startup shock of 5 x 900 kbps
  into one UL slot per 2.5 ms with 25-40 % BLER; cam4's queue was not in the phone's RLC (BSR <= 29 KB) but
  before it (laptop / USB tethering / phone IP stack). GoogCC cut cam4 to 136-230 kbps and stayed there
  (5 probes, no recovery); OpenH264 then skipped most frames ("iContinualSkipFrames") -> 4 fps. Its RAN
  figures are the same as the other UEs, so this is a sender-path effect. From 15 s on all streams:
  rel-OWD median 25-35 ms, p90 45-90 ms, max 70-220 ms; RTT 25-60 ms.
* Next: stagger starts or `--start-bitrate-kbps 300` to remove the startup shock; keep the RF fix runs
  separate from that change.
* Profile switched to the srsRAN default port mapping (2026-09-28 evening): `tx_mode` removed (continuous:
  TX on RF A TX/RX, RX on RF A RX2, no ATR switching), `nof_antennas_dl/ul 1/1`, `tx_gain 65` (was 80;
  RX2 hears the gNB's own DL during DL slots, srsRAN #77; tutorial value is 50). OLLA and DDDSU unchanged.
  ZMQ dry run OK. Expected: the S-slot-PDSCH -> 78 % PUSCH failure disappears; if the phones lose the cell,
  raise tx_gain first.

## 2026-09-28 — RX2 run (results/20260928-174551-5-UE): switching hypothesis CONFIRMED; new limit = RX overdrive
TX on RF A TX/RX, RX on RF A RX2, tx_mode default (continuous), 1x1, tx_gain 65, rx_gain 40, OLLA/DDDSU as
before. Four senders (cam2 not started). verify PASS, 0 RLF, 0 RF events, no "Same port" line.
* FACT: the DL-coupled failure is gone. PUSCH fail with PDSCH in the S slot 13.5 % vs 14.1 % without
  (before: 78.0 % vs 8.9 %); alone-in-slot 2.7 % vs 4.5 % (before 69.9 % vs 6.2 %). The same-port
  ATR switching was the cause of the 20-35 % UL floor in every earlier run.
* UL now behaves like an SNR-limited link: fail monotone in MCS (MCS 0-7: 1-6 %, 12-14: 22-26 %, 27: 72 %)
  and in SINR (>= 10 dB: 2-9 %, < 0 dB: 30-98 %); alone 2.1 %, 2 UEs 14.6 %, 3 UEs 30 %, 4 UEs 37.5 %.
  Overall 13.9 % (was 22-35 %). OLLA offsets now spread (-1 .. -19.7) instead of all pinned.
* New problem: PUSCH SINR fell from 30-35 dB to 10-18 dB while the received power per RE is at the
  metric ceiling (ul_rsrp 0.0 dBFS on 3 of 4 UEs; -8.6 on the fourth) and the noise+interference proxy
  (rsrp - sinr) rose from -41 dBFS to -14 .. -22 dBFS, worse when UEs share the slot (-13.9 vs -19.2).
  Mechanism (HYPOTHESIS, consistent): tx_gain 80 -> 65 lowered the DL by 15 dB while SIB1 still announces
  ss-PBCH-BlockPower -16 dBm, so every UE estimates 15 dB more path loss and, with alpha = 1, transmits
  15 dB more (P0 -76 dBm): the B210 RX (rx_gain 40) is driven into clipping; clipping distortion is the
  new floor and grows with the number of co-scheduled UEs. DL also degraded: CQI 12 -> 5-10, DL retx
  4 % -> up to 24 %, HARQ-ACK DTX 8 -> 14 % (feedback path for GoogCC).
* App: cam0 25.6 fps / cam4 18.7 fps, but cam1 11.7 fps (18 freezes) and cam3 10.7 fps (9 freezes): the
  two UEs with the lowest SINR (10-15 dB, MCS 3-6, 15-25 % fail) had GoogCC at 100-200 kbps.
  Aggregate 3.0 -> 0.8 -> 1.5 Mbps. 0 RTP loss on all four.
* Next (one change): restore `tx_gain: 80` (UE power and DL CQI back to the earlier regime; RSRP should
  return to ~-7 dBFS). If the N+I proxy alone-in-slot does not return to ~-40 dBFS, TX->RX2 leakage is
  present -> then lower rx_gain (40 -> 30). Alternative that keeps tx_gain 65: `ssb_block_power: -31`
  so the UEs' path-loss estimate matches the real DL power.
* tx_gain back to 80 (2026-09-28 evening) after results/20260928-174551-5-UE showed RX overdrive from the UE power-control reaction; RX2 mapping, 1x1, OLLA, DDDSU unchanged.

## 2026-09-28 — RX2 + tx_gain 80 run (results/20260928-175503-5-UE): the RF path is now clean
TX RF A TX/RX, RX RF A RX2, tx_mode default, 1x1, gains 80/40, DDDSU, OLLA 10 %/20 dB/0.02. Five senders.
verify PASS, 0 RLF, 0 RF events.
* UL: overall CRC fail 9.6 % (same-port runs: 22-35 %); new-data 6.9 %; per UE 7.4-11.4 %. Failure is
  SNR-shaped (SINR >= 25 dB: 0.2-1.4 %, 10-20 dB: 7-17 %; alone 4.1 %, 2 UEs 8.2 %, 4 UEs 18 %). No
  S-slot PDSCH effect (9.4 % vs 8.9 %). Noise+interference proxy back to -47 dBFS (alone) / -43 (sharing):
  no measurable TX->RX2 leakage floor at tx_gain 80. OLLA offsets -8 .. -15 dB, MCS 5-24 per UE (no UE
  pinned). PUSCH SINR 18-34 dB; per-UE RSRP -29 .. 0 dBFS (cam2's phone .15 is the weak one at -29 dBFS /
  17.7 dB, MCS 5-8; cam4's phone .13 saturates the metric at 0 dBFS with CQI 5: strong UL, weak DL).
* App: cam0 29.6 fps / 620-1090 kbps, cam3 29.8 fps / 510-800 kbps, both 0 freezes, 0 loss. cam2 15.9 fps
  (weak UE, 170-530 kbps). Aggregate 2.4-3.4 Mbps in the first 25 s (highest so far), 1.4-1.9 Mbps after
  cam1 dropped out.
* cam1 stopped at 24.6 s: receiver recv1 PeerConnection went disconnected -> failed -> closed while the
  phone (.11) stayed attached and active in UL until the end (no RLF, no release, DL retx 6 %). An
  ICE/DTLS-level failure on the app path, not RAN. Needs sender-cam1.log.
* cam4 (phone .13) again: 2.9 fps, 12 freezes, the only RTP loss of the run (14 packets, 13 of them at
  t=2.3-2.7 s during the startup shock: BSR mean 128 KB / max 700 KB in the first 10 s, i.e. ~6 s of
  data queued in the phone) and GoogCC stuck at 65-250 kbps afterwards. Same laptop/phone pair as in
  the two previous runs. RAN-side this UE is fine (fail 7.4 %, MCS 16-23). Open: why this pair queues
  700 KB at start (weak DL feedback path? tethering?). Needs its tx traces.
* Startup shock remains the dominant app-level problem now that the RF floor is gone: all UEs show BSR
  55-700 KB in the first 10 s. Next change on the app side only: `--start-bitrate-kbps 300` (or
  staggered starts). RAN config to be frozen here as the baseline.
* Sender traces (results/5-ue-60s-pixel7-only-2/): cam0 / cam3 healthy end to end (0 loss, 30 fps, GoogCC
  620-1090 / 560-780 kbps, 21-26 overuse events in 60 s, rel-OWD p90 ~30 ms, max 50-130 ms).
  cam1: the laptop's packets stopped reaching the RAN at ~25 s (gNB PDCP UL from .11 to the relay host:
  400-650 pkts/5 s -> 0 from t=25 s) while the phone stayed attached and the sender kept trying (RTP out
  264 -> 50 -> 12 pkts/5 s, RTCP feedback in: none after 25 s); ICE consent then failed the
  PeerConnection. Laptop<->phone tethering path dropped, not RAN, not GoogCC.
  cam4: GoogCC start shock explained. At t=0 the sender emitted 1.56 Mbps (start 900 kbps + libwebrtc's
  initial probe clusters, 2 created / 5 "successes"), the phone .13 buffered to the top BSR bucket
  (logged 700 KB) for 4 s, RTT 1.4 -> 2.1 s, target cut to 131 kbps at t=3 s and never above ~250 kbps
  afterwards (ALR, slow increase, further overuse at 40-45 s); OpenH264 then skipped most frames
  (196 encoded / 1786 captured). The 14 lost packets are from that first-second overflow. gNB served the
  UE (207 grants / 430 KB in the first second), so the overshoot is on the sender/tether side.
  cam2: weak UE (SINR 18 dB, MCS 5-8) -> GoogCC 210-515 kbps, encoder skipping after 35 s; placement.
* Next (app side only, RAN frozen): `--start-bitrate-kbps 300` on every sender (probes scale with the
  start rate) and/or staggered starts; re-run 5-UE. Check the cam1 laptop's USB tethering before that.

## 2026-09-28 — RAN audit of results/20260928-175503-5-UE (RX2, tx_gain 80): can the RAN be used as-is?
Tool: analysis/exp_ran_audit.py. Window 0-60 s, 5 UEs (cam1 active only 25 s on the app side, RAN fine).
* Health: 0 RLF, 0 UHD/PHY real-time events, 0 late HARQ, 0 failed PDCCH/UCI allocations, 0 error
  indications, 0 UCI discards, 6 PRACH detections (= attaches). TA 0.22 us median, 1.16 us max (CP 2.34).
  SINR per UE flat over time (+-2 dB). SR -> grant 3.0 ms median (3.1 p90) for every UE; BSR>0 -> next
  grant 1-3 ms median, 6 ms p90, 33-63 ms max. Grant->CRC 4.1 ms flat. No scheduler starvation.
* Slot use: 94 % of UL slots carry a grant, median 31/51 PRB, avg UL PRB utilisation 57 % for 2.4 Mbps
  delivered. Mostly 1-2 UEs per slot (8155 / 8662 slots), 3+ in 5633. DL PRB utilisation 2 % (RTCP only).
  UL grant efficiency: MAC PDU vs RLC payload shows 15-22 % non-payload for the two high-rate UEs
  (cam0, cam3) but 54-56 % for the low-rate ones (cam1, cam2, cam4): grants exceed the data (BSR bucket
  upper bounds, 5-15 % of grants issued at BSR 0). About a third of used UL PRBs carry padding. Not a
  problem at 5 UEs (43 % free) but it caps scaling; a 20 MHz DDDSU cell at this efficiency saturates
  around 4 Mbps delivered.
* Reception: CRC fail 7-11 % per UE (OLLA target 10 %), new-data 3-8 %. HARQ completion p99 12-22 ms,
  max 42-89 ms; 9-63 processes/min abandoned to RLC ARQ; t-Reassembly expiries 16-166/min (cam4 worst).
  RAN-internal hold (MAC PDU -> PDCP delivery) 0.02 ms median for three UEs, 2.6-5 ms median / 27-33 ms
  p99 / 50-83 ms max for the two high-rate UEs (RLC reordering after HARQ failures).
* Near-far is now the dominant RF limit (FACT): received power per RE spans 30 dB (cam4 0 dBFS = metric
  ceiling, cam3 -8, cam0/cam1 -16..-17, cam2 -30). In shared slots the strongest UE fails 3.6 %, UEs
  0-10 dB below 9.2 %, 10-20 dB below 14.2 %, >= 20 dB below 23.2 %; every UE fails 2-4x more when
  sharing than alone (0.7-4.2 % -> 8-13 %). Open-loop PC (P0 -76 dBm, alpha 1) does not equalise: cam4
  sits at the UE minimum power next to the RX2 antenna, cam2 is power-limited far away. The weak UE
  (cam2, MCS 5-6) takes 37 % of UL PRBs for 0.33 Mbps (Jain 0.82).
* PUCCH: HARQ-ACK DTX 5-14 % per UE, ACK-occasion SINR 3.5 dB (cam2) / 6.2 dB (cam0) up to 17 dB;
  p0_nominal for PUCCH is the default -90 dBm. DL retx 1-9 %, DL MCS 3-13, CQI 5-12.
* Verdict: the RAN is usable as a baseline now (no artefacts, sub-5 ms scheduling, stable SINR, BLER at
  target), with three things to fix or control before scaling / drawing conclusions:
  (1) equalise received power: move cam4's phone away from the RX2 antenna and cam2 closer, and/or
      `pusch.enable_cl_loop_pw_control: true` with `target_pusch_sinr` ~25 (default 10 dB is too low),
      plus rx_gain 40 -> 30 for ADC headroom; (2) PUCCH power: `pucch.p0_nominal -80` (or closed-loop
      PUCCH PC) to cut HARQ-ACK DTX; (3) account for grant padding when computing capacity.

## 2026-09-29 — run_experiment.sh (one-command multi-UE runs) + cleanup
* `./run_experiment.sh experiments/<scenario>.json` on the gNB PC: receivers, laptop preflight over the sync LAN
  (repo HEAD, asset, chrony offset), per-camera settings from JSON, synchronised start (T = now + delay, each
  laptop waits on its chrony clock), sender-trace collection into <run>/senders/camK, verify, report. The scenario
  and the resolved start time / host status are archived in <run>/scenario.json and <run>/experiment.json.
* Local smoke test (experiments/demo-local.json, 2 senders over loopback, 12 s): whole flow in 26 s, verify PASS;
  the two senders started 0.2 ms apart, 3 ms after the target second. Found and fixed on the way: a backgrounded
  shell ignores SIGINT, so the orchestrator stops run_receiver.sh with SIGTERM (its EXIT trap still INTs the
  receivers so the traces get their footers).
* Cleanup: run_sender.sh rewritten without the marker blocks (source order kendo_viewK > fade_walk > any .yuv >
  pattern; single sync check); gNB YAML comments reduced to value + reason (history in docs/RAN_CONFIG.md);
  video/prepare_fade_walk.sh removed (non-functional under YouTube's PO-token policy). exp_run_report.py skips
  the gNB sections when a run has no gnb/ traces.

## 2026-09-29 — first orchestrated 2-UE run (results/20260929-170043-2ue): INVALID, media took the sync LAN
run_experiment.sh worked end to end (preflight over ssh, both senders started 0.6 ms apart, 300 s, traces
collected, report) and its A2 check caught the problem: both streams' selected ICE pair had the gNB PC's
sync-LAN address 192.168.77.1 as local candidate, RTT 0 ms, 96 MB each; gNB PDCP UL saw 13 / 5 RTP packets.
Hence 2.5 Mbps flat (libwebrtc cap), 30 fps, 0 freezes: a gigabit-Ethernet result, not a 5G one.
* Cause: the first sync-LAN firewall only filtered INPUT. libwebrtc's ICE sends connectivity checks from the gNB
  PC to the laptops' host candidates (192.168.77.1K); the outbound UDP created conntrack state, so the replies
  came back as ESTABLISHED and the pair succeeded with the lowest RTT of all.
* Fix (scripts/setup/sync_lan_server.sh, applied): dedicated chains P5G_SYNC_IN/OUT on enp4s0 allowing only
  NTP, SSH (both ways) and ICMP, dropping everything else in BOTH directions; verified ssh/ping/chrony still
  work and outbound UDP is dropped. run_experiment.sh now refuses to launch over ssh without the DROP chain and
  writes <run>/INVALID when A2 finds a stream off the 5G path. Laptop Wi-Fi should be off during runs as well.
* verify_run.py right after the run reported 11 failures because the gNB trace files had no footers yet (the gNB
  was still running); re-run after stopping the gNB it is PASS. The orchestrator now says so.

## 2026-09-29 — second orchestrated 2-UE run (results/20260929-172846-2ue): VALID, first clean 5-minute run
Firewall fixed; A2: cam0 selected pair 10.53.1.1, gNB PDCP UL RTP 100 % to 10.53.1.1 -> media over the 5G link.
Senders started 0.6 ms apart (chrony 36 us on both laptops). verify PASS after the gNB was stopped.
* cam0 (phone .15): 300 s, 29.6 fps, 1 freeze (0.2 s), 0 loss, jitter buffer 98 ms avg, 1.4-2.5 Mbps
  (GoogCC 1.5-2.4 Mbps while two UEs shared the cell, 2.5 Mbps = libwebrtc cap once alone).
* cam1 (phone .11): 2.5 Mbps (cap) and 30 fps for 80 s, then its link degraded (SINR 27-29 -> 21 dB, MCS 23 -> 13,
  BSR to the top bucket, GoogCC 4.1 -> 1.2 Mbps), and at t=142 s the phone stopped answering: DL HARQ-ACK DTX
  99 %, RLF "100 consecutive HARQ-ACK KOs" at 17:32:13, release 17:32:17; no CSI/UL/DL afterwards. Same phone
  (.11) as the 24.6 s dropout on 2026-09-28. The laptop kept sending STUN keepalives; ICE failed at ~139 s.
  HYPOTHESIS: phone-side (thermal throttling under sustained max-power UL, USB tether power state, or cable);
  swap the phone / keep it cool and charging, and watch its SINR trend in the next run.
* cam0 dipped to 0.5-1.0 Mbps at t=140-165 s (13 GoogCC overuse events at t=140) exactly while the gNB ran the
  4 s RLF timer on ue1 and its UL grants fell to ~1000/10 s (from ~1900): HYPOTHESIS the scheduler kept
  allocating the dead UE. Recovered to the cap within 30 s of the release.
* RAN: UL CRC failure 10.0 % / 8.7 % (OLLA target 10 %), MCS 19-24 median, OLLA -8..-9 dB (not saturated),
  SINR 24-29 dB; large single-UE allocations at MCS 27 fail 25-36 % (the 256QAM ceiling, handled by OLLA).
  grant->CRC 4.1 ms flat, 0 RF events. Aggregate 4.0-4.9 Mbps with two UEs: the highest so far.
* exp_run_report.py now spans the whole run (10 s bins beyond 90 s).

## 2026-09-29 — third 2-UE run (results/20260929-185428-2ue), phone swapped on SIM .11: the RADIO LINK is the problem
Valid path (A2 OK), start 0.16 ms apart, verify PASS after gNB stop. cam1 (.11, different phone) ran 2.5 Mbps /
30 fps for 170 s, degraded, and at t=273 s the UE stopped transmitting (UL CRC KOs, then no CSI) -> gNB RLF
and release at 19:00:04; it re-attached 47 s later. cam0 (.15) ran 300 s but its bitrate slid from 2.9 to
0.3 Mbps: PUSCH SNR 24 -> 11 dB, CQI 8 -> 4, UL RSRP -28 -> -35 dBFS over the run.
* Root cause is not the phone: swapping it reproduced the dropout (24.6 s, 142 s, 273 s across three runs).
  The gNB table shows why: **power headroom 0 dB on both UEs all run** (phones at maximum transmit power) and
  **CQI 4-7 / DL MCS 0-3**, versus PHR 23-25 dB and CQI 12-15 on 2026-09-28 17:55 with identical gNB settings
  (RX2, tx_gain 80). Both link directions lost ~20 dB since yesterday -> a physical change after re-cabling
  for the sync switch: antenna/cable seating on RF A TX/RX and RX2, antenna type, or phone placement.
  With PHR 0 the UE cannot follow any extra loss, so a small fade ends in UE-side out-of-sync -> RLF (cam1);
  and a phone held at max power for minutes heats and backs off its TX (cam0's slide). HYPOTHESIS on the
  exact element; FACT that the link budget is ~20 dB worse than yesterday.
* Action: re-seat both antennas/cables, confirm the antenna on TX/RX is the DL one and covers 3.5 GHz, put the
  phones 1-2 m from the antennas in line of sight, then check the gNB table before running: CQI >= 11 and
  PHR >= 15 dB for every UE. run_experiment.sh now prints this link check (CQI / PUSCH SNR / RSRP / PHR per
  UE) in preflight and warns on PHR <= 3 dB or CQI < 9.

## 2026-09-29 — 2-UE run with antennas back on RF A (results/20260929-191234-2ue): link fixed, GoogCC dynamics remain
Cause of the previous two runs confirmed: the antennas had been moved to RF B while srsRAN's 1x1 cell drives
channel 0 = RF A (UHD B210 subdev order A:A, A:B); the gNB was transmitting into an open TX/RX connector and
receiving on an open RX2. Back on RF A: CQI 14-15 (was 4-7), PHR 15-24 dB (was 0), PUSCH SNR 25-31 dB,
0 RLF, both streams 300 s over the 5G link (A2 OK), verify PASS. The slot-14 excess shrank to 10.8 % vs 7.5-7.9 %
in the other UL slots and the odd/even-frame split vanished (21 % / 14 % before).
* cam1 (.11): 29.7 fps, 3 freezes (0.7 s), but bitrate 0.7-1.3 Mbps most of the time with bursts to 2.2-2.6 Mbps
  at t=140-160, 250, 290 s. cam0 (.15): PUSCH SNR 14-18 dB for the first 120 s (MCS 6-11; placement) then 27-30;
  GoogCC 340-450 kbps -> 16 fps, 17 freezes in that phase, 1-3.6 Mbps afterwards.
* RAN is not the bottleneck any more: UL PRB utilisation 13-45 %, HARQ completion p99 12-27 ms, MAC->PDCP hold 0,
  RLC t-Reassembly expiries 0-19/10 s except 32-42 at t=260 s.
* What remains is GoogCC on a shared UL: both senders probe up together (t=150-170, 250-260, 290), BSR spikes
  to 55-150 KB (0.2-0.5 s of data), tx->rx one-way delay max 220-750 ms, 8-21 overuse events per 10 s, both
  back off, ~90 s cycles; in between, HARQ jitter (rel-OWD p90 30-50 ms at p50 15-19 ms) still triggers 2-10
  overuse events per 10 s and keeps the estimate well below the 2.5 Mbps cap. This is the phenomenon the
  testbed was built to measure; the RAN and orchestration are now good enough to study it.

## 2026-09-29 — 2-UE run results/20260929-192247-2ue: clean regime, then a fade-triggered collapse (analysis/exp_sender_report.py)
Link: CQI 14-15, PHR 15-23 dB, PUSCH SNR 26-34 dB, 0 RLF, both streams over the 5G link, verify PASS, start 2.8 ms apart.
* 0-90 s: both senders at the 2.5 Mbps encoder cap, 30 fps, UL CRC failure 0-3 % at MCS 27, 0 overuse events,
  rel-OWD p90 30 ms / max 40-50 ms, UL PRB 28-38 %, aggregate 5.0 Mbps. MCS 27 (256QAM) decodes at ~0 % when the
  SINR is >= 30 dB on the repaired RF path.
* t=97-101 s: cam1's UE (.11) fades 28 -> 20 dB SINR for ~5 s. OLLA takes its MCS 23 -> 6; the scheduler gives it
  ~300 grants/s (was 180-210) to move the same bytes; UL load jumps.
* t=103-104 s: BOTH UEs fail 31-35 % of TBs (cam0 at 29 dB SINR too), BSR 77-108 KB, rel-OWD max 580 / 983 ms,
  overuse 5+3 (cam0) and 6+7 (cam1) in two seconds; GoogCC 4.2 -> 0.98 Mbps (cam0) and 2.0 -> 0.49 Mbps (cam1).
  Queues drain by t=106; link back to 0-10 % failure by t=107.
* Recovery: GoogCC climbs ~+30 kbps/s; cam0 reaches its cap again at t=190 s, cam1 never does (1.0-1.7 Mbps for
  the remaining 190 s, HARQ jitter p90 30-40 ms keeps producing 1-7 overuse events per 10 s). Second event at
  t=205-215 (cam0 4.0 -> 2.0 Mbps). Aggregate 2.3-3.6 Mbps after the first event vs 5.0 before.
* Reading: a 5 s single-UE fade becomes a 1 s cell-wide congestion pulse (link adaptation of one UE consumes the
  shared UL), GoogCC reacts to the delay pulse with a 60-80 % cut and recovers linearly and slowly; the RAN was
  back to normal within 3 s. HYPOTHESIS for cam0's 32-35 % failures at 29 dB SINR during the pulse: dense
  co-scheduling with the low-MCS UE (inter-UE interference / power imbalance); to be tested with the near-far
  breakdown in exp_run_report.py on more runs.
* Tooling: analysis/exp_sender_report.py <run> reproduces this (tx->rx join is sequence-wrap aware; 300 s runs wrap
  the 16-bit RTP sequence once).

## 2026-09-30 — GStreamer as a fixed-profile sender: local check (no radio), 10 s, 720p30, x264 CBR
Question (docs/SCENARIO_EDGE_PROFILES.md §6.3 option B): does a GStreamer pipeline keep fps and resolution fixed, and
can the bitrate be changed while PLAYING without touching either? Pipeline on the gNB PC (GStreamer 1.20.3, x264enc):
`videotestsrc is-live=true pattern=<ball|snow> num-buffers=300 ! video/x-raw,format=I420,width=1280,height=720,framerate=30/1
 ! x264enc tune=zerolatency speed-preset=veryfast pass=cbr bitrate=2500 vbv-buf-capacity=33 key-int-max=90 threads=4
 [option-string=nal-hrd=cbr:force-cfr=1] ! rtph264pay mtu=1200 ! fakesink sync=false`, pad probes on the encoder sink
(raw frames in) and src (encoded frames out, size, DELTA_UNIT flag), `bitrate` set to 800 at t = 5 s
(scratchpad script gst_fixed_check.py, not versioned; the pipeline above reproduces it).
* fps / resolution: 300 raw frames in -> 300 encoded out, 1280x720 for every frame, in all three variants, across the
  bitrate change. x264 has no frame-skip mode (unlike OpenH264 `bEnableFrameSkip`); the only drop points in a GStreamer
  pipeline are opt-in: `videorate` (only if the source is off-grid), `qos=true` on sink/encoder (default false),
  `queue leaky=` / `appsrc leaky-type=` (default none), `udpsink max-lateness` (default -1). None is on by default.
* bitrate (content-limited unless HRD): `ball` (trivial content) produced 88-133 kbps against a 2500 kbps target — x264 CBR
  is a ceiling, not a floor. `snow` (noise) produced 2095 kbps -> 648 kbps after the change (≈84 % of target; the 33 ms VBV
  buffer = one frame makes the RC conservative). With `nal-hrd=cbr` filler NALs make the output exactly 312.5 kB/s =
  2500 kbps every second (constant size per frame, what a RAN-facing "profile" would like), BUT the bitrate change at
  t = 5 s was refused: x264 "VBV parameters cannot be changed when NAL HRD is in use" (GST_DEBUG=x264enc:5). So strict CBR
  with filler and runtime bitrate change are mutually exclusive in x264; a profile switch under HRD-CBR needs an encoder
  reopen (= IDR), which the RAN must be told about anyway (epoch).
* GOP determinism: on `snow`, x264 placed 3-4 I-frames per second (scenecut detection on noise), i.e. the encoder chose the
  keyframe pattern on its own. For a declared profile set `option-string=scenecut=0:min-keyint=N` with `key-int-max=N`.
* x264enc `bitrate` and `vbv-buf-capacity` are the only rate properties changeable in PLAYING (gst-inspect flags);
  `pass`, `key-int-max`, `option-string` are not (NULL/READY only).
* Not measured here: RTP packet counts (rtph264pay emits buffer lists for fragmented frames; the BUFFER probe under-counts),
  encode latency, CPU. Not a substitute for the OTA invariant check in §6.3.

## 2026-09-30 — two transport trees: webrtc/ frozen, gstreamer/ built and verified (loopback + srsUE code test)
Decision (docs/SCENARIO_EDGE_PROFILES.md §6.3): the fixed-profile sender is a GStreamer stack, not libwebrtc with
overrides. Layout: `webrtc/` (moved as-is with git mv, tag `webrtc-baseline-2026-09-30` marks the last root-level
state) and `gstreamer/` (new); no code shared, only the trace-file contract (TRACE_SCHEMA §0-a); shared RAN scripts
take the tree as a parameter.
* webrtc after the move: `make webrtc-build-apps` (6 s incremental), `webrtc/run_experiment.sh demo-local` PASS,
  `make run-local TREE=webrtc` PASS (results/20260930-114022-webrtc-moved: 3782/3782 RTP, 0 lost, 681 frames in 15 s).
  Behaviour unchanged (same binaries, only paths). The stale CMake cache had to be deleted once.
* gstreamer loopback (results/20260930-120719-demo-local, 2 senders 12 s): cam0 720p30 2500 kbps -> 360 frames
  delivered = 30.0 fps; cam1 640x360@15 800 kbps -> 180 frames = 15.0 fps; 0 RTP loss; verify PASS. The webrtc tree
  on the identical scenario an hour earlier delivered 27.4 fps and 6.9 fps (GoogCC start-up + OpenH264 skipping).
* gstreamer srsUE code test (results/20260930-120754-gst-first, 720p30 2500 kbps, 20 s window): 838 captured /
  837 encoded / 837 decoded and delivered; 9689/9689 RTP through gNB PDCP (`pdcp_ul` 9712 incl. RTCP), 0 lost,
  UL BLER 1.1 %. **All 837 frames join sender -> gNB PDCP -> receiver directly on `rtp_ts`** (no per-SSRC offset:
  rtph264pay timestamp-offset 0, PTS from the capture grid). capture -> gNB PDCP marker packet 31.7 ms median /
  49.1 p90 / 220 max; capture -> app 32.3 / 49.6 / 222 (decode + delivery add 0.6 ms; jitter buffer 50 ms did not add
  waiting because frames arrive later than its schedule). 10 RTP packets per frame (mtu 1200).
* Implementation facts worth knowing: (1) x264enc shifts output PTS by a constant and shifts its segment by the
  same amount (gst_video_encoder_set_min_pts), so the encoded-frame probe must map PTS through the pad's segment to
  running time before applying the 90 kHz scale — the first run logged rtp_ts 1877455799 for a frame whose wire value
  was 3686 until this was done; (2) per-frame side info (rtp_ts, packet count, first/last arrival) rides from the
  depayloader to the sink as untagged GstReferenceTimestampMeta, which GstVideoDecoder copies onto the decoded
  frame — no lookup table, no lock; (3) rtph264depay pushes the AU synchronously while handling the marker packet,
  so a sink-pad probe and a src-pad probe on the depayloader run on the same thread; (4) pipefail + `awk ... exit`
  on gst-inspect output aborts a script (SIGPIPE) — the provenance loop in build_apps.sh must not exit awk early;
  (5) the apt index was stale (404 on plugins-base dev) until `apt-get update`.
* Not yet: OTA run of the gstreamer tree (laptops need `make deps` + `make build-apps`, and the preflight should
  compare GStreamer versions), NACK/RTX (deliberately absent), profile changes of width/height/fps at run time
  (refused with a warning; bitrate works), multi-stream receiver process.
* Fix (same day): tx-frames had one row more than tx-encoded (361 vs 360, 838 vs 837) — the last captured frame
  was pushed (to_encoder=1) and then the pipeline went to NULL before x264enc emitted it. The sender now sends
  EOS through appsrc after stopping the grid and waits (<= 2 s) for EOS on the bus before NULL: demo-local
  re-run results/20260930-155332-demo-local gives captured = encoded = delivered (361/361/361, 181/181/181).
* Codex review (same day, static) found and the following were fixed: (H) missed capture slots after a blocked push
  were skipped silently -> now one `to_encoder=0` row per missed slot + count at shutdown; (H) Arrival ring had plain
  fields shared between threads -> all-atomic entries, ms-keyed 1024 slots (was rtp_ts % 64: only 8 slots at 30 fps),
  late/reordered packets update their frame's entry; (H) media started before the receiver had opened its traces ->
  `stream-ack` handshake + 5 s watchdog; (H) shutdown joined a thread that could be blocked in
  gst_app_src_push_buffer -> RequestStop / bounded WaitStopped / EOS / NULL / Join order; (M) I420 sizes with
  width % 4 != 0 would have been mis-read -> refused at start-up (GstVideoInfo check); (M) tx-encoded lacked
  `at_target_quality` -> added (-1); (M) receiver `stats_` FILE* race -> atomic; (M) RTCP destination behind port
  translation -> learned from incoming RTCP (GstNetAddressMeta); (M) local_apps.sh stop killed every video_sender
  on the host -> run-scoped labelled pids, ordered TERM + wait, sudo for both sides; (M) run_receiver.sh reused any
  listener on 8765 -> ping/pong identity check, P5G_CONTROL_PORT; (L) meta reference caps were created lazily on a
  streaming thread -> InitFrameMetaCaps at start-up (per-frame meta allocations remain, bounded; documented).
  Re-verified: demo-local 361/361/361 and 181/181/181 (results/20260930-162048-demo-local); run-local 689/689/689,
  8012/8012 RTP through the gNB, ordered stop with no KILL fallback (results/20260930-162118-gst-review-fixes).
* Codex passes 2 and 3 (same day): remaining PARTIALs fixed — slots elapsed during a final blocked push are recorded;
  arrival snapshots are seqlock-coherent (writer release fence after the odd seq, reader acquire fence + re-check,
  `--jitter-ms` capped at 800 against the 1024 ms ring); ack watchdog independent of the stats period, stats on a
  monotonic schedule with tick = gcd(period, 1000); stream-ack bound to the exact receiver connection and a
  handshake generation (re-registered same-id connection, stale generation and replay are dropped — unit-checked);
  RTCP endpoint learning only from a strictly validated compound whose first packet is SR/RR with the announced
  SSRC; P5G_CONTROL_PORT passed to the laptops over ssh. A bug introduced on the way (generation counter on the
  wrong object) was caught by the sender's 5 s ack watchdog (run marked FAIL) before it reached a commit.
  Re-verified: demo-local 361/361/361 + 181/181/181 (results/20260930-163242-demo-local), run-local
  (results/20260930-163320-gst-review3).
* Codex pass 4 (same day): the two remaining PARTIALs fixed — the stream-ack handshake is bound to BOTH connections
  (receiver and sender) plus a generation, and cleared when either disconnects or is replaced (in-memory checks:
  sender disconnect / sender replacement / receiver replacement / stale generation / replay all dropped, the intended
  ack forwarded once); RTCP endpoint learning requires a fully valid compound per RFC 3550 §6.4.1/6.4.2 (report-count
  sized SR/RR, padding only on the last packet with a sane count byte, known packet types, exact tiling) — 11 crafted
  packets checked (scratchpad rtcp_valid_test.cc, not versioned): the 8-byte "RR with rc=1" and padded-not-last cases are
  now rejected. Re-verified: demo-local 20260930-163749-demo-local, run-local 20260930-163815-gst-review4.
* Codex pass 5 (same day): RTCP validation completed — every packet of the compound now needs its type- and
  count-specific minimum body (SR/RR exact, SDES >= 4+8·SC, BYE >= 4+4·SC, APP/RTPFB/PSFB >= 12; unknown types
  invalid; compound must start with SR/RR). The 11 crafted cases became 18 (bare 4-byte SR header after a valid SR,
  under-sized SDES/RTPFB, SDES-first, SR with a profile extension: all rejected) and live in
  gstreamer/apps/tests/rtcp_valid_test.cc, run by build_apps.sh. Re-verified: demo-local 20260930-164255-demo-local, run-local 20260930-164321-gst-review5.

## 2026-09-30 — first OTA run of the gstreamer tree, 2 UEs (results/20260930-165543-2ue-gst): the stack works; cam1's link cannot carry 2.5 Mbps
Scenario gstreamer/experiments/2ue-720p30.json: cam0 (laptop .10, phone 10.45.1.15, rnti 0x4603) and cam1 (laptop .11,
phone 10.45.1.11, rnti 0x4601), both 1280x720@30 **fixed 2500 kbps** (x264 ABR, GOP 60), Kendo view0/1, 300 s, synchronised
start. Preflight: chrony RMS 45 / 47 µs, link cam0 PUSCH SNR 20 dB CQI 10, cam1 PUSCH SNR 11.5 dB CQI 14, PHR 27 dB both.
* **Mechanics all worked**: control handshake (`stream-ack` both), RTCP endpoint learned from the phones' SR, senders started
  0.6 ms apart, traces collected over the sync LAN, every trace has its footer, 0 `.ERROR`, `verify_run` PASS (after the gNB
  stop), A2: 100 % of both streams' RTP over the 5G link (n = 100047 / 96488 PDCP rows). 0 missed capture slots on either
  laptop (`to_encoder=1` for all 8996 / 9000 rows); captured == encoded on both (8996, 9000), encoder output 2501 kbps each,
  150 IDR each (every 2 s as configured). The fixed profile did exactly what it says: the senders never changed anything.
* **cam0 (20 dB, MCS 13-19)**: 8996/8996 frames delivered at 30.0 fps, 100044/100044 RTP (0 loss). Capture -> app (absolute,
  chrony): median 71 ms, p90 693, p99 3695, max 4133 ms. Steady state 63-70 ms median (grant cycle + 50 ms jitter buffer);
  four excursions to 0.7-4 s at t = 15-45, 135, 175-180, 200-215, 280-290 s (see graphs/delay.png).
* **cam1 (8-12 dB, MCS 3-9)**: 9000 encoded, 8444 delivered (556 lost, all captured between t = 16 and 61 s), 4902/101381 RTP
  lost (4.8 %, all in t = 10-60 s) — lost BEFORE RLC AM (which would have recovered them): the phone's own queue overflowed.
  gNB BSR for this UE: mean ~700 KB, max 2.1-3.1 MB the whole run (= 7-10 s of video). Capture -> app median 4.1 s, p90 9.3 s,
  max 20.9 s; the queue peaked at t≈38 s (20 s), drained to ~2 s by t=120 s, refilled at every cell event. Delivered 20.8 fps
  in the first 30 s, 30 fps afterwards (late, not dropped: x264 never skips, the receiver never drops).
* **RAN**: UL PRB utilisation ~80 % from the moment both senders ran (2 UEs sharing the single UL slot, 4600 grants per
  10 s); cam1 at MCS 3-9 needs ~3x the PRBs of cam0 for the same 2.5 Mbps. UL CRC failure 10.0 % on both (OLLA at target),
  HARQ completion p99 22-24 ms, grant->CRC 4.1 ms flat, 0 RLF. At t=200 s both UEs' MCS dropped together (cam1 9 -> 2, cam0
  19 -> 8, OLLA -10 dB) = a cell-wide event, not a per-UE fade; both queues jumped (cam0 4 s, cam1 +5 s).
* **Reading (for docs/SCENARIO_EDGE_PROFILES.md)**: this is the "profile exceeds the feasible capacity" case in its pure form.
  Without a congestion controller the excess does not disappear, it queues in the UE (up to 3 MB) and shows up as seconds of
  delay and, when the phone's buffer is exhausted, as loss before the RAN. Nothing in the RAN or the sender signals it; only
  the gNB BSR and the receiver's capture->app delay reveal it. The edge must choose the per-camera profile from the RAN's
  per-UE cost (cam1 at MCS ~8 ≈ 1/3 of cam0's spectral efficiency): the same 2.5 Mbps is a 30 % cell load for cam0 and an
  infeasible one for cam1. Comparison point: the webrtc tree on 2026-09-29 (192247) would have cut cam1 to ~0.5-1 Mbps within
  seconds and kept the delay at tens of ms — at the cost of fps/quality decided by the sender, invisible to the edge.
* **Artefacts to remove before the next run**: (1) cam1's phone at 11 dB PUSCH SNR vs 20 dB (placement; the 09-29 runs had
  25-34 dB); (2) cam0's phone downloaded **230 MB over UDP/QUIC from 58.123.x / 1.225.x (KT CDN)** during the run
  (gnb_pdcp_dl: 216k SDUs, ~6 Mbps DL) — background app traffic on the same bearer; disable background data / updates on the
  phones; (3) `log.all_level: info` in the gNB profile writes 416 MB of gnb.log per 5 min (3.46 M lines; 1.39 M in the
  09-29 run too) — srslog's own thread, but disk I/O on the gNB PC; set `warning` (rule 2: extra logs opt-in).
* Tooling: exp_run_report.py's last W3C-stats dependency (aggregate kbps) replaced by the RTP ledger; plot_run works on the run.
* Next runs (one variable each): (a) same placement, cam1 at 1000 kbps / cam0 2500 (profile fitted to the link) — expect no
  queue; (b) move cam1's phone to >= 20 dB and repeat 2 x 2500; (c) 5ue-720p30.json (5 x 1000 kbps) once (a) is clean.

## 2026-09-30 — rtpgccbwe (GCC) on GStreamer 1.20.3: compatible, verified on loopback
Question: option B (docs/SCENARIO_EDGE_PROFILES.md, "fixed fps/resolution + GCC-driven bitrate" on the gstreamer tree)
needs `rtpgccbwe` from gst-plugins-rs; its main branch's webrtc plugin requires GStreamer 1.22 — does the rtp plugin
(rtpgccbwe) work on our 1.20.3?
* FACT (crates.io metadata): every `gst-plugin-rtp` release from 0.11 to 0.15.4 is built for the GStreamer 1.20 API
  (Cargo feature `v1_20`); only the webrtc plugin needs 1.22. 0.13.7 has MSRV Rust 1.71, so Ubuntu 22.04's apt
  `rustc/cargo 1.75` builds it (no rustup). 0.14/0.15 need Rust 1.85+/1.92.
* FACT (our 1.20.3 source): `gstrtpsession.c gst_rtp_session_notify_twcc` pushes the "RTPTWCCPackets" custom upstream
  event that rtpgccbwe consumes (`gcc/imp.rs:1186`), so the feedback path exists in 1.20.
* Built: `gstreamer/scripts/build_gst_rs.sh` (crate pinned in gstreamer.lock: gst-plugin-rtp 0.13.7) -> 49 s ->
  `gstreamer/build/gst-plugins-rs/libgstrsrtp.so`; `gst-inspect-1.0 rtpgccbwe` OK with GST_PLUGIN_PATH.
* Loopback check (scratchpad gcc_check.py, not versioned): videotestsrc snow 720p30 -> x264enc cbr 3000 -> rtph264pay with
  the TWCC header extension (id 1, added via the `add-extension` signal as in gst-examples/webrtc/sendrecv) ->
  rtpgccbwe(min 0.3, max 6 Mbps) -> rtpbin(rtp-profile=avpf) -> udpsink; receiver udpsrc with `extmap-1` in caps ->
  rtpbin(avpf), RTCP both ways; `notify::estimated-bitrate` -> x264 `bitrate`. tc tbf 1.5 Mbit/s on lo from t=10 s
  to 25 s. Result: estimate 3.1 -> 5.7 Mbps unconstrained (0-8 s), 5.7 -> 1.2 (t=11.5) -> 0.55 Mbps (t=13) under the
  limit, then +8 %/s ramp to 1.3 Mbps by t=25, 4.2 Mbps by t=40 after the limit was removed; 290 estimate updates in
  40 s. GCC behaviour as expected (undershoot on delay increase, slow multiplicative recovery). Pitfall found: in a
  gst-launch string `rtpgccbwe ! rtpbin ! udpsink` links the wrong rtpbin pads (rtpgccbwe reported NotLinked); the
  send session must be wired explicitly (`rtpbin.send_rtp_sink_0`, `rtpbin.send_rtp_src_0`), as our C++ does.
* Decision: option B = the existing sender/receiver + `--cc gcc` (TWCC extension, rtpgccbwe before the send session,
  avpf, estimate -> x264 bitrate through the same path as the `profile` message) vs `--cc profile` (default, no
  estimator). fps/resolution stay caps-fixed in both. The laptops need libgstrsrtp.so (copy) + GST_PLUGIN_PATH.
