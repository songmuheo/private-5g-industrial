// PeerConnection session helpers shared by sender and receiver: factory assembly (modular, with the
// tracing hooks injected), ICE servers, codec preferences, abs-capture-time negotiation, getStats
// dumps and SDP observers. See the original design notes inside.
#ifndef P5G_APPS_COMMON_WEBRTC_SESSION_H
#define P5G_APPS_COMMON_WEBRTC_SESSION_H

#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api/field_trials.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/call/call_factory_interface.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/transport/field_trial_based_config.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "media/engine/webrtc_media_engine.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

#include "app_util.h"
#include "webrtc_tracing.h"

// PeerConnectionFactory assembly and small libwebrtc helpers shared by sender and receiver.
//
// The factory is built with CreateModularPeerConnectionFactory, replicating the assembly of
// api/create_peerconnection_factory.cc line by line, with exactly two differences:
//   * event_log_factory  = RtpPacketLedgerFactory (per-packet RTP/RTCP/event ledger, webrtc_tracing.h)
//   * video_encoder_factory is wrapped by LedgerVideoEncoderFactory when an encoded-frame trace is
//     given (sender); video_decoder_factory by LedgerVideoDecoderFactory when requested (receiver).
//   * network_controller_factory = LedgerNetworkControllerFactory (stock GoogCC + observer) when a
//     congestion-controller trace is given (sender).
// Everything else (codecs, audio processing, task queue factory, field trials) is stock.

namespace p5g {

inline rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> CreateFactory(
    rtc::Thread* signaling_thread, EncodedFrameTrace* encoded_trace,
    RtpPacketLedgerFactory** out_ledger_factory,
    LedgerVideoDecoderFactory** out_decoder_factory = nullptr,
    EncoderRateTrace* encoder_rates = nullptr, CcUpdateTrace* cc_trace = nullptr) {
  auto make_venc = [encoded_trace, encoder_rates]() -> std::unique_ptr<webrtc::VideoEncoderFactory> {
    auto base = std::make_unique<webrtc::VideoEncoderFactoryTemplate<
        webrtc::LibvpxVp8EncoderTemplateAdapter, webrtc::LibvpxVp9EncoderTemplateAdapter,
        webrtc::OpenH264EncoderTemplateAdapter, webrtc::LibaomAv1EncoderTemplateAdapter>>();
    if (!encoded_trace) return base;
    return std::make_unique<LedgerVideoEncoderFactory>(std::move(base), encoded_trace, encoder_rates);
  };
  auto make_vdec = [out_decoder_factory]() -> std::unique_ptr<webrtc::VideoDecoderFactory> {
    auto base = std::make_unique<webrtc::VideoDecoderFactoryTemplate<
        webrtc::LibvpxVp8DecoderTemplateAdapter, webrtc::LibvpxVp9DecoderTemplateAdapter,
        webrtc::OpenH264DecoderTemplateAdapter, webrtc::Dav1dDecoderTemplateAdapter>>();
    if (!out_decoder_factory) return base;
    auto wrapped = std::make_unique<LedgerVideoDecoderFactory>(std::move(base));
    *out_decoder_factory = wrapped.get();  // owned by the media engine / factory
    return wrapped;
  };

  // Headless hosts have no audio device: use the dummy ADM (video-only workload).
  static std::unique_ptr<webrtc::TaskQueueFactory> adm_task_queue_factory =
      webrtc::CreateDefaultTaskQueueFactory();
  rtc::scoped_refptr<webrtc::AudioDeviceModule> dummy_adm = webrtc::AudioDeviceModule::Create(
      webrtc::AudioDeviceModule::kDummyAudio, adm_task_queue_factory.get());

  // ---- api/create_peerconnection_factory.cc assembly ----
  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = nullptr;
  deps.worker_thread = nullptr;
  deps.signaling_thread = signaling_thread;
  // Stock field trials (none set). PeerConnectionFactory honours an injected network controller
  // only behind "WebRTC-Bwe-InjectedCongestionController" (pc/peer_connection_factory.cc); that
  // trial is a pure gate — it is read nowhere else in M120 — so enabling it changes nothing but
  // which factory object is asked to create the (still stock) GoogCC controller.
  std::unique_ptr<webrtc::FieldTrialsView> trials;
  if (cc_trace) {
    trials = webrtc::FieldTrials::CreateNoGlobal("WebRTC-Bwe-InjectedCongestionController/Enabled/");
  } else {
    trials = std::make_unique<webrtc::FieldTrialBasedConfig>();
  }
  deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory(trials.get());
  deps.call_factory = webrtc::CreateCallFactory();
  auto ledger = std::make_unique<RtpPacketLedgerFactory>();
  if (out_ledger_factory) *out_ledger_factory = ledger.get();
  deps.event_log_factory = std::move(ledger);  // <- non-stock line 1 (observer)
  if (cc_trace)                                 // <- non-stock line 2 (observer around stock GoogCC)
    deps.network_controller_factory = std::make_unique<LedgerNetworkControllerFactory>(cc_trace);
  deps.trials = std::move(trials);

  cricket::MediaEngineDependencies media_deps;
  media_deps.task_queue_factory = deps.task_queue_factory.get();
  media_deps.adm = std::move(dummy_adm);
  media_deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  media_deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  media_deps.audio_processing = webrtc::AudioProcessingBuilder().Create();
  media_deps.audio_mixer = nullptr;
  media_deps.video_encoder_factory = make_venc();
  media_deps.video_decoder_factory = make_vdec();
  media_deps.trials = deps.trials.get();
  deps.media_engine = cricket::CreateMediaEngine(std::move(media_deps));

  return webrtc::CreateModularPeerConnectionFactory(std::move(deps));
}

// Parse "url1,url2" into RTCConfiguration::servers (STUN/TURN). TURN credentials are not supported
// here on purpose: the testbed hosts are ours, so a plain STUN server is enough to discover the
// UE's reflexive address behind the UPF NAT.
inline void ApplyIceServers(webrtc::PeerConnectionInterface::RTCConfiguration* config,
                            const std::string& csv) {
  size_t start = 0;
  while (start < csv.size()) {
    const size_t comma = csv.find(',', start);
    const std::string url = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!url.empty()) {
      webrtc::PeerConnectionInterface::IceServer s;
      s.uri = url;
      config->servers.push_back(s);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
}

// Codec preference list pinning the primary video codec (VP8 | VP9 | H264 | AV1) plus the
// retransmission / FEC helper codecs. Applied identically on both ends so negotiation is
// deterministic across runs.
inline std::vector<webrtc::RtpCodecCapability> CodecPreferences(
    webrtc::PeerConnectionFactoryInterface* factory, const std::string& primary) {
  auto caps = factory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_VIDEO);
  std::vector<webrtc::RtpCodecCapability> out;
  for (const auto& c : caps.codecs)
    if (c.name == primary || c.name == "rtx" || c.name == "red" || c.name == "ulpfec") out.push_back(c);
  return out;
}

// Negotiate the abs-capture-time RTP header extension (off by default in M120,
// media/engine/webrtc_video_engine.cc). It carries the sender's capture NTP time per frame, which is
// what lets the analysis resolve the per-SSRC random RTP-timestamp offset and join tx/rx traces.
// Losing it silently would make the run unusable, hence fatal.
inline void RequireAbsCaptureTime(webrtc::RtpTransceiverInterface* tr) {
  auto exts = tr->GetHeaderExtensionsToNegotiate();
  bool found = false;
  for (auto& e : exts) {
    if (e.uri == webrtc::RtpExtension::kAbsoluteCaptureTimeUri) {
      e.direction = webrtc::RtpTransceiverDirection::kSendRecv;
      found = true;
    }
  }
  if (!found) P5G_FATAL("abs-capture-time extension not in capability list");
  auto err = tr->SetHeaderExtensionsToNegotiate(exts);
  if (!err.ok()) P5G_FATAL("cannot enable abs-capture-time: " << err.message());
}

// libwebrtc internal logging: silent by default; P5G_WEBRTC_LOG=error|warning|info|verbose.
inline void MaybeEnableWebrtcLogging() {
  const char* v = std::getenv("P5G_WEBRTC_LOG");
  if (!v || !v[0]) return;
  const std::string s = v;
  const rtc::LoggingSeverity sev = s == "verbose" ? rtc::LS_VERBOSE
                                   : s == "info"  ? rtc::LS_INFO
                                   : s == "error" ? rtc::LS_ERROR
                                                  : rtc::LS_WARNING;
  rtc::LogMessage::LogToDebug(sev);
  P5G_LOG_WARN << "libwebrtc logging enabled (" << v << ") — do not use for measurement runs";
}

// Synchronous getStats() -> JSON file.
class SyncStatsCollector : public webrtc::RTCStatsCollectorCallback {
 public:
  static rtc::scoped_refptr<SyncStatsCollector> Create() {
    return rtc::make_ref_counted<SyncStatsCollector>();
  }
  void OnStatsDelivered(const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      json_ = report->ToJson();
      done_ = true;
    }
    cv_.notify_all();
  }
  bool Wait(std::string* out) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool ok = cv_.wait_for(lk, std::chrono::seconds(2), [this] { return done_; });
    *out = json_;
    return ok;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::string json_;
};

// Periodic getStats() sample appended as one JSON line: {"mono_ns":..,"wall_ns":..,"stats":[...]}.
// This is the W3C webrtc-stats report (RTCStatsReport::ToJson), i.e. the official per-second view of
// outbound-rtp / inbound-rtp / candidate-pair (RTT, availableOutgoingBitrate = GCC estimate) / codec.
inline void AppendStatsLine(webrtc::PeerConnectionInterface* pc, const std::string& path) {
  if (!pc) return;
  auto c = SyncStatsCollector::Create();
  const int64_t mono = NowMonoNs(), wall = NowWallNs();
  pc->GetStats(c.get());
  std::string json;
  if (!c->Wait(&json) || json.empty()) {
    P5G_LOG_WARN << "periodic getStats failed for " << path;
    return;
  }
  std::ofstream out(path, std::ios::app);
  out << "{\"mono_ns\":" << mono << ",\"wall_ns\":" << wall << ",\"stats\":" << json << "}\n";
}

// SDP observers (examples/peerconnection/client/conductor.cc style, lambdas instead of classes).
class SetSdpObserver : public webrtc::SetSessionDescriptionObserver {
 public:
  static rtc::scoped_refptr<SetSdpObserver> Create(std::function<void()> on_ok = {}) {
    return rtc::make_ref_counted<SetSdpObserver>(std::move(on_ok));
  }
  explicit SetSdpObserver(std::function<void()> on_ok) : on_ok_(std::move(on_ok)) {}
  void OnSuccess() override {
    if (on_ok_) on_ok_();
  }
  void OnFailure(webrtc::RTCError error) override {
    P5G_LOG_ERROR << "SetSessionDescription failed: " << error.message();
  }

 private:
  std::function<void()> on_ok_;
};

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  using OnOk = std::function<void(webrtc::SessionDescriptionInterface*)>;
  static rtc::scoped_refptr<CreateSdpObserver> Create(OnOk on_ok) {
    return rtc::make_ref_counted<CreateSdpObserver>(std::move(on_ok));
  }
  explicit CreateSdpObserver(OnOk on_ok) : on_ok_(std::move(on_ok)) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override { on_ok_(desc); }
  void OnFailure(webrtc::RTCError error) override {
    P5G_LOG_ERROR << "CreateSessionDescription failed: " << error.message();
  }

 private:
  OnOk on_ok_;
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_WEBRTC_SESSION_H
