// video_receiver — libwebrtc video receiver with per-frame / per-packet tracing.
//
// Follows examples/peerconnection/client/conductor.cc (answerer): register -> receive offer ->
// SetRemoteDescription -> CreateAnswer -> SetLocalDescription -> (non-trickle) send answer after ICE
// gathering -> OnTrack attaches a frame sink. One PeerConnection per incoming stream.
//
// Traces (all in --trace-dir):
//   <stream>-rx-frames.csv   every decoded frame delivered to the app (trace_ring.h)
//   <stream>-rx-rtp.csv      every received RTP packet;  <stream>-rx-rtcp.csv RTCP (both directions)
//   <stream>-rx-decoded.csv  every frame through the video decoder (jitter-buffer exit / decode done)
//   <stream>-rx-stats.jsonl  W3C getStats() every --stats-period-ms (0 = off)
//
// Playout: stock libwebrtc jitter buffer by default (realistic app behaviour). --low-latency-playout
// switches to the zero-playout-delay field trials for machine-consumer experiments.
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/video/video_sink_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "system_wrappers/include/ntp_time.h"

#include "app_util.h"
#include "trace_ring.h"
#include "signaling_client.h"
#include "webrtc_session.h"

namespace p5g {

struct ReceiverConfig {
  std::string signaling_host = "127.0.0.1";
  int signaling_port = 8765;
  std::string session = "s1";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  int duration_s = 0;
  // ICE servers (comma-separated URLs, e.g. stun:stun.l.google.com:19302). Empty = host candidates only
  // (single-site test). Behind the UPF NAT on the internet topology a STUN server lets the UE side
  // learn its server-reflexive address, as in examples/peerconnection/client/conductor.cc.
  std::string ice_servers;
  int stats_period_ms = 1000;  // periodic getStats() sampling; 0 = off
  bool low_latency_playout = false;
};
static ReceiverConfig g_cfg;

// Track sink: records one row per decoded frame. Runs on the decode queue with two libwebrtc
// locks held, so it only copies a POD row (trace_ring.h) — no allocation, no I/O.
class FrameSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit FrameSink(std::unique_ptr<DecodedFrameTrace> trace) : trace_(std::move(trace)) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    const int64_t recv_wall_ns = NowWallNs();
    const int64_t recv_mono_ns = NowMonoNs();
    int64_t acap_ms = -1, first_ns = -1, last_ns = -1;
    int32_t npkt = 0;
    uint32_t ssrc = 0;
    for (const auto& pi : frame.packet_infos()) {
      const int64_t rt = pi.receive_time().us() * 1000;  // payload-processing time (not socket rx)
      if (acap_ms < 0 && pi.absolute_capture_time().has_value())
        acap_ms = webrtc::UQ32x32ToInt64Ms(pi.absolute_capture_time()->absolute_capture_timestamp);
      if (first_ns < 0 || rt < first_ns) first_ns = rt;
      if (rt > last_ns) last_ns = rt;
      ssrc = pi.ssrc();
      ++npkt;
    }
    // Live reconstruction of the sender-space RTP timestamp (same formula as video_source.h) so the
    // row can be matched to the sender's tx-frames.csv by eye; no offline join needed.
    int64_t sender_ts_est = -1, wire_offset = -1;
    if (acap_ms >= 0) {
      sender_ts_est = static_cast<int64_t>(90u * static_cast<uint32_t>(acap_ms));
      wire_offset = static_cast<int64_t>((frame.timestamp() - static_cast<uint32_t>(sender_ts_est)));
    }
    trace_->Write(DecodedFrameRow{idx_++, frame.timestamp(), acap_ms, sender_ts_est, wire_offset,
                                  recv_wall_ns, recv_mono_ns, frame.width(), frame.height(), npkt,
                                  first_ns, last_ns, ssrc});
  }
  void Close() { trace_->Close(); }

 private:
  std::unique_ptr<DecodedFrameTrace> trace_;
  int64_t idx_ = 0;
};

class ReceiverPeer : public webrtc::PeerConnectionObserver {
 public:
  ReceiverPeer(std::string stream_id, SignalingClient* sig) : stream_id_(std::move(stream_id)), sig_(sig) {}

  std::string TracePrefix() const { return g_cfg.trace_dir + "/" + stream_id_ + "-rx"; }

  bool HandleOffer(webrtc::PeerConnectionFactoryInterface* factory, RtpPacketLedgerFactory* ledgers,
                   LedgerVideoDecoderFactory* decoders, const std::string& sdp) {
    decoded_trace_ = std::make_unique<DecodedFrameLedgerTrace>(
        TracePrefix() + "-decoded.csv", kDecodedFrameLedgerHeader, &FormatDecodedFrameLedgerRow,
        kFrameTraceCapacity);
    if (decoders) decoders->SetTrace(decoded_trace_.get());
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    ApplyIceServers(&config, g_cfg.ice_servers);
    if (g_cfg.low_latency_playout) config.set_prerenderer_smoothing(false);
    ledgers->SetNextLedgerBasename(TracePrefix());
    webrtc::PeerConnectionDependencies deps(this);
    auto pc = factory->CreatePeerConnectionOrError(config, std::move(deps));
    if (!pc.ok()) {
      P5G_LOG_ERROR << "CreatePeerConnection failed: " << pc.error().message();
      return false;
    }
    pc_ = pc.MoveValue();
    webrtc::SdpParseError err;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &err);
    if (!desc) {
      P5G_LOG_ERROR << "bad offer SDP: " << err.description;
      return false;
    }
    pc_->SetRemoteDescription(SetSdpObserver::Create().get(), desc.release());
    for (auto& tr : pc_->GetTransceivers())
      if (tr->media_type() == cricket::MEDIA_TYPE_VIDEO) RequireAbsCaptureTime(tr.get());
    pc_->CreateAnswer(CreateSdpObserver::Create([this](webrtc::SessionDescriptionInterface* d) {
                        pc_->SetLocalDescription(SetSdpObserver::Create().get(), d);
                      }).get(),
                      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    return true;
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> t) override {
    auto track = t->receiver()->track();
    if (track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) return;
    if (sink_) P5G_FATAL("more than one video track for stream " << stream_id_);
    video_track_ = rtc::scoped_refptr<webrtc::VideoTrackInterface>(
        static_cast<webrtc::VideoTrackInterface*>(track.get()));
    sink_ = std::make_unique<FrameSink>(std::make_unique<DecodedFrameTrace>(
        TracePrefix() + "-frames.csv", kDecodedFrameHeader, &FormatDecodedFrameRow, kFrameTraceCapacity));
    video_track_->AddOrUpdateSink(sink_.get(), rtc::VideoSinkWants());
    P5G_LOG_INFO << "video track attached for stream " << stream_id_;
  }

  void AppendStats() { p5g::AppendStatsLine(pc_.get(), TracePrefix() + "-stats.jsonl"); }
  void Close() {
    if (video_track_ && sink_) video_track_->RemoveSink(sink_.get());
    if (pc_) pc_->Close();  // stops the decode queue -> no more decoder callbacks
    if (sink_) sink_->Close();
    if (decoded_trace_) decoded_trace_->Close();
  }

  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override {
    if (state != webrtc::PeerConnectionInterface::kIceGatheringComplete || answer_sent_.exchange(true))
      return;
    const auto* ld = pc_->local_description();
    if (!ld) P5G_FATAL("no local description after ICE gathering");
    std::string sdp;
    ld->ToString(&sdp);
    if (!sig_->Send({{"type", "answer"}, {"stream", stream_id_}, {"sdp", sdp}}))
      P5G_LOG_ERROR << "answer send failed";
  }
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) override {
    P5G_LOG_INFO << "peerconnection state=" << static_cast<int>(s) << " (0 new,1 connecting,2 connected,3 disconnected,4 failed,5 closed)";
  }
  void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}

 private:
  const std::string stream_id_;
  SignalingClient* const sig_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
  std::unique_ptr<FrameSink> sink_;
  std::unique_ptr<DecodedFrameLedgerTrace> decoded_trace_;  // must outlive the PC's decoders
  std::atomic<bool> answer_sent_{false};
};

class Receiver {
 public:
  Receiver() {
    signaling_thread_ = rtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
    factory_ = CreateFactory(signaling_thread_.get(), nullptr, &ledgers_, &decoders_);
    if (!factory_) P5G_FATAL("PeerConnectionFactory creation failed");
  }

  bool Run() {
    if (!sig_.Connect(g_cfg.signaling_host, g_cfg.signaling_port)) {
      P5G_LOG_ERROR << "signaling connect failed";
      return false;
    }
    sig_.Start([this](const json& m) { OnMessage(m); });
    return sig_.Send({{"type", "register"}, {"role", "receiver"}, {"session", g_cfg.session},
                      {"receiver", g_cfg.receiver_id}});
  }

  void OnMessage(const json& m) {
    if (m.value("type", "") != "offer") return;
    const std::string stream = m.at("stream").get<std::string>();
    const std::string sdp = m.at("sdp").get<std::string>();
    std::lock_guard<std::mutex> lk(mu_);
    if (peers_.count(stream)) return;
    auto peer = std::make_unique<ReceiverPeer>(stream, &sig_);
    bool ok = false;
    signaling_thread_->BlockingCall([&] { ok = peer->HandleOffer(factory_.get(), ledgers_, decoders_, sdp); });
    if (!ok) P5G_FATAL("peer init failed for stream " << stream);
    peers_[stream] = std::move(peer);
    P5G_LOG_INFO << "answering offer for stream " << stream;
  }

  void AppendStats() {  // 1 s periodic W3C stats sample -> <prefix>-stats.jsonl
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : peers_) kv.second->AppendStats();
  }

  void Shutdown() {
    sig_.Stop();
    std::lock_guard<std::mutex> lk(mu_);
    signaling_thread_->BlockingCall([&] {
      for (auto& kv : peers_) kv.second->Close();
    });
    if (ledgers_) ledgers_->CloseAll();
  }

 private:
  std::unique_ptr<rtc::Thread> signaling_thread_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  RtpPacketLedgerFactory* ledgers_ = nullptr;
  LedgerVideoDecoderFactory* decoders_ = nullptr;  // owned by factory_
  SignalingClient sig_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<ReceiverPeer>> peers_;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr,
               "video_receiver --signaling-host H --signaling-port P --session S --receiver-id ID\n"
               "               --trace-dir DIR [--duration S] [--low-latency-playout] [--ice-servers URLS] [--stats-period-ms 1000]\n");
}

int main(int argc, char** argv) {
  p5g::CliArgs a(argc, argv);
  if (a.Has("help")) {
    Usage();
    return 0;
  }
  auto& c = p5g::g_cfg;
  c.signaling_host = a.Get("signaling-host", c.signaling_host);
  c.signaling_port = a.GetInt("signaling-port", c.signaling_port);
  c.session = a.Get("session", c.session);
  c.receiver_id = a.Get("receiver-id", c.receiver_id);
  c.trace_dir = a.Get("trace-dir", c.trace_dir);
  c.duration_s = a.GetInt("duration", 0);
  c.ice_servers = a.Get("ice-servers", "");
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms);
  c.low_latency_playout = a.Has("low-latency-playout");

  if (c.low_latency_playout) {
    // Zero playout delay via stock field trials (M120): ForcePlayoutDelay min=0 with max in (0,500]
    // takes the low-latency render path without opening the frame-drop path a max of 0 would open;
    // ZeroPlayoutDelay/max_decode_queue_size:0 removes the residual 8 ms decode pacing.
    static const std::string kTrials =
        "WebRTC-ForcePlayoutDelay/min_ms:0,max_ms:500/WebRTC-ZeroPlayoutDelay/max_decode_queue_size:0/";
    webrtc::field_trial::InitFieldTrialsFromString(kTrials.c_str());
    P5G_LOG_WARN << "low-latency playout enabled (non-stock jitter buffer behaviour)";
  }

  P5G_LOG_INFO << "config: receiver_id=" << c.receiver_id << " session=" << c.session
               << " low_latency_playout=" << (c.low_latency_playout ? 1 : 0)
               << " stats_period_ms=" << c.stats_period_ms
               << " ice_servers=" << (c.ice_servers.empty() ? "none" : c.ice_servers);

  p5g::InstallSignalHandlers();
  p5g::MaybeEnableWebrtcLogging();
  rtc::InitializeSSL();
  {
    p5g::Receiver receiver;
    if (!receiver.Run()) {
      rtc::CleanupSSL();
      return 1;
    }
    p5g::RunUntilShutdown(c.duration_s, [&] { receiver.AppendStats(); }, c.stats_period_ms);
    receiver.Shutdown();
  }
  rtc::CleanupSSL();
  return 0;
}
