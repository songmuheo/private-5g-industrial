// video_sender — libwebrtc video sender with per-frame / per-packet tracing.
//
// Follows examples/peerconnection/client/conductor.cc for the PeerConnection flow (offerer):
// register at the signaling server -> wait for the target receiver -> CreateOffer ->
// SetLocalDescription -> (non-trickle) send the complete SDP after ICE gathering completes ->
// apply the answer. One PeerConnection per receiver.
//
// Deployment: on the real testbed this runs on the laptop tethered to a Pixel phone (uplink video
// through the private 5G cell) or on the data-network host (downlink video). Locally it runs inside
// the srsUE network namespace.
//
// Traces (all in --trace-dir):
//   <stream>-tx-frames.csv   every capture slot (video_source.h)
//   <stream>-tx-encoded.csv  every encoded frame (webrtc_tracing.h)
//   <stream>-tx-encoder-rates.csv  every encoder target-bitrate update (VideoEncoder::SetRates)
//   <stream>-tx-cc.csv       every GoogCC output update (target/stable rate, estimate, RTT, loss, pacer)
//   <stream>-tx-rtp.csv      every sent RTP packet;  <stream>-tx-rtcp.csv every RTCP packet (both dirs)
//   <stream>-tx-stats.jsonl  W3C getStats() every --stats-period-ms (0 = off)
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "api/peer_connection_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include "app_util.h"
#include "webrtc_tracing.h"
#include "trace_ring.h"
#include "signaling_client.h"
#include "video_source.h"
#include "webrtc_session.h"

namespace p5g {

struct SenderConfig {
  std::string signaling_host = "127.0.0.1";
  int signaling_port = 8765;
  std::string session = "s1";
  std::string stream_id = "cam0";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  std::string codec = "H264";         // VP8 | VP9 | H264 | AV1
  std::string degradation = "stock";  // stock | maintain_resolution | maintain_framerate
  int max_bitrate_kbps = 0;           // 0 = libwebrtc default (derived from resolution)
  int start_bitrate_kbps = 0;         // 0 = libwebrtc default
  int duration_s = 0;
  // ICE servers (comma-separated URLs, e.g. stun:stun.l.google.com:19302). Empty = host candidates only
  // (single-site test). Behind the UPF NAT on the internet topology a STUN server lets the UE side
  // learn its server-reflexive address, as in examples/peerconnection/client/conductor.cc.
  std::string ice_servers;
  int stats_period_ms = 1000;  // periodic getStats() sampling; 0 = off
  // The only on-wire deviation from stock M120: negotiate the abs-capture-time header extension
  // (+12 bytes on the first packet of each frame) so the receiver can log the sender-space rtp_ts
  // live. 0 = stock header set; rx-frames.sender_rtp_ts_est then stays -1 (join offline via RTCP SR).
  int abs_capture_time = 1;
  VideoSourceConfig video;
};
static SenderConfig g_cfg;

std::string TracePrefix() { return g_cfg.trace_dir + "/" + g_cfg.stream_id + "-tx"; }

class SenderPeer : public webrtc::PeerConnectionObserver {
 public:
  SenderPeer(std::string receiver_id, SignalingClient* sig)
      : receiver_id_(std::move(receiver_id)), sig_(sig) {}

  bool Init(webrtc::PeerConnectionFactoryInterface* factory, RtpPacketLedgerFactory* ledgers,
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source) {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    ApplyIceServers(&config, g_cfg.ice_servers);
    ledgers->SetNextLedgerBasename(TracePrefix());
    webrtc::PeerConnectionDependencies deps(this);
    auto pc = factory->CreatePeerConnectionOrError(config, std::move(deps));
    if (!pc.ok()) {
      P5G_LOG_ERROR << "CreatePeerConnection failed: " << pc.error().message();
      return false;
    }
    pc_ = pc.MoveValue();

    auto track = factory->CreateVideoTrack(source, "video");
    auto added = pc_->AddTrack(track, {"stream_" + g_cfg.stream_id});
    if (!added.ok()) {
      P5G_LOG_ERROR << "AddTrack failed: " << added.error().message();
      return false;
    }

    // SetCodecPreferences takes a mutable ArrayView, which cannot bind to a temporary: keep a named,
    // non-const vector alive for the call.
    std::vector<webrtc::RtpCodecCapability> prefs = CodecPreferences(factory, g_cfg.codec);
    for (auto& tr : pc_->GetTransceivers()) {
      if (tr->media_type() != cricket::MEDIA_TYPE_VIDEO) continue;
      auto err = tr->SetCodecPreferences(prefs);
      if (!err.ok()) P5G_FATAL("SetCodecPreferences(" << g_cfg.codec << ") failed: " << err.message());
      if (g_cfg.abs_capture_time) RequireAbsCaptureTime(tr.get());
    }

    // Optional deviations from stock, all off by default and recorded in the run config.
    auto sender = added.value();
    auto params = sender->GetParameters();
    if (g_cfg.degradation == "maintain_resolution")
      params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
    else if (g_cfg.degradation == "maintain_framerate")
      params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
    else if (g_cfg.degradation != "stock")
      P5G_FATAL("unknown --degradation " << g_cfg.degradation);
    if (g_cfg.max_bitrate_kbps > 0)
      for (auto& e : params.encodings) e.max_bitrate_bps = g_cfg.max_bitrate_kbps * 1000;
    if (g_cfg.degradation != "stock" || g_cfg.max_bitrate_kbps > 0) {
      auto perr = sender->SetParameters(params);
      if (!perr.ok()) P5G_FATAL("SetParameters failed: " << perr.message());
    }
    if (g_cfg.start_bitrate_kbps > 0) {
      webrtc::BitrateSettings bs;
      bs.start_bitrate_bps = g_cfg.start_bitrate_kbps * 1000;
      pc_->SetBitrate(bs);
    }

    pc_->CreateOffer(CreateSdpObserver::Create([this](webrtc::SessionDescriptionInterface* d) {
                       pc_->SetLocalDescription(SetSdpObserver::Create().get(), d);
                     }).get(),
                     webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    return true;
  }

  void OnAnswer(const std::string& sdp) {
    webrtc::SdpParseError err;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &err);
    if (!desc) {
      P5G_LOG_ERROR << "bad answer SDP: " << err.description;
      return;
    }
    pc_->SetRemoteDescription(SetSdpObserver::Create([] { P5G_LOG_INFO << "answer applied"; }).get(),
                              desc.release());
  }

  void AppendStats() { p5g::AppendStatsLine(pc_.get(), TracePrefix() + "-stats.jsonl"); }
  void Close() {
    if (pc_) pc_->Close();
  }

  // PeerConnectionObserver — non-trickle ICE: send the full SDP once gathering is complete.
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override {
    if (state != webrtc::PeerConnectionInterface::kIceGatheringComplete || offer_sent_.exchange(true))
      return;
    const auto* ld = pc_->local_description();
    if (!ld) P5G_FATAL("no local description after ICE gathering");
    std::string sdp;
    ld->ToString(&sdp);
    if (!sig_->Send({{"type", "offer"}, {"to", receiver_id_}, {"stream", g_cfg.stream_id}, {"sdp", sdp}}))
      P5G_LOG_ERROR << "offer send failed";
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
  const std::string receiver_id_;
  SignalingClient* const sig_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  std::atomic<bool> offer_sent_{false};
};

class Sender {
 public:
  Sender() {
    encoded_trace_ = std::make_unique<EncodedFrameTrace>(TracePrefix() + "-encoded.csv",
                                                         kEncodedFrameHeader, &FormatEncodedFrameRow,
                                                         kFrameTraceCapacity);
    encoder_rates_ = std::make_unique<EncoderRateTrace>(TracePrefix() + "-encoder-rates.csv",
                                                        kEncoderRateHeader, &FormatEncoderRateRow,
                                                        kFrameTraceCapacity);
    cc_trace_ = std::make_unique<CcUpdateTrace>(TracePrefix() + "-cc.csv", kCcUpdateHeader,
                                                &FormatCcUpdateRow, kFrameTraceCapacity);
    signaling_thread_ = rtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
    factory_ = CreateFactory(signaling_thread_.get(), encoded_trace_.get(), &ledgers_, nullptr,
                             encoder_rates_.get(), cc_trace_.get());
    if (!factory_) P5G_FATAL("PeerConnectionFactory creation failed");
  }

  bool Run() {
    if (!sig_.Connect(g_cfg.signaling_host, g_cfg.signaling_port)) {
      P5G_LOG_ERROR << "signaling connect failed (" << g_cfg.signaling_host << ":" << g_cfg.signaling_port << ")";
      return false;
    }
    sig_.Start([this](const json& m) { OnMessage(m); });
    return sig_.Send({{"type", "register"}, {"role", "sender"}, {"session", g_cfg.session},
                      {"stream", g_cfg.stream_id}});
  }

  void OnMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "receiver-ready") {
      const std::string rid = m.at("receiver").get<std::string>();
      if (rid != g_cfg.receiver_id) return;
      Connect(rid);
    } else if (type == "answer") {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = peers_.find(m.at("receiver").get<std::string>());
      if (it == peers_.end()) return;
      const std::string sdp = m.at("sdp").get<std::string>();
      SenderPeer* p = it->second.get();
      signaling_thread_->BlockingCall([&] { p->OnAnswer(sdp); });  // PC API on the signaling thread
    }
  }

  void Connect(const std::string& rid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (peers_.count(rid)) return;
    frame_trace_ = std::make_unique<CaptureFrameTrace>(TracePrefix() + "-frames.csv", kCaptureFrameHeader,
                                                       &FormatCaptureFrameRow, kFrameTraceCapacity);
    source_ = GridVideoTrackSource::Create(g_cfg.video, frame_trace_.get());
    auto peer = std::make_unique<SenderPeer>(rid, &sig_);
    bool ok = false;
    signaling_thread_->BlockingCall([&] { ok = peer->Init(factory_.get(), ledgers_, source_); });
    if (!ok) P5G_FATAL("peer init failed for receiver " << rid);
    peers_[rid] = std::move(peer);
    P5G_LOG_INFO << "offering stream " << g_cfg.stream_id << " to receiver " << rid;
  }

  void AppendStats() {  // 1 s periodic W3C stats sample -> <prefix>-stats.jsonl
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : peers_) kv.second->AppendStats();
  }

  // Ordered teardown: signaling -> capture thread -> PCs -> traces (no writer left dangling).
  void Shutdown() {
    sig_.Stop();
    std::lock_guard<std::mutex> lk(mu_);
    if (source_) source_->StopCapture();
    signaling_thread_->BlockingCall([&] {
      for (auto& kv : peers_) kv.second->Close();
    });
    if (frame_trace_) frame_trace_->Close();
    if (ledgers_) ledgers_->CloseAll();
    if (encoded_trace_) encoded_trace_->Close();  // after PC close: encoder queue is drained
  }

 private:
  std::unique_ptr<EncodedFrameTrace> encoded_trace_;  // must outlive factory_ (encoder wrapper holds raw ptr)
  std::unique_ptr<EncoderRateTrace> encoder_rates_;   // same lifetime rule
  std::unique_ptr<CcUpdateTrace> cc_trace_;           // same lifetime rule
  std::unique_ptr<rtc::Thread> signaling_thread_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  RtpPacketLedgerFactory* ledgers_ = nullptr;  // owned by factory_
  SignalingClient sig_;
  std::mutex mu_;  // never taken on the WebRTC signaling thread (BlockingCall deadlock)
  std::unique_ptr<CaptureFrameTrace> frame_trace_;
  rtc::scoped_refptr<GridVideoTrackSource> source_;
  std::map<std::string, std::unique_ptr<SenderPeer>> peers_;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr,
               "video_sender --signaling-host H --signaling-port P --session S --stream-id ID --to RECV_ID\n"
               "             --trace-dir DIR [--yuv FILE | (pattern)] --width W --height H --fps F\n"
               "             [--codec H264|VP8|VP9|AV1] [--max-bitrate-kbps N] [--start-bitrate-kbps N]\n"
               "             [--degradation stock|maintain_resolution|maintain_framerate] [--duration S]\n"
               "             [--ice-servers stun:host:port,...] [--stats-period-ms 1000]\n"
               "             [--abs-capture-time 1|0]\n");
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
  c.stream_id = a.Get("stream-id", c.stream_id);
  c.receiver_id = a.Get("to", c.receiver_id);
  c.trace_dir = a.Get("trace-dir", c.trace_dir);
  c.codec = a.Get("codec", c.codec);
  for (auto& ch : c.codec) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (c.codec != "VP8" && c.codec != "VP9" && c.codec != "H264" && c.codec != "AV1") P5G_FATAL("unsupported --codec " << c.codec);
  c.degradation = a.Get("degradation", c.degradation);
  c.max_bitrate_kbps = a.GetInt("max-bitrate-kbps", 0);
  c.start_bitrate_kbps = a.GetInt("start-bitrate-kbps", 0);
  c.duration_s = a.GetInt("duration", 0);
  c.ice_servers = a.Get("ice-servers", "");
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms);
  c.abs_capture_time = a.GetInt("abs-capture-time", c.abs_capture_time);
  c.video.yuv_path = a.Get("yuv", "");
  c.video.width = a.GetInt("width", c.video.width);
  c.video.height = a.GetInt("height", c.video.height);
  c.video.fps = a.GetInt("fps", c.video.fps);

  p5g::InstallSignalHandlers();
  p5g::MaybeEnableWebrtcLogging();
  rtc::InitializeSSL();
  {
    p5g::Sender sender;
    if (!sender.Run()) {
      rtc::CleanupSSL();
      return 1;
    }
    p5g::RunUntilShutdown(c.duration_s, [&] { sender.AppendStats(); }, c.stats_period_ms);
    sender.Shutdown();
  }
  rtc::CleanupSSL();
  return 0;
}
