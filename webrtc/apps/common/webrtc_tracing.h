// libwebrtc tracing hooks — implemented against public extension points only (no libwebrtc patch):
//   1. RtpPacketLedger / RtpPacketLedgerFactory: RtcEventLog implementation injected through
//      PeerConnectionFactoryDependencies::event_log_factory -> -rtp.csv, -rtcp.csv, -events.csv
//   2. LedgerVideoEncoderFactory: VideoEncoderFactory wrapper -> -tx-encoded.csv, -tx-encoder-rates.csv
//   3. LedgerVideoDecoderFactory: VideoDecoderFactory wrapper -> -rx-decoded.csv
//   4. LedgerNetworkControllerFactory: NetworkControllerFactoryInterface wrapper around the stock
//      GoogCC factory (PeerConnectionFactoryDependencies::network_controller_factory) -> -tx-cc.csv

#ifndef P5G_APPS_COMMON_WEBRTC_TRACING_H
#define P5G_APPS_COMMON_WEBRTC_TRACING_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/rtc_event_log/rtc_event.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/rtc_event_log/rtc_event_log_factory_interface.h"
#include "api/transport/goog_cc_factory.h"
#include "api/transport/network_control.h"
#include "api/video/encoded_image.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "logging/rtc_event_log/events/rtc_event_alr_state.h"
#include "logging/rtc_event_log/events/rtc_event_bwe_update_delay_based.h"
#include "logging/rtc_event_log/events/rtc_event_bwe_update_loss_based.h"
#include "logging/rtc_event_log/events/rtc_event_dtls_transport_state.h"
#include "logging/rtc_event_log/events/rtc_event_dtls_writable_state.h"
#include "logging/rtc_event_log/events/rtc_event_ice_candidate_pair.h"
#include "logging/rtc_event_log/events/rtc_event_ice_candidate_pair_config.h"
#include "logging/rtc_event_log/events/rtc_event_probe_cluster_created.h"
#include "logging/rtc_event_log/events/rtc_event_probe_result_failure.h"
#include "logging/rtc_event_log/events/rtc_event_probe_result_success.h"
#include "logging/rtc_event_log/events/rtc_event_route_change.h"
#include "logging/rtc_event_log/events/rtc_event_rtcp_packet_incoming.h"
#include "logging/rtc_event_log/events/rtc_event_rtcp_packet_outgoing.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_incoming.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_outgoing.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "app_util.h"
#include "trace_ring.h"

// Per-packet RTP / RTCP ledger, injected through libwebrtc's official extension point
// PeerConnectionFactoryDependencies::event_log_factory (RtcEventLogFactoryInterface).
//
// libwebrtc already calls RtcEventLog::Log() for every RTP packet it sends
// (RtpSenderEgress::SendPacket, right after the socket write) and receives
// (Call::DeliverRtpPacket, after SRTP decryption, before stream demux) and for every RTCP packet
// in both directions. Instead of the stock RtcEventLogImpl (per-packet mutex, millisecond event
// clock, protobuf serialisation) we implement the interface ourselves and write one POD row per
// packet into a lock-free ring (trace_ring.h). No libwebrtc source is modified.
//
// Timestamps are taken here with CLOCK_MONOTONIC (+ CLOCK_REALTIME): "out" ≈ handed to the UDP
// socket, "in" ≈ earliest point available through public API after the socket read. Kernel
// send/arrival timestamps are not exposed by libwebrtc; use tcpdump on the host for those.
//
// Files (per PeerConnection): <basename>-rtp.csv, <basename>-rtcp.csv, <basename>-events.csv.

namespace p5g {

// ---- RTP row --------------------------------------------------------------------------------
struct RtpPacketRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  uint32_t rtp_ts;         // wire space
  uint32_t ssrc;
  int32_t pkt_bytes;       // header + payload + padding
  int32_t hdr_bytes;       // incl. extensions
  int32_t pad_bytes;
  int32_t payload_bytes;
  int32_t probe_cluster_id; // BWE probe cluster (out only), -1 otherwise
  uint16_t seq;
  uint8_t pt;
  uint8_t marker;
  uint8_t csrc_count;
  uint8_t has_ext;
  uint8_t dir;             // 0 = out, 1 = in
};

inline constexpr const char* kRtpPacketHeader =
    "log_mono_ns,log_wall_ns,dir,seq,rtp_ts,ssrc,pt,marker,pkt_bytes,hdr_bytes,pad_bytes,"
    "payload_bytes,csrc_count,has_ext,probe_cluster_id";

inline void FormatRtpPacketRow(std::FILE* f, const RtpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%d\n", (long long)r.log_mono_ns,
               (long long)r.log_wall_ns, r.dir ? "in" : "out", r.seq, r.rtp_ts, r.ssrc, r.pt,
               r.marker, r.pkt_bytes, r.hdr_bytes, r.pad_bytes, r.payload_bytes, r.csrc_count,
               r.has_ext, r.probe_cluster_id);
}

// ---- RTCP row -------------------------------------------------------------------------------
// A compound RTCP packet is summarised as up to kMaxRtcpParts (packet type, fmt/count) pairs, e.g.
// 200 (SR), 201 (RR), 205/1 (NACK), 205/15 (transport-cc feedback), 206/1 (PLI), 206/4 (FIR),
// 206/15 (REMB). RFC 3550 §6, RFC 4585 §6, RFC 5104, draft-holmer-rmcat-transport-wide-cc.
inline constexpr int kMaxRtcpParts = 6;
struct RtcpPacketRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  uint32_t sender_ssrc;    // SSRC in the first RTCP header (0 if malformed)
  int32_t bytes;
  uint8_t dir;
  uint8_t num_parts;
  uint8_t pt[kMaxRtcpParts];
  uint8_t fmt[kMaxRtcpParts];
};

inline constexpr const char* kRtcpPacketHeader =
    "log_mono_ns,log_wall_ns,dir,bytes,sender_ssrc,num_parts,parts";

inline void FormatRtcpPacketRow(std::FILE* f, const RtcpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%d,%u,%u,", (long long)r.log_mono_ns, (long long)r.log_wall_ns,
               r.dir ? "in" : "out", r.bytes, r.sender_ssrc, r.num_parts);
  for (int i = 0; i < r.num_parts && i < kMaxRtcpParts; ++i)
    std::fprintf(f, "%s%u/%u", i ? ";" : "", r.pt[i], r.fmt[i]);
  std::fputc('\n', f);
}

using RtpPacketTrace = TraceRing<RtpPacketRow, /*kMultiWriter=*/true>;
using RtcpPacketTrace = TraceRing<RtcpPacketRow, /*kMultiWriter=*/true>;

// ---- Everything else libwebrtc reports through RtcEventLog (official event types) -------------
// One generic row: `event` names the type, a..f are its integer fields (meaning per event, see
// docs/TRACE_SCHEMA.md). These are the GoogCC internals (delay/loss-based estimates, probes, ALR),
// transport state (ICE pair checks / selection, DTLS, route) and decoded frames — as emitted by the
// stock stack, without touching its code.
enum class LedgerEventKind : uint8_t {
  bwe_delay = 0,       // a=bitrate_bps b=detector_state(0 normal,1 underusing,2 overusing)
  bwe_loss,            // a=bitrate_bps b=fraction_loss(0..255) c=total_packets
  probe_created,       // a=cluster_id b=bitrate_bps c=min_probes d=min_bytes
  probe_success,       // a=cluster_id b=measured_bitrate_bps
  probe_failure,       // a=cluster_id b=reason(0 invalid_interval,1 invalid_ratio,2 timeout)
  alr_state,           // a=in_alr
  route_change,        // a=connected b=overhead_bytes
  ice_pair_event,      // a=type(0 check_sent,1 check_received,2 response_sent,3 response_received) b=pair_id c=transaction_id
  ice_pair_config,     // a=type(0 added,1 updated,2 destroyed,3 selected) b=pair_id
  dtls_state,          // a=DtlsTransportState(0 new,1 connecting,2 connected,3 closed,4 failed)
  dtls_writable,       // a=writable
  stream_config,       // a=1 send / 0 receive (config events; content not decoded)
  kCount
};
inline constexpr const char* kLedgerEventNames[] = {
    "bwe_delay", "bwe_loss", "probe_created", "probe_success", "probe_failure", "alr_state",
    "route_change", "ice_pair_event", "ice_pair_config", "dtls_state", "dtls_writable",
    "stream_config"};

struct LedgerEventRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  int64_t a, b, c, d, e, f;
  uint8_t kind;
};
inline constexpr const char* kLedgerEventHeader = "log_mono_ns,log_wall_ns,event,a,b,c,d,e,f";
inline void FormatLedgerEventRow(std::FILE* fp, const LedgerEventRow& r) {
  std::fprintf(fp, "%lld,%lld,%s,%lld,%lld,%lld,%lld,%lld,%lld\n", (long long)r.log_mono_ns,
               (long long)r.log_wall_ns,
               r.kind < static_cast<uint8_t>(LedgerEventKind::kCount) ? kLedgerEventNames[r.kind] : "unknown",
               (long long)r.a, (long long)r.b, (long long)r.c, (long long)r.d, (long long)r.e, (long long)r.f);
}
using LedgerEventTrace = TraceRing<LedgerEventRow, /*kMultiWriter=*/true>;

// ---- RtcEventLog implementation --------------------------------------------------------------
class RtpPacketLedger : public webrtc::RtcEventLog {
 public:
  RtpPacketLedger(std::shared_ptr<RtpPacketTrace> rtp, std::shared_ptr<RtcpPacketTrace> rtcp,
                  std::shared_ptr<LedgerEventTrace> events)
      : rtp_(std::move(rtp)), rtcp_(std::move(rtcp)), events_(std::move(events)) {}

  // We write our own files; libwebrtc's output path is unused but the interface is pure virtual.
  bool StartLogging(std::unique_ptr<webrtc::RtcEventLogOutput>, int64_t) override { return true; }
  void StopLogging() override {}
  void StopLogging(std::function<void()> callback) override { callback(); }

  // Hot path (pacer / network thread): no allocation, no lock, no I/O.
  void Log(std::unique_ptr<webrtc::RtcEvent> event) override {
    using T = webrtc::RtcEvent::Type;
    const T type = event->GetType();
    switch (type) {
      case T::RtpPacketOutgoing:
      case T::RtpPacketIncoming: {
        RtpPacketRow r{};
        r.log_mono_ns = NowMonoNs();
        r.log_wall_ns = NowWallNs();
        if (type == T::RtpPacketOutgoing) {
          const auto& e = static_cast<const webrtc::RtcEventRtpPacketOutgoing&>(*event);
          FillRtp(r, e);
          r.dir = 0;
          r.probe_cluster_id = e.probe_cluster_id();
        } else {
          const auto& e = static_cast<const webrtc::RtcEventRtpPacketIncoming&>(*event);
          FillRtp(r, e);
          r.dir = 1;
          r.probe_cluster_id = -1;
        }
        rtp_->Write(r);
        break;
      }
      case T::RtcpPacketOutgoing:
      case T::RtcpPacketIncoming: {
        RtcpPacketRow r{};
        r.log_mono_ns = NowMonoNs();
        r.log_wall_ns = NowWallNs();
        if (type == T::RtcpPacketOutgoing) {
          const auto& e = static_cast<const webrtc::RtcEventRtcpPacketOutgoing&>(*event);
          FillRtcp(r, e.packet().data(), e.packet().size());
          r.dir = 0;
        } else {
          const auto& e = static_cast<const webrtc::RtcEventRtcpPacketIncoming&>(*event);
          FillRtcp(r, e.packet().data(), e.packet().size());
          r.dir = 1;
        }
        rtcp_->Write(r);
        break;
      }
      default:
        LogOther(type, *event);
        break;
    }
  }

 private:
  void LogOther(webrtc::RtcEvent::Type type, const webrtc::RtcEvent& ev) {
    using T = webrtc::RtcEvent::Type;
    LedgerEventRow r{};
    r.log_mono_ns = NowMonoNs();
    r.log_wall_ns = NowWallNs();
    r.a = r.b = r.c = r.d = r.e = r.f = -1;
    switch (type) {
      case T::BweUpdateDelayBased: {
        const auto& e = static_cast<const webrtc::RtcEventBweUpdateDelayBased&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::bwe_delay);
        r.a = e.bitrate_bps();
        r.b = static_cast<int64_t>(e.detector_state());
        break;
      }
      case T::BweUpdateLossBased: {
        const auto& e = static_cast<const webrtc::RtcEventBweUpdateLossBased&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::bwe_loss);
        r.a = e.bitrate_bps();
        r.b = e.fraction_loss();
        r.c = e.total_packets();
        break;
      }
      case T::ProbeClusterCreated: {
        const auto& e = static_cast<const webrtc::RtcEventProbeClusterCreated&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::probe_created);
        r.a = e.id();
        r.b = e.bitrate_bps();
        r.c = e.min_probes();
        r.d = e.min_bytes();
        break;
      }
      case T::ProbeResultSuccess: {
        const auto& e = static_cast<const webrtc::RtcEventProbeResultSuccess&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::probe_success);
        r.a = e.id();
        r.b = e.bitrate_bps();
        break;
      }
      case T::ProbeResultFailure: {
        const auto& e = static_cast<const webrtc::RtcEventProbeResultFailure&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::probe_failure);
        r.a = e.id();
        r.b = static_cast<int64_t>(e.failure_reason());
        break;
      }
      case T::AlrStateEvent: {
        const auto& e = static_cast<const webrtc::RtcEventAlrState&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::alr_state);
        r.a = e.in_alr() ? 1 : 0;
        break;
      }
      case T::RouteChangeEvent: {
        const auto& e = static_cast<const webrtc::RtcEventRouteChange&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::route_change);
        r.a = e.connected() ? 1 : 0;
        r.b = e.overhead();
        break;
      }
      case T::IceCandidatePairEvent: {
        const auto& e = static_cast<const webrtc::RtcEventIceCandidatePair&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::ice_pair_event);
        r.a = static_cast<int64_t>(e.type());
        r.b = e.candidate_pair_id();
        r.c = e.transaction_id();
        break;
      }
      case T::IceCandidatePairConfig: {
        const auto& e = static_cast<const webrtc::RtcEventIceCandidatePairConfig&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::ice_pair_config);
        r.a = static_cast<int64_t>(e.type());
        r.b = e.candidate_pair_id();
        break;
      }
      case T::DtlsTransportState: {
        const auto& e = static_cast<const webrtc::RtcEventDtlsTransportState&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::dtls_state);
        r.a = static_cast<int64_t>(e.dtls_transport_state());
        break;
      }
      case T::DtlsWritableState: {
        const auto& e = static_cast<const webrtc::RtcEventDtlsWritableState&>(ev);
        r.kind = static_cast<uint8_t>(LedgerEventKind::dtls_writable);
        r.a = e.writable() ? 1 : 0;
        break;
      }
      case T::VideoSendStreamConfig:
      case T::VideoReceiveStreamConfig:
        r.kind = static_cast<uint8_t>(LedgerEventKind::stream_config);
        r.a = type == T::VideoSendStreamConfig ? 1 : 0;
        break;
      default:
        return;  // audio / NetEq / generic-transport / remote-estimate (no public accessors) events
    }
    events_->Write(r);
  }

  template <typename Event>
  static void FillRtp(RtpPacketRow& r, const Event& e) {
    r.seq = e.SequenceNumber();
    r.rtp_ts = e.Timestamp();
    r.ssrc = e.Ssrc();
    r.pt = e.PayloadType();
    r.marker = e.Marker() ? 1 : 0;
    r.pkt_bytes = static_cast<int32_t>(e.packet_length());
    r.hdr_bytes = static_cast<int32_t>(e.header_length());
    r.pad_bytes = static_cast<int32_t>(e.padding_length());
    r.payload_bytes = static_cast<int32_t>(e.payload_length());
    const auto h = e.RawHeader();  // RFC 3550 §5.1 first byte: V(2) P(1) X(1) CC(4)
    r.csrc_count = h.empty() ? 0 : static_cast<uint8_t>(h[0] & 0x0F);
    r.has_ext = h.empty() ? 0 : static_cast<uint8_t>((h[0] >> 4) & 0x01);
  }

  // Walk a compound RTCP packet: each part has a 4-byte common header
  // V(2) P(1) RC/FMT(5) | PT(8) | length(16, in 32-bit words minus one).
  static void FillRtcp(RtcpPacketRow& r, const uint8_t* p, size_t len) {
    r.bytes = static_cast<int32_t>(len);
    if (len >= 8) {
      r.sender_ssrc = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
    }
    size_t off = 0;
    while (off + 4 <= len && r.num_parts < kMaxRtcpParts) {
      const uint8_t fmt = p[off] & 0x1F;
      const uint8_t pt = p[off + 1];
      const size_t part_len = (size_t(p[off + 2]) << 8 | p[off + 3]) * 4 + 4;
      r.pt[r.num_parts] = pt;
      r.fmt[r.num_parts] = fmt;
      ++r.num_parts;
      if (part_len == 0) break;
      off += part_len;
    }
  }

  const std::shared_ptr<RtpPacketTrace> rtp_;
  const std::shared_ptr<RtcpPacketTrace> rtcp_;
  const std::shared_ptr<LedgerEventTrace> events_;
};

// Factory to plug into PeerConnectionFactoryDependencies::event_log_factory.
// One PeerConnectionFactory can create several PeerConnections; Create() cannot know which. Both
// apps create PCs serially on the signaling thread, so the app names the next ledger just before
// creating the PC. Creating a PC without a pending name is fatal (silent loss of packet records).
class RtpPacketLedgerFactory : public webrtc::RtcEventLogFactoryInterface {
 public:
  // basename: e.g. "<trace_dir>/<stream>-tx" -> "<stream>-tx-rtp.csv", "<stream>-tx-rtcp.csv".
  void SetNextLedgerBasename(std::string basename) { next_ = std::move(basename); }

  std::unique_ptr<webrtc::RtcEventLog> Create(webrtc::RtcEventLog::EncodingType) const override {
    if (next_.empty()) P5G_FATAL("RtpPacketLedgerFactory: PeerConnection created without ledger name");
    for (const auto& t : rtp_traces_)
      if (t->path() == next_ + "-rtp.csv") P5G_FATAL("RtpPacketLedgerFactory: ledger path reused " << next_);
    auto rtp = std::make_shared<RtpPacketTrace>(next_ + "-rtp.csv", kRtpPacketHeader,
                                                &FormatRtpPacketRow, kPacketTraceCapacity);
    auto rtcp = std::make_shared<RtcpPacketTrace>(next_ + "-rtcp.csv", kRtcpPacketHeader,
                                                  &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    auto events = std::make_shared<LedgerEventTrace>(next_ + "-events.csv", kLedgerEventHeader,
                                                     &FormatLedgerEventRow, kPacketTraceCapacity / 4);
    next_.clear();
    rtp_traces_.push_back(rtp);
    rtcp_traces_.push_back(rtcp);
    event_traces_.push_back(events);
    return std::make_unique<RtpPacketLedger>(std::move(rtp), std::move(rtcp), std::move(events));
  }
  std::unique_ptr<webrtc::RtcEventLog> CreateRtcEventLog(
      webrtc::RtcEventLog::EncodingType type) override {
    return Create(type);
  }

  // After all PeerConnections are closed.
  void CloseAll() {
    for (auto& t : rtp_traces_) t->Close();
    for (auto& t : rtcp_traces_) t->Close();
    for (auto& t : event_traces_) t->Close();
  }

 private:
  mutable std::string next_;
  mutable std::vector<std::shared_ptr<RtpPacketTrace>> rtp_traces_;
  mutable std::vector<std::shared_ptr<RtcpPacketTrace>> rtcp_traces_;
  mutable std::vector<std::shared_ptr<LedgerEventTrace>> event_traces_;
};

}  // namespace p5g

// Encoded-frame ledger: one row per frame the video encoder produced.
//
// Installed through libwebrtc's documented codec injection point (a custom VideoEncoderFactory,
// modules/video_coding/g3doc/index.md). The wrapper delegates everything to the real encoder and
// only intercepts the EncodedImageCallback to record (rtp_ts, encode-complete time, size, frame
// type, QP, layer indices, ...) before forwarding the image unchanged. No libwebrtc source changes.
//
// Why here and not a FrameTransformer: with an encoder->packetizer FrameTransformer installed, M120
// switches the send path from synchronous to an asynchronous task-queue hop
// (modules/rtp_rtcp/source/rtp_sender_video.cc, "The frame will be sent async once transformed"),
// which would change the very latency we measure. The encoder callback keeps the stock send path.
//
// Key space: EncodedImage::RtpTimestamp() at this point is still the *sender-space* RTP timestamp
// (the per-SSRC random offset is added later in RtpVideoSender), i.e. identical to the value in
// <stream>-tx-frames.csv. Join directly on rtp_ts, no offset correction.

namespace p5g {

// <stream>-tx-encoded.csv
struct EncodedFrameRow {
  uint32_t rtp_ts;          // sender space
  int64_t encode_done_mono_ns;
  int64_t encode_done_wall_ns;
  int32_t bytes;
  int32_t width, height;    // actual encoded resolution (after adaptation)
  int32_t frame_type;       // webrtc::VideoFrameType: 0 empty, 3 key, 4 delta
  int32_t qp;               // -1 if not reported
  int32_t temporal_idx;     // -1 if none
  int32_t spatial_idx;      // -1 if none
  int32_t simulcast_idx;    // -1 if none
  int64_t capture_time_ms;  // EncodedImage::capture_time_ms_ (source timestamp_us / 1000)
  int64_t ntp_time_ms;      // EncodedImage::NtpTimeMs()
  int32_t codec;            // webrtc::VideoCodecType from CodecSpecificInfo (-1 if absent)
  int32_t is_idr;           // H264: idr_frame; other codecs -1
  int32_t at_target_quality;
};

inline constexpr const char* kEncodedFrameHeader =
    "rtp_ts,encode_done_mono_ns,encode_done_wall_ns,bytes,width,height,frame_type,qp,temporal_idx,"
    "spatial_idx,simulcast_idx,capture_time_ms,ntp_time_ms,codec,is_idr,at_target_quality";

inline void FormatEncodedFrameRow(std::FILE* f, const EncodedFrameRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%d,%d,%d\n", r.rtp_ts,
               (long long)r.encode_done_mono_ns, (long long)r.encode_done_wall_ns, r.bytes,
               r.width, r.height, r.frame_type, r.qp, r.temporal_idx, r.spatial_idx,
               r.simulcast_idx, (long long)r.capture_time_ms, (long long)r.ntp_time_ms, r.codec,
               r.is_idr, r.at_target_quality);
}

// Encoder callbacks come from the encoder task queue; simulcast/SVC adapters may run several
// encoders, so allow multiple writers.
using EncodedFrameTrace = TraceRing<EncodedFrameRow, /*kMultiWriter=*/true>;

// <stream>-tx-encoder-rates.csv: one row per VideoEncoder::SetRates() call, i.e. every time the
// stock rate controller (GoogCC estimate -> VideoStreamEncoder/BitrateAllocator) hands the encoder a
// new target. This is the "target bitrate" the encoder is asked to hit, as opposed to the estimate
// (`-events.csv` bwe_*) and the bytes actually sent (`-rtp.csv`). Official API: api/video_codecs/
// video_encoder.h RateControlParameters (target_bitrate, bitrate, bandwidth_allocation, framerate_fps).
struct EncoderRateRow {
  int64_t set_mono_ns;
  int64_t set_wall_ns;
  int64_t target_bps;              // RateControlParameters::target_bitrate.get_sum_bps()
  int64_t allocated_bps;           // RateControlParameters::bitrate.get_sum_bps() (after headroom/limits)
  int64_t bandwidth_allocation_bps;// RateControlParameters::bandwidth_allocation (network estimate share)
  double framerate_fps;
  int32_t num_active_spatial_layers;
};

inline constexpr const char* kEncoderRateHeader =
    "set_mono_ns,set_wall_ns,target_bps,allocated_bps,bandwidth_allocation_bps,framerate_fps,"
    "num_active_spatial_layers";

inline void FormatEncoderRateRow(std::FILE* f, const EncoderRateRow& r) {
  std::fprintf(f, "%lld,%lld,%lld,%lld,%lld,%.3f,%d\n", (long long)r.set_mono_ns,
               (long long)r.set_wall_ns, (long long)r.target_bps, (long long)r.allocated_bps,
               (long long)r.bandwidth_allocation_bps, r.framerate_fps, r.num_active_spatial_layers);
}

using EncoderRateTrace = TraceRing<EncoderRateRow, /*kMultiWriter=*/true>;

// ---------------------------------------------------------------------------------------------
// <stream>-tx-cc.csv: the congestion controller's output, one row per NetworkControlUpdate that
// carries something (target rate, pacer config, congestion window or probes). This is GoogCC's
// *final* answer after delay/loss estimation, pushback and constraints — the value that feeds
// BitrateAllocator and thus the encoder target (`-tx-encoder-rates.csv`). `bwe_delay`/`bwe_loss`
// in `-events.csv` are the two sub-estimators one step earlier.
// Official injection point: PeerConnectionFactoryDependencies::network_controller_factory
// (api/transport/network_control.h). The wrapper forwards every call to the stock
// GoogCcNetworkControllerFactory (constructed exactly like RtpTransportControllerSend's fallback)
// and records the returned update; it never modifies it.
struct CcUpdateRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  int32_t trigger;                 // CcTrigger below: which controller input produced the update
  int64_t target_bps;              // TargetTransferRate::target_rate            (-1 absent)
  int64_t stable_target_bps;       // TargetTransferRate::stable_target_rate     (-1 absent)
  int64_t est_bandwidth_bps;       // NetworkEstimate::bandwidth                 (-1 absent/inf)
  int64_t rtt_us;                  // NetworkEstimate::round_trip_time           (-1 absent/inf)
  double loss_rate_ratio;          // NetworkEstimate::loss_rate_ratio 0..1      (-1 absent)
  int64_t bwe_period_ms;           // NetworkEstimate::bwe_period                (-1 absent/inf)
  double cwnd_reduce_ratio;        // TargetTransferRate::cwnd_reduce_ratio      (-1 absent)
  int64_t pacer_rate_bps;          // PacerConfig::data_rate()                   (-1 absent)
  int64_t pad_rate_bps;            // PacerConfig::pad_rate()                    (-1 absent)
  int64_t cwnd_bytes;              // congestion_window                          (-1 absent/inf)
  int32_t num_probe_clusters;      // probe_cluster_configs.size()
};

enum class CcTrigger : int32_t {
  process_interval = 0, transport_feedback, transport_loss, rtt_update, route_change,
  network_availability, target_rate_constraints, streams_config, remote_bitrate_report,
  network_state_estimate, sent_packet, received_packet
};

inline constexpr const char* kCcUpdateHeader =
    "log_mono_ns,log_wall_ns,trigger,target_bps,stable_target_bps,est_bandwidth_bps,rtt_us,"
    "loss_rate_ratio,bwe_period_ms,cwnd_reduce_ratio,pacer_rate_bps,pad_rate_bps,cwnd_bytes,"
    "num_probe_clusters";

inline void FormatCcUpdateRow(std::FILE* f, const CcUpdateRow& r) {
  std::fprintf(f, "%lld,%lld,%d,%lld,%lld,%lld,%lld,%.4f,%lld,%.3f,%lld,%lld,%lld,%d\n",
               (long long)r.log_mono_ns, (long long)r.log_wall_ns, r.trigger,
               (long long)r.target_bps, (long long)r.stable_target_bps,
               (long long)r.est_bandwidth_bps, (long long)r.rtt_us, r.loss_rate_ratio,
               (long long)r.bwe_period_ms, r.cwnd_reduce_ratio, (long long)r.pacer_rate_bps,
               (long long)r.pad_rate_bps, (long long)r.cwnd_bytes, r.num_probe_clusters);
}

using CcUpdateTrace = TraceRing<CcUpdateRow, /*kMultiWriter=*/true>;

class LedgerNetworkController : public webrtc::NetworkControllerInterface {
 public:
  LedgerNetworkController(std::unique_ptr<webrtc::NetworkControllerInterface> inner,
                          CcUpdateTrace* trace)
      : inner_(std::move(inner)), trace_(trace) {}

  webrtc::NetworkControlUpdate OnNetworkAvailability(webrtc::NetworkAvailability m) override {
    return Observe(CcTrigger::network_availability, [&] { return inner_->OnNetworkAvailability(m); });
  }
  webrtc::NetworkControlUpdate OnNetworkRouteChange(webrtc::NetworkRouteChange m) override {
    return Observe(CcTrigger::route_change, [&] { return inner_->OnNetworkRouteChange(m); });
  }
  webrtc::NetworkControlUpdate OnProcessInterval(webrtc::ProcessInterval m) override {
    return Observe(CcTrigger::process_interval, [&] { return inner_->OnProcessInterval(m); });
  }
  webrtc::NetworkControlUpdate OnRemoteBitrateReport(webrtc::RemoteBitrateReport m) override {
    return Observe(CcTrigger::remote_bitrate_report, [&] { return inner_->OnRemoteBitrateReport(m); });
  }
  webrtc::NetworkControlUpdate OnRoundTripTimeUpdate(webrtc::RoundTripTimeUpdate m) override {
    return Observe(CcTrigger::rtt_update, [&] { return inner_->OnRoundTripTimeUpdate(m); });
  }
  webrtc::NetworkControlUpdate OnSentPacket(webrtc::SentPacket m) override {
    return Observe(CcTrigger::sent_packet, [&] { return inner_->OnSentPacket(m); });
  }
  webrtc::NetworkControlUpdate OnReceivedPacket(webrtc::ReceivedPacket m) override {
    return Observe(CcTrigger::received_packet, [&] { return inner_->OnReceivedPacket(m); });
  }
  webrtc::NetworkControlUpdate OnStreamsConfig(webrtc::StreamsConfig m) override {
    return Observe(CcTrigger::streams_config, [&] { return inner_->OnStreamsConfig(m); });
  }
  webrtc::NetworkControlUpdate OnTargetRateConstraints(webrtc::TargetRateConstraints m) override {
    return Observe(CcTrigger::target_rate_constraints, [&] { return inner_->OnTargetRateConstraints(m); });
  }
  webrtc::NetworkControlUpdate OnTransportLossReport(webrtc::TransportLossReport m) override {
    return Observe(CcTrigger::transport_loss, [&] { return inner_->OnTransportLossReport(m); });
  }
  webrtc::NetworkControlUpdate OnTransportPacketsFeedback(
      webrtc::TransportPacketsFeedback m) override {
    return Observe(CcTrigger::transport_feedback, [&] { return inner_->OnTransportPacketsFeedback(m); });
  }
  webrtc::NetworkControlUpdate OnNetworkStateEstimate(webrtc::NetworkStateEstimate m) override {
    return Observe(CcTrigger::network_state_estimate, [&] { return inner_->OnNetworkStateEstimate(m); });
  }

 private:
  // Pure observation: the update is returned unchanged; rows only for updates that carry data.
  // NetworkControlUpdate has no move constructor (user-declared copy ctor), so it is constructed
  // in place from the stock call and returned as a named local (elided) — no extra copies.
  template <class F>
  webrtc::NetworkControlUpdate Observe(CcTrigger trigger, F&& call) {
    webrtc::NetworkControlUpdate u = call();
    Record(u, trigger);
    return u;
  }
  void Record(const webrtc::NetworkControlUpdate& u, CcTrigger trigger) {
    if (!trace_ || !u.has_updates()) return;
    CcUpdateRow r{};
    r.log_mono_ns = NowMonoNs();
    r.log_wall_ns = NowWallNs();
    r.trigger = static_cast<int32_t>(trigger);
    r.target_bps = r.stable_target_bps = r.est_bandwidth_bps = r.rtt_us = r.bwe_period_ms = -1;
    r.loss_rate_ratio = r.cwnd_reduce_ratio = -1.0;
    r.pacer_rate_bps = r.pad_rate_bps = r.cwnd_bytes = -1;
    if (u.target_rate) {
      const auto& tr = *u.target_rate;
      r.target_bps = tr.target_rate.bps();
      r.stable_target_bps = tr.stable_target_rate.bps();
      if (tr.network_estimate.bandwidth.IsFinite()) r.est_bandwidth_bps = tr.network_estimate.bandwidth.bps();
      if (tr.network_estimate.round_trip_time.IsFinite()) r.rtt_us = tr.network_estimate.round_trip_time.us();
      if (tr.network_estimate.bwe_period.IsFinite()) r.bwe_period_ms = tr.network_estimate.bwe_period.ms();
      r.loss_rate_ratio = tr.network_estimate.loss_rate_ratio;
      r.cwnd_reduce_ratio = tr.cwnd_reduce_ratio;
    }
    if (u.pacer_config) {
      r.pacer_rate_bps = u.pacer_config->data_rate().bps();
      r.pad_rate_bps = u.pacer_config->pad_rate().bps();
    }
    if (u.congestion_window && u.congestion_window->IsFinite()) r.cwnd_bytes = u.congestion_window->bytes();
    r.num_probe_clusters = static_cast<int32_t>(u.probe_cluster_configs.size());
    trace_->Write(r);
  }

  const std::unique_ptr<webrtc::NetworkControllerInterface> inner_;
  CcUpdateTrace* const trace_;
};

class LedgerNetworkControllerFactory : public webrtc::NetworkControllerFactoryInterface {
 public:
  explicit LedgerNetworkControllerFactory(CcUpdateTrace* trace) : trace_(trace) {}
  std::unique_ptr<webrtc::NetworkControllerInterface> Create(
      webrtc::NetworkControllerConfig config) override {
    return std::make_unique<LedgerNetworkController>(inner_.Create(config), trace_);
  }
  webrtc::TimeDelta GetProcessInterval() const override { return inner_.GetProcessInterval(); }

 private:
  // Same construction as call/rtp_transport_controller_send.cc's fallback with the default
  // (null) network_state_predictor_factory, so the controller behaves exactly like stock.
  webrtc::GoogCcNetworkControllerFactory inner_{
      static_cast<webrtc::NetworkStatePredictorFactoryInterface*>(nullptr)};
  CcUpdateTrace* const trace_;
};

class LedgerEncodedImageCallback : public webrtc::EncodedImageCallback {
 public:
  LedgerEncodedImageCallback(webrtc::EncodedImageCallback* inner, EncodedFrameTrace* trace)
      : inner_(inner), trace_(trace) {}

  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo* csi) override {
    EncodedFrameRow row{};
    row.rtp_ts = image.RtpTimestamp();
    row.encode_done_mono_ns = NowMonoNs();
    row.encode_done_wall_ns = NowWallNs();
    row.bytes = static_cast<int32_t>(image.size());
    row.width = static_cast<int32_t>(image._encodedWidth);
    row.height = static_cast<int32_t>(image._encodedHeight);
    row.frame_type = static_cast<int32_t>(image._frameType);
    row.qp = image.qp_;
    row.temporal_idx = image.TemporalIndex().value_or(-1);
    row.spatial_idx = image.SpatialIndex().value_or(-1);
    row.simulcast_idx = image.SimulcastIndex().value_or(-1);
    row.capture_time_ms = image.capture_time_ms_;
    row.ntp_time_ms = image.NtpTimeMs();
    row.codec = csi ? static_cast<int32_t>(csi->codecType) : -1;
    row.is_idr = (csi && csi->codecType == webrtc::kVideoCodecH264)
                     ? (csi->codecSpecific.H264.idr_frame ? 1 : 0)
                     : -1;
    row.at_target_quality = image.IsAtTargetQuality() ? 1 : 0;
    // Forward first (packetisation starts inside), record after: the record is pure observation.
    const Result r = inner_ ? inner_->OnEncodedImage(image, csi) : Result(Result::ERROR_SEND_FAILED);
    if (trace_) trace_->Write(row);
    return r;
  }
  void OnDroppedFrame(DropReason reason) override {
    if (inner_) inner_->OnDroppedFrame(reason);
  }

 private:
  webrtc::EncodedImageCallback* const inner_;
  EncodedFrameTrace* const trace_;
};

class LedgerVideoEncoder : public webrtc::VideoEncoder {
 public:
  LedgerVideoEncoder(std::unique_ptr<webrtc::VideoEncoder> inner, EncodedFrameTrace* trace,
                     EncoderRateTrace* rates)
      : inner_(std::move(inner)), trace_(trace), rates_(rates) {}

  void SetFecControllerOverride(webrtc::FecControllerOverride* o) override {
    inner_->SetFecControllerOverride(o);
  }
  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& settings) override {
    return inner_->InitEncode(codec_settings, settings);
  }
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) override {
    if (cb == nullptr) {  // nullptr = unregister (SimulcastEncoderAdapter convention)
      const int32_t r = inner_->RegisterEncodeCompleteCallback(nullptr);
      cb_.reset();
      return r;
    }
    auto next = std::make_unique<LedgerEncodedImageCallback>(cb, trace_);
    const int32_t r = inner_->RegisterEncodeCompleteCallback(next.get());
    if (r == WEBRTC_VIDEO_CODEC_OK) cb_ = std::move(next);  // drop old only after re-registration
    return r;
  }
  int32_t Release() override { return inner_->Release(); }
  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* types) override {
    return inner_->Encode(frame, types);
  }
  void SetRates(const RateControlParameters& p) override {
    inner_->SetRates(p);  // forward first; the record is pure observation
    if (!rates_) return;
    EncoderRateRow row{};
    row.set_mono_ns = NowMonoNs();
    row.set_wall_ns = NowWallNs();
    row.target_bps = p.target_bitrate.get_sum_bps();
    row.allocated_bps = p.bitrate.get_sum_bps();
    row.bandwidth_allocation_bps = p.bandwidth_allocation.bps();
    row.framerate_fps = p.framerate_fps;
    int32_t active = 0;
    for (size_t si = 0; si < webrtc::kMaxSpatialLayers; ++si)
      if (p.bitrate.IsSpatialLayerUsed(si)) ++active;
    row.num_active_spatial_layers = active;
    rates_->Write(row);
  }
  void OnPacketLossRateUpdate(float r) override { inner_->OnPacketLossRateUpdate(r); }
  void OnRttUpdate(int64_t rtt_ms) override { inner_->OnRttUpdate(rtt_ms); }
  void OnLossNotification(const LossNotification& n) override { inner_->OnLossNotification(n); }
  EncoderInfo GetEncoderInfo() const override { return inner_->GetEncoderInfo(); }

 private:
  const std::unique_ptr<webrtc::VideoEncoder> inner_;
  EncodedFrameTrace* const trace_;
  EncoderRateTrace* const rates_;
  std::unique_ptr<LedgerEncodedImageCallback> cb_;
};

// Transparent factory wrapper: codec capabilities and negotiation are untouched.
class LedgerVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  LedgerVideoEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner,
                            EncodedFrameTrace* trace, EncoderRateTrace* rates)
      : inner_(std::move(inner)), trace_(trace), rates_(rates) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return inner_->GetSupportedFormats();
  }
  std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
    return inner_->GetImplementations();
  }
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                 absl::optional<std::string> scalability_mode) const override {
    return inner_->QueryCodecSupport(format, scalability_mode);
  }
  std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(
      const webrtc::SdpVideoFormat& format) override {
    auto enc = inner_->CreateVideoEncoder(format);
    if (!enc) return nullptr;
    return std::make_unique<LedgerVideoEncoder>(std::move(enc), trace_, rates_);
  }
  std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface> GetEncoderSelector()
      const override {
    return inner_->GetEncoderSelector();
  }

 private:
  const std::unique_ptr<webrtc::VideoEncoderFactory> inner_;
  EncodedFrameTrace* const trace_;
  EncoderRateTrace* const rates_;
};

}  // namespace p5g

namespace p5g {

// ---- Decoded-frame ledger: one row per frame that went through the video decoder ---------------
//
// Same injection point as the encoder ledger, on the receive side (custom VideoDecoderFactory). The
// wrapper records when libwebrtc hands a frame to the decoder (= the frame left the jitter buffer,
// VideoStreamDecoder -> VideoDecoder::Decode) and when the decoder returns it (DecodedImageCallback),
// then forwards everything unchanged. Together with -rx-rtp.csv (packet arrival) and -rx-frames.csv
// (app sink) this splits receive-side latency into jitter-buffer wait / decode / delivery.
struct DecodedFrameLedgerRow {
  uint32_t rtp_ts;                // wire space (EncodedImage::RtpTimestamp() == VideoFrame::timestamp())
  int64_t decode_start_mono_ns;   // Decode() entry
  int64_t decode_done_mono_ns;    // Decoded() callback entry
  int64_t decode_done_wall_ns;
  int32_t input_bytes;
  int32_t frame_type;             // VideoFrameType: 3 key, 4 delta
  int64_t render_time_ms;         // libwebrtc's planned render time for the frame
  int32_t decoder_decode_time_ms; // reported by the decoder, -1 if not
  int32_t qp;                     // reported by the decoder, -1 if not
  int32_t width, height;
};
inline constexpr const char* kDecodedFrameLedgerHeader =
    "rtp_ts,decode_start_mono_ns,decode_done_mono_ns,decode_done_wall_ns,input_bytes,frame_type,"
    "render_time_ms,decoder_decode_time_ms,qp,width,height";
inline void FormatDecodedFrameLedgerRow(std::FILE* f, const DecodedFrameLedgerRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%lld,%d,%d,%lld,%d,%d,%d,%d\n", r.rtp_ts,
               (long long)r.decode_start_mono_ns, (long long)r.decode_done_mono_ns,
               (long long)r.decode_done_wall_ns, r.input_bytes, r.frame_type,
               (long long)r.render_time_ms, r.decoder_decode_time_ms, r.qp, r.width, r.height);
}
using DecodedFrameLedgerTrace = TraceRing<DecodedFrameLedgerRow, /*kMultiWriter=*/true>;

class LedgerVideoDecoder : public webrtc::VideoDecoder, public webrtc::DecodedImageCallback {
 public:
  LedgerVideoDecoder(std::unique_ptr<webrtc::VideoDecoder> inner, DecodedFrameLedgerTrace* trace)
      : inner_(std::move(inner)), trace_(trace) {}

  bool Configure(const Settings& s) override { return inner_->Configure(s); }
  int32_t Decode(const webrtc::EncodedImage& img, int64_t render_time_ms) override {
    Remember(img, render_time_ms);
    return inner_->Decode(img, render_time_ms);
  }
  int32_t Decode(const webrtc::EncodedImage& img, bool missing_frames, int64_t render_time_ms) override {
    Remember(img, render_time_ms);
    return inner_->Decode(img, missing_frames, render_time_ms);
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* cb) override {
    app_cb_ = cb;
    return inner_->RegisterDecodeCompleteCallback(cb ? this : nullptr);
  }
  int32_t Release() override { return inner_->Release(); }
  DecoderInfo GetDecoderInfo() const override { return inner_->GetDecoderInfo(); }
  const char* ImplementationName() const override { return inner_->ImplementationName(); }

  // DecodedImageCallback (all three overloads a decoder may call); record, then forward the same one.
  int32_t Decoded(webrtc::VideoFrame& f) override {
    Record(f, -1, -1);
    return app_cb_ ? app_cb_->Decoded(f) : -1;
  }
  int32_t Decoded(webrtc::VideoFrame& f, int64_t decode_time_ms) override {
    Record(f, static_cast<int32_t>(decode_time_ms), -1);
    return app_cb_ ? app_cb_->Decoded(f, decode_time_ms) : -1;
  }
  void Decoded(webrtc::VideoFrame& f, absl::optional<int32_t> decode_time_ms,
               absl::optional<uint8_t> qp) override {
    Record(f, decode_time_ms.value_or(-1), qp.has_value() ? *qp : -1);
    if (app_cb_) app_cb_->Decoded(f, decode_time_ms, qp);
  }

 private:
  struct Pending {
    uint32_t rtp_ts;
    int64_t start_mono_ns;
    int64_t render_time_ms;
    int32_t bytes;
    int32_t frame_type;
  };
  static constexpr size_t kPending = 32;  // decoders may hold a few frames in flight

  void Remember(const webrtc::EncodedImage& img, int64_t render_time_ms) {
    Pending& p = pending_[next_++ % kPending];
    p.rtp_ts = img.RtpTimestamp();
    p.start_mono_ns = NowMonoNs();
    p.render_time_ms = render_time_ms;
    p.bytes = static_cast<int32_t>(img.size());
    p.frame_type = static_cast<int32_t>(img._frameType);
  }
  void Record(const webrtc::VideoFrame& f, int32_t decode_time_ms, int32_t qp) {
    if (!trace_) return;
    DecodedFrameLedgerRow r{};
    r.rtp_ts = f.timestamp();
    r.decode_done_mono_ns = NowMonoNs();
    r.decode_done_wall_ns = NowWallNs();
    r.decode_start_mono_ns = -1;
    r.input_bytes = -1;
    r.frame_type = -1;
    r.render_time_ms = -1;
    for (const Pending& p : pending_) {
      if (p.rtp_ts == r.rtp_ts && p.start_mono_ns != 0) {
        r.decode_start_mono_ns = p.start_mono_ns;
        r.input_bytes = p.bytes;
        r.frame_type = p.frame_type;
        r.render_time_ms = p.render_time_ms;
        break;
      }
    }
    r.decoder_decode_time_ms = decode_time_ms;
    r.qp = qp;
    r.width = f.width();
    r.height = f.height();
    trace_->Write(r);
  }

  const std::unique_ptr<webrtc::VideoDecoder> inner_;
  DecodedFrameLedgerTrace* const trace_;
  webrtc::DecodedImageCallback* app_cb_ = nullptr;
  Pending pending_[kPending] = {};  // Decode()/Decoded() run on the decode queue: no locking needed
  size_t next_ = 0;
};

// Transparent VideoDecoderFactory wrapper. The trace to use is set by the app right before it creates
// the PeerConnection (decoders are created later, on the first frame, on the decode queue); one
// receiver process serves one stream, which is how the run scripts start it.
class LedgerVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  explicit LedgerVideoDecoderFactory(std::unique_ptr<webrtc::VideoDecoderFactory> inner)
      : inner_(std::move(inner)) {}
  void SetTrace(DecodedFrameLedgerTrace* trace) { trace_.store(trace, std::memory_order_release); }

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return inner_->GetSupportedFormats();
  }
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                 bool reference_scaling) const override {
    return inner_->QueryCodecSupport(format, reference_scaling);
  }
  std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(
      const webrtc::SdpVideoFormat& format) override {
    auto dec = inner_->CreateVideoDecoder(format);
    if (!dec) return nullptr;
    return std::make_unique<LedgerVideoDecoder>(std::move(dec), trace_.load(std::memory_order_acquire));
  }

 private:
  const std::unique_ptr<webrtc::VideoDecoderFactory> inner_;
  std::atomic<DecodedFrameLedgerTrace*> trace_{nullptr};
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_WEBRTC_TRACING_H
