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

## Addendum 2026-09-29 (evening): full texts from ~/ran-mec/docs/reference_papers and dataset access checks

Full-text corrections (pdftotext of the local PDFs):
* ARMA (MobiSys'25): knobs are {bitrate, DNN} chosen per GoP (1 s) per UE; frame rate is "uniform across UEs" and
  resolution is fixed (MOT17-02 "encoded at 1080p resolution at 30 fps and 20 Mbps"). Encoder H.264 via FFmpeg 4.1.9,
  transport SRT. Testbed srsRAN-5G + Pixel 6a, GPS clock at BS, NTP timestamps. §5.3 "Inter-UE Keyframe Interleaving":
  UEs' capture timing is synchronized and IDR offsets are staggered (offset_i = i*floor(fps*GoP/N_UEs)) so keyframes
  (3-5x larger) of different UEs never collide in the uplink. Datasets: MOT17 (5 CCTV street videos), BDD (10 dashcam),
  YTFace (6 mobile). No public code; SMEC's arma/ client is a fixed-rate re-implementation.
* Tutti (MobiCom'22): per-frame JPEG, 720p at 30 fps from ILSVRC2015-VID image sets, srsRAN LTE + free5GC, Nexus 5 /
  Axon 10 UEs, driving car. Fixed configuration; RAN-side PRB pre-grant is the only adaptation.
* Pendulum (2025, same group): {bitrate, DNN} per user, MOT17-11 720p30 at 8 Mbps; resolution/fps fixed.
* CORA (PACMNET'25): not video; fixed-size image requests (e.g. 3x112x112), OAI + Open5GS, 400 ms SLO.
* JCAB (INFOCOM'20) and VideoEdge (SEC'18): resolution and frame-sampling rate ARE the knobs (JCAB simulation;
  VideoEdge 5 resolutions x 5 sampling rates per query plan, chosen at planning time).
* Multi-camera papers: CrossRoI and Multi-View Scheduling use AI City Challenge 2020/2021 (5 synchronized cameras,
  1080p, 10 fps); PIB and CollabCam use WILDTRACK (7 cams 1080p, 400 annotated frames at 2 fps); CoLA (2026) uses the
  Warehouse MultiCam RF Dataset (16 fixed cams, Gazebo+Sionna, one robot). All keep fps identical across cameras.
* Fusion-timeliness metrics in the AoI folder: Age of Collection (AoC, IEEE IoT-J 2024) "decreases only when all
  cooperative packets are received"; Age of Correlated Information (INFOCOM WKSHPS 2018) peak age per scene over the
  cameras covering it; GA-Joint "synchronization penalty" = spread of upload completion times within a group;
  CoLA "age of latent". These are the formal versions of "all K frames must arrive by the deadline".

Dataset access checks (done directly, 2026-09-29):

| Dataset | Access | Cameras / scene | Video | GT | Fit |
|---|---|---|---|---|---|
| NVIDIA PhysicalAI-SmartSpaces (HF, CC-BY-4.0, not gated) | direct download verified (curl); 2025: 15 train warehouses, 23 scenes total, 70 GB w/o depth | Warehouse_000: 25 cams, calibrated, 1080p30 | H.264 High, 300 s = 9000 frames per camera, ~4.5 Mbps (ffprobe) | per-frame 2D bbox per camera + 3D box + IDs; classes Person, Forklift, NovaCarter, Transporter, humanoids; GT for train/val/test (2026 real captures withheld) | best: warehouse, many synced cams, 300 s clips, full GT; synthetic (Omniverse), 2026 adds 2 real warehouses w/o GT |
| Warehouse MultiCam RF Dataset (Zenodo 20315129, CC-BY-4.0) | open, 82.6 GB (dataset_1.zip 10.3 GB) | 16 fixed RGB cams + Sionna RF | images per timestamp, res/fps not stated | robot pose only, no detection labels | RF+vision novelty, but single robot and no labels |
| MMPTRACK (ICCV'21 workshop) | e-mail + signed terms | 5 envs incl. "industry", overlapping FoV | 640x320 @ 15 fps JPEG frames, 5 h train | 2D bbox + top-down positions + IDs | real multi-person industrial scene but low resolution and gated |
| WILDTRACK (EPFL) | direct (Google Drive) | 7 GoPro 1080p | 60 fps video, 10 fps frames, 400 annotated frames @ 2 fps | bbox + ground-grid IDs | campus, sparse GT |
| MultiviewX (Unity) | OneDrive | 6 cams 1080p | frames | 2D/3D bbox | playground, not industrial |
| InHARD (Zenodo, CC-BY-4.0) | open, 50 GB 7z | 3 C920 views of one operator, 1280x720 | fps not stated | 13 action classes (ANVIL), skeleton 120 Hz | industrial but single workstation, action labels not detection |
| IKEA ASM / Assembly101 (CC-BY-NC-4.0) | Google Drive / HF scripts | 3 views / 8 static + 4 ego | not stated on page | actions, poses, part segmentation (1 % manual) | assembly tasks, non-commercial, not area surveillance |
| Kendo (current asset) | local | 6 of 7 views 1024x768 -> 720p | 10 s looped | none | pipeline check only |

Decision: adopt PhysicalAI-SmartSpaces 2025 warehouses as the multi-view source (fits 300 s runs and 5-25 UEs);
keep Kendo for smoke tests. Local probe files: scratchpad ds/calib_wh000.json, ds/cam0000.mp4.
