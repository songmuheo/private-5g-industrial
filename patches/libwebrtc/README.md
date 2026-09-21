# libwebrtc patches — none

libwebrtc (M120, `branch-heads/6099`, commit `b0b827e0fa26f6dc141807c8bbd41c050029483b`) is used
**stock**. All application-side instrumentation goes through public extension points, so no source
patch is required:

| what we record | libwebrtc extension point used | file |
|---|---|---|
| every RTP / RTCP packet sent and received, and every other `RtcEventLog` event (GoogCC delay/loss estimates, probes, ALR, ICE pair checks/selection, DTLS, route) | `PeerConnectionFactoryDependencies::event_log_factory` (`RtcEventLogFactoryInterface`) | `apps/common/webrtc_tracing.h` |
| 1 s W3C webrtc-stats samples | `PeerConnectionInterface::GetStats` | `apps/common/webrtc_session.h` (`AppendStatsLine`) |
| every encoded frame (rtp_ts, time, size, type, QP, layers) | custom `VideoEncoderFactory` wrapping the stock template factory (`modules/video_coding/g3doc/index.md`) | `apps/common/webrtc_tracing.h` |
| every capture slot | our own `VideoTrackSource` (pattern of `examples/peerconnection/client/conductor.cc` `CapturerTrackSource` / `test/test_video_capturer.cc`) | `apps/common/video_source.h` |
| every frame in/out of the decoder (jitter-buffer exit, decode time, QP) | custom `VideoDecoderFactory` wrapping the stock template factory | `apps/common/webrtc_tracing.h` |
| every decoded frame reaching the app | `VideoSinkInterface` on the received track | `apps/receiver/video_receiver.cc` |
| abs-capture-time per packet (to resolve the per-SSRC RTP timestamp offset) | header extension negotiated via `RtpTransceiverInterface::SetHeaderExtensionsToNegotiate` | `apps/common/webrtc_session.h` |

If a future need cannot be met from the outside (e.g. GCC-internal state), put a `NNNN-*.patch`
here and apply it in `scripts/build/build_libwebrtc.sh` **before** `gn gen`; keep the checkout
itself clean (`git -C third_party/libwebrtc/src status --porcelain` must stay empty for the
`BUILD_INFO.txt` provenance to read `libwebrtc_dirty=no`).
