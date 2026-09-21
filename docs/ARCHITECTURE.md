# Architecture

## 1. Topology

```
        UE site                              gNB PC (ran/, patches/, results/)                    internet host
┌─────────────┐ USB ┌───────┐ NR n78 TDD ┌──────┐ UHD ┌───────────────────────────┐ N2/N3 ┌──────────────┐ NAT ┌────────────────────┐
│ laptop      │─────│ Pixel │ ~~~~~~~~~ │ B210 │─────│ srsRAN gNB + p5g tracer   │───────│ Open5GS 5GC  │─────│ video_receiver     │
│ video_sender│     │ (UE)  │           └──────┘     │ -> results/<run>/gnb      │       │ (docker)     │     │ signaling relay    │
└─────────────┘     └───────┘                        └───────────────────────────┘       └──────────────┘     └────────────────────┘
```

* Core is co-located with the gNB (docker bridge `10.53.1.0/24`; gNB N2/N3 on `10.53.1.1`, Open5GS
  `10.53.1.2`, UE pool `10.45.0.0/16` NATed to the internet).
* Receiver and signaling relay are on the internet side; the UE only needs outbound connectivity. ICE
  works through the UPF NAT because the UE side initiates the checks; `--ice-servers stun:…` adds
  server-reflexive candidates if the internet host is itself behind NAT.
* Downlink experiments swap the two apps; tracing is symmetric.

## 2. What is logged where (all real time)

```
sender                                   gNB (uplink)                                    receiver
capture ─► encode ─► RTP out ─► [UE stack+air] ─► MAC PDU ─► RLC SDU ─► PDCP SDU ─► [core+internet] ─► RTP in ─► decode ─► app
tx-frames  tx-encoded tx-rtp         sched_ul/ul_crc  mac_ul_pdu rlc_ul   pdcp_ul                          rx-rtp    rx-decoded rx-frames
tx-events (GoogCC: delay/loss estimate, probes, ALR; ICE/DTLS)        rx-events                          tx/rx-stats.jsonl (1 s getStats)
```

* Frame identity = RTP timestamp (set by the source from the capture grid). The receiver sees
  `rtp_ts + K` (per-SSRC random offset, RFC 3550) and logs `sender_rtp_ts_est` live from the
  abs-capture-time extension so rows can be matched by eye.
* Packet identity = `(ssrc, seq)`: the gNB PDCP hook parses the RTP header of each user-plane SDU
  (clear even with SRTP) → joins app ledgers to RAN events without touching the UE.
* Per-UE RAN attribution: `gnb_sched_{ul,dl}` (who got which PRBs/MCS/TBS in which slot),
  `gnb_ul_crc` / `gnb_dl_harq_ack`, `gnb_bsr` / `gnb_sr`, `gnb_csi`, `gnb_rlc_ul`.
* Official extras: srsRAN JSON metrics (remote-control WebSocket), stdout metrics table, gNB log,
  Open5GS log; optional stock pcaps (`P5G_GNB_PCAP=1`).

## 3. Design decisions

| decision | chosen | alternatives / why not |
|---|---|---|
| RAN | srsRAN_Project 25.10, native, UHD | OAI (earlier work): srsRAN's COTS-UE + B210 path is the documented tutorial; docker gNB rejected (USB/UHD passthrough) |
| Core | Open5GS via srsRAN's docker recipe, on the gNB PC | any 5GC was acceptable; this one is what the srsRAN COTS-UE tutorial pairs with |
| gNB tracing | build-time patches, fixed circular rings, background writer | JSON metrics alone (1 s aggregates) too coarse; pcap lacks scheduler decisions; `dprintf` in hooks blocks RT threads; linear "whole-run" buffers cost GBs of RSS |
| libwebrtc tracing | official extension points, zero patches | patching `RtcEventLog` (earlier work) modifies third-party code; injecting our `RtcEventLog` gives ns timestamps and all event types |
| Encoded-frame hook | `VideoEncoderFactory` wrapper | `FrameTransformer` makes the M120 send path asynchronous |
| Signaling | TCP JSON-lines relay | SFU / WebSocket stacks add hops and dependencies; P2P is the topology under study |
| App toolchain | libwebrtc's bundled clang + libc++ (static) | system libstdc++ is ABI-incompatible with `use_custom_libcxx=true` |
| Code test | srsUE (srsRAN_4G) over ZeroMQ in netns `ue1`, `run_local_e2e.sh` | kept only to exercise code + logging on one PC; srsUE is 15 kHz FDD only, so the profile differs from the n78 TDD testbed profile |

## 4. Known limits

* RTP `in` timestamps are after SRTP decryption (libwebrtc's earliest public hook), not at the socket.
* libwebrtc emits no per-frame decode event through `RtcEventLog`; decode timing comes from the
  decoder-factory wrapper (`-rx-decoded.csv`).
* Cross-host latencies depend on wall-clock sync.
* Radio profile not yet exercised with hardware; SIMs in `ran/P-Sim.csv` are PLMN 999/70 while the
  configs use 001/01.
