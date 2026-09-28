# RAN configuration — what is set, why, and what it was measured against

Profile: `ran/gnb/configs/gnb_b210_n78_tdd_20mhz.yml` (srsRAN_Project `release_25_10`, commit 51e1091, stock +
tracer patches). Everything not listed below is the srsRAN default; the file itself carries every replaced
default and the reason next to the value. All numbers here come from the runs in `docs/NOTES.md` (2026-09-28).

## Hardware and radio

| item | value | why |
|---|---|---|
| SDR | USRP B210, `type=b200`, 23.04 MS/s, `sc12` over the wire | 20 MHz @ 30 kHz is the USB 3.0 limit; srsRAN B200 sample values |
| Ports | **RF A `TX/RX` = DL TX, RF A `RX2` = UL RX**, `tx_mode` default (`continuous`), 1 TX / 1 RX antenna | srsRAN's documented port mapping (discussion #77, sample config). `same-port` (one antenna, ATR switching) was used until 2026-09-28 and caused a 20-35 % UL error floor: PUSCH failed 78 % when the gNB had transmitted PDSCH in the preceding special slot vs 9 % otherwise; with RX2 the dependence is gone (9.4 % vs 8.9 %). |
| Gains | `tx_gain 80`, `rx_gain 40` | sample values. `tx_gain 65` was tried with RX2: the UEs' open-loop power control (P0 -76 dBm, alpha 1, SIB1 still announcing -16 dBm SSB power) raised their TX by 15 dB, PUSCH hit 0 dBFS and clipped (noise floor -41 -> -14..-22 dBFS). Reverted. |
| Antennas | 2 TX / 2 RX is possible only with four antennas (A/B TX/RX + A/B RX2); B2x0 refuses unequal TX/RX counts. | measured: "B2x0 devices do not support different number of transmit and receive antennas" |
| Clock | internal | phones attach reliably in the lab; an external 10 MHz/GPSDO is the srsRAN recommendation if they do not |

## Cell

| item | value |
|---|---|
| Band / ARFCN / BW / SCS | n78, DL ARFCN 632628 (3489.42 MHz), 20 MHz (51 PRB), 30 kHz |
| PLMN / TAC / PCI | 00101 / 7 / 1 (Open5GS at 10.53.1.2, N2/N3 bind 10.53.1.1) |
| TDD | `tdd_ul_dl_cfg`: **DDDSU**, period 5 slots (2.5 ms), special slot 6 DL / 4 guard / 4 UL symbols (srsRAN's own DDDSU e2e values). Default was DDDDDDSUUU (5 ms, 3 UL slots). Effect measured: HARQ retransmission gap 10.0 -> 7.5 ms, HARQ completion p99 44 -> 36 ms; cost: UL share 3/10 -> 1/5 of slots. PRACH index stays 159 (B4, slot 19 = UL). |
| PRACH | default 159 | fits both patterns (`prach_helper.cpp`) |
| pcap | `mac_type: dlt` | required by the validator once >= 2 antennas are configured; harmless at 1x1 |

## UL link adaptation (`cell_cfg.pusch`)

| parameter | value | default | why |
|---|---|---|---|
| `olla_target_bler` | 0.1 | 0.01 | CQI is defined at 10 % BLER (TS 38.214 5.2.2.1); the usual first-transmission OLLA target |
| `olla_max_snr_offset` | 20 dB | 5 dB | srsRAN's UL SNR->MCS thresholds are ZMQ/AWGN-calibrated (MCS 27 = 21.7 dB, MCS 19 = 17.1 dB). From the measured 30-35 dB PUSCH SINR the loop needs 11-20 dB to leave MCS 27 / reach 64QAM; with 5 dB it sat at the floor in every run. |
| `olla_snr_inc_step` | 0.02 dB | 0.001 dB | up 0.02 per ACK, down 0.18 per NACK: converges in ~2 s instead of ~45 s (longer than a 60 s run), dithers +-0.2 dB (< one MCS step) |
| `mcs_table` | qam256 (default) | | kept; with the RF path fixed MCS 25-27 decode at ~1 % failure when SINR >= 30 dB |
| `rv_sequence` | {0} (default) | | note: srsRAN's UL default is RV0 repeats only (PDSCH default is {0,2,3,1}); `[0,2,3,1]` is available if HARQ combining gain is wanted |

## Defaults kept that matter for interpretation

Scheduler `time_qos`; SR period 20 ms; 16 UL HARQ processes, max 4 retransmissions, `harq_retx_timeout` 100 ms;
RLC AM DRB: t-Reassembly 35 ms, t-StatusProhibit 0, t-PollRetransmit 45/100 ms, max_retx 8/32;
PUSCH power control open-loop only: `p0_nominal_with_grant` -76 dBm, alpha 1.0 (closed loop off);
PUCCH `p0_nominal` -90 dBm (closed loop off); DL OLLA target 1 %; SSB power announced -16 dBm; k2 = 4.

## What the RAN looks like when healthy (baseline run 2026-09-28 17:55, 5 UEs)

0 RLF, 0 UHD late/overflow/underflow, 0 late HARQ, 0 failed PDCCH/UCI allocations; SR -> grant 3.0 ms,
BSR -> grant 1-3 ms (p90 6 ms); grant -> CRC 4.1 ms flat; TA <= 1.2 us; per-UE SINR flat within +-2 dB;
UL CRC failure 7-11 % per UE (OLLA at target), HARQ completion p99 12-22 ms; UL PRB utilisation 57 % for
2.4 Mbps delivered; DL 2 %. `analysis/exp_ran_audit.py <run>` prints all of this for any run;
`analysis/exp_run_report.py <run> 5` adds the app side and the failure structure.

## Known limits of this RAN (measured), in the order to address them

1. **Near-far / dynamic range.** No per-UE AGC on the B210 and open-loop power control that cannot equalise
   received power: UEs span 30 dB (0 dBFS next to the RX2 antenna .. -30 dBFS). In shared UL slots the
   strongest UE fails 3.6 %, UEs >= 20 dB weaker 23 %; every UE fails 2-4x more when sharing than alone.
   Fix: antenna placement (>= 1-2 m from the RX2 antenna for every phone), then
   `pusch.enable_cl_loop_pw_control: true` with `target_pusch_sinr` ~25 (default 10 is too low) and
   `rx_gain` 40 -> 30 for ADC headroom.
2. **PUCCH margin.** HARQ-ACK DTX 5-14 % per UE, ACK-occasion SINR down to 3.5 dB. Candidate:
   `pucch.p0_nominal` -90 -> -80 dBm, or closed-loop PUCCH power control.
3. **Grant padding.** MAC PDU vs RLC payload: 15-22 % non-payload on high-rate UEs, 54-56 % on low-rate
   ones (BSR bucket upper bounds). About a third of used UL PRBs carry padding; at this efficiency the
   cell saturates near 4 Mbps delivered. Account for it in capacity statements.
4. **UL share.** DDDSU gives 1 UL slot per 2.5 ms (20 %). 5 senders starting at 900 kbps with libwebrtc's
   3x/6x initial probes overflow the UE buffers for ~5 s (BSR up to the top bucket); start senders at
   `--start-bitrate-kbps 300` or stagger them.

## History of the RAN changes (all 2026-09-28, details in docs/NOTES.md)

| change | outcome |
|---|---|
| DDDSU instead of the default DDDDDDSUUU | HARQ RTT 10 -> 7.5 ms; UL BLER unchanged (32 %) -> the floor was elsewhere |
| OLLA 10 % / 20 dB / 0.02 | offsets moved to -19.9 dB and MCS to 5-14 within 10 s, BLER still 22 %, flat in MCS -> not an SNR margin problem |
| 1 TX / 2 RX (same-port) | refused by the UHD driver on B2x0 |
| 2 TX / 2 RX (same-port) | UL BLER 20-25 %; failure tied to DL activity in the preceding slots (78 % vs 9 %) |
| TX/RX + RX2, tx_gain 65 | DL-coupling gone (13.5 % vs 14.1 %); receiver overdriven by the UE power-control reaction |
| TX/RX + RX2, tx_gain 80 (current) | UL failure 9.6 %, SNR-shaped; noise floor -47 dBFS; baseline |
