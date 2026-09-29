# How published video-analytics systems handle resolution / fps (code-level survey, 2026-09-29)

Question: do MEC / video-analytics systems fix resolution and frame rate, or adapt them? Checked the actual
repositories (GitHub API + raw sources), not only the papers. "Fixed" = a per-run constant baked into the input
or config; "knob" = chosen at runtime by the application from an offline-profiled set. None of the systems below
lets the transport (e.g. WebRTC/GoogCC) adapt anything; where bitrate changes, the application changes it.

| System (venue) | Code | Video input | Resolution / fps | Transport | Per-frame deadline |
|---|---|---|---|---|---|
| SMEC (NSDI'26) | github.com/smec-project | MOT17-02 file re-encoded 1080p30 @ 8 Mbps, re-muxed by FFmpeg at file PTS | fixed (no encoder in client) | RTP/UDP up, TCP results; no CC | 100 ms SLO registered by server; late frames dropped |
| ARMA (MobiSys'25) | not public; SMEC ships a baseline client (`arma/video-od`) | same file | fixed in that client; paper adapts bitrate / DNN / GPU (abstract) | RTP + UDP per-frame notify (rnti, size, ts) to RAN | E2E SLO |
| Tutti (MobiCom'22) | baseline client in SMEC repo | same file | fixed | RTP | E2E SLO |
| ASTRA (arXiv 2609.07020) | not public | GStreamer RTSP/TCP H.264 emulated cameras | knob: {832,608,416}px x {30,15,10,5} fps, re-chosen per 60 s window | RTSP; controller sets encoder | E2E latency incl. inference |
| AWStream (SIGCOMM'18) | github.com/awstream/awstream | trace (per-frame sizes CSV) | knob: width{320..1920} x fps{1,2,3,5,10,30} x QP{0..50}, Pareto profile | TCP, app rate control | none |
| DDS (SIGCOMM'20) | github.com/KuntaiDu/dds | PNG dirs | res scale + QP fixed per run; fps not a knob | HTTP POST batches of 15 | none |
| AccMPEG (NSDI'22) | github.com/KuntaiDu/AccMPEG | PNG dirs | 1280x720 fixed, fps fixed; per-macroblock QP | none | none |
| OneAdapt (SoCC'23) | github.com/KuntaiDu/OneAdapt | 10 fps segments | knob: QP, res (7 sizes), b-bias; fps knob present but disabled | none | none |
| Reducto (SIGCOMM'20) | github.com/reducto-sigcomm-2020/reducto | mp4 segments | resolution fixed; fps via frame filtering | none | none |
| Ekya (NSDI'22) | github.com/edge-video-services/ekya | mp4 -> jpeg | fixed; adapts GPU share only | none | none |
| CrossRoI (MMSys'21) | github.com/hongpeng-guo/CrossRoI | AI City videos x5 cams | `FRAME_RATE = 10` fixed, homogeneous; per-cam RoI crops | none (offline) | none; align by frame-index offsets |
| Polly (INFOCOM'23) | none | AICC videos | 1080p @ 10 fps fixed, all cams | - | assumes synchronized cameras |
| Argus (TMC'25) | none | datasets 1080p/720p @ 10-30 fps | fixed per dataset | Ethernet | NTP sync, frames matched if |dt| < 3 ms |
| Gemel (NSDI'23) | none | fixed fps 5-30 | fixed | - | 100 ms per-frame SLA, late frames dropped |
| Vulcan (NSDI'24) | none | 720p30 | knob: sampling 1/2..1/6, resize 0.6..1.0 (BO) | - | per query |
| Elf (MobiCom'21) | github.com/wuyangzhang/elf | KITTI PNG | fixed 1224x370; partitions per frame | ZeroMQ/TCP | hard barrier (wait all servers), no timeout |
| Chameleon, VideoStorm | no public code | - | knobs per paper | - | - |

Take-aways for this testbed
* The MEC/RAN-scheduling line (SMEC, ARMA, Tutti) fixes resolution and fps and treats the stream as a constant-rate
  workload so that SLO satisfaction reflects the scheduler alone. Our WebRTC/GoogCC sender is the odd one out: it
  adapts bitrate continuously and (with MAINTAIN_RESOLUTION) drops frames, so results mix CC behaviour with RAN behaviour.
* Where resolution/fps do change, it is the application choosing from a discrete profiled set at coarse intervals
  (AWStream, ASTRA 60 s windows, Vulcan, OneAdapt); no system lets a transport-level CC decide fps.
* Multi-camera systems keep fps identical across cameras and align by frame index or NTP timestamps (Argus, 3 ms);
  none has a fairness mechanism across cameras and only Elf has an explicit wait-for-all barrier (without deadline).
* Consequence: for the RAN-effect study, run a fixed-fps/fixed-resolution baseline (all three drop layers off,
  see docs/ARCHITECTURE.md / plot_run.py notes) with bitrate as the only free variable; treat knob adaptation
  (RAN-aware, per camera) as the experimental condition, not the baseline.
