// video_sender (gstreamer tree) — fixed-profile RTP/H.264 sender with per-frame / per-packet tracing.
//
// Based on: subprojects/gst-plugins-good/tests/examples/rtp/server-alsasrc-PCMA.c @ GStreamer 1.20.3
//           (rtpbin send session: payloader -> send_rtp_sink_0, send_rtp_src_0 -> udpsink,
//           send_rtcp_src_0 -> udpsink sync=false async=false, udpsrc -> recv_rtcp_sink_0) and
//           subprojects/gst-plugins-base/tests/examples/app/appsrc-stream.c (appsrc push mode).
// Local modifications: video instead of audio (appsrc grid source -> x264enc -> rtph264pay), the
//           RTCP out/in share one socket (the receiver's RTCP reports come back to the port we send
//           from), pad probes for the traces, receiver address from the control channel, bitrate
//           changes by `profile` messages while PLAYING.
//
// Profile semantics (docs/SCENARIO_EDGE_PROFILES.md §6): resolution and fps are fixed by construction
// (appsrc caps + capture grid; x264 never skips frames), the bitrate is the x264 target (its default
// ABR + VBV rate control: a ceiling the content may stay under, changeable while PLAYING). Nothing in
// the pipeline adapts to the network: there is no congestion controller, no pacer, no frame dropper.
//
// Deployment: on the testbed this runs on the laptop tethered to a Pixel phone (uplink video through
// the private 5G cell). Locally it runs inside the srsUE network namespace.
//
// Traces (all in --trace-dir, same names/columns as the webrtc tree where the concept exists):
//   <stream>-tx-frames.csv          every capture slot (video_source.h)
//   <stream>-tx-encoded.csv         every encoded access unit (x264enc src pad)
//   <stream>-tx-encoder-rates.csv   every bitrate handed to the encoder (start + profile changes)
//   <stream>-tx-rtp.csv             every RTP packet out (udpsink sink pad)
//   <stream>-tx-rtcp.csv            every RTCP compound packet out / in
//   <stream>-tx-stats.jsonl         rtpsession stats every --stats-period-ms (0 = off)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>

#include <gio/gio.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "app_util.h"
#include "control_client.h"
#include "gst_util.h"
#include "trace_ring.h"
#include "video_source.h"

namespace p5g {

struct SenderConfig {
  std::string control_host = "127.0.0.1";
  int control_port = 8765;
  std::string session = "s1";
  std::string stream_id = "cam0";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  VideoSourceConfig video;
  // encoder profile (x264enc). gop 0 = 2 s (2 * fps). vbv_ms = VBV buffer in ms (x264enc default 600).
  int bitrate_kbps = 2500;
  int gop_frames = 0;
  int vbv_ms = 600;
  std::string preset = "veryfast";  // x264 speed preset; zerolatency tune is always on
  int threads = 4;
  // Who decides the bitrate: "profile" = the value above (and `profile` control messages) — nothing in
  // the stack adapts; "gcc" = rtpgccbwe (Google Congestion Control, gst-plugins-rs) estimates from TWCC
  // feedback and its estimate is applied to x264 through the same path. fps/resolution are caps in both.
  std::string cc = "profile";
  int gcc_min_kbps = 300;
  int gcc_max_kbps = 0;   // 0 = the profile bitrate is the ceiling
  // RTP
  int mtu = 1200;
  int pt = 96;
  uint32_t ssrc = 0;  // 0 = random (rtph264pay default)
  int stats_period_ms = 1000;
  int duration_s = 0;
};
static SenderConfig g_cfg;

std::string TracePrefix() { return g_cfg.trace_dir + "/" + g_cfg.stream_id + "-tx"; }

class Sender {
 public:
  Sender() {
    gst_segment_init(&enc_segment_, GST_FORMAT_TIME);
    frames_ = std::make_unique<CaptureFrameTrace>(TracePrefix() + "-frames.csv", kCaptureFrameHeader, &FormatCaptureFrameRow, kFrameTraceCapacity);
    encoded_ = std::make_unique<EncodedFrameTrace>(TracePrefix() + "-encoded.csv", kEncodedFrameHeader, &FormatEncodedFrameRow, kFrameTraceCapacity);
    rates_ = std::make_unique<EncoderRateTrace>(TracePrefix() + "-encoder-rates.csv", kEncoderRateHeader, &FormatEncoderRateRow, kFrameTraceCapacity);
    rtp_ = std::make_unique<RtpPacketTrace>(TracePrefix() + "-rtp.csv", kRtpPacketHeader, &FormatRtpPacketRow, kPacketTraceCapacity);
    rtcp_ = std::make_unique<RtcpPacketTrace>(TracePrefix() + "-rtcp.csv", kRtcpPacketHeader, &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    stats_ = g_cfg.stats_period_ms > 0 ? std::fopen((TracePrefix() + "-stats.jsonl").c_str(), "w") : nullptr;
    if (g_cfg.cc == "gcc")
      cc_ = std::make_unique<CcUpdateTrace>(TracePrefix() + "-cc.csv", kCcUpdateHeader, &FormatCcUpdateRow, kFrameTraceCapacity);
    BuildPipeline();
  }

  bool Run() {
    if (!ctl_.Connect(g_cfg.control_host, g_cfg.control_port)) {
      P5G_LOG_ERROR << "control connect failed (" << g_cfg.control_host << ":" << g_cfg.control_port << ")";
      return false;
    }
    ctl_.Start([this](const json& m) { OnMessage(m); });
    return ctl_.Send({{"type", "register"}, {"role", "sender"}, {"session", g_cfg.session}, {"stream", g_cfg.stream_id}, {"to", g_cfg.receiver_id}});
  }

  // Periodic (main thread): rtpsession stats as one JSON line. Not on a streaming thread.
  // Also the watchdog for the stream-ack handshake (a receiver that never answers must not hang the run).
  void AppendStats(int tick_ms) {
    if (!started_) {
      const int64_t dl = ack_deadline_mono_ns_.load();
      if (dl && NowMonoNs() > dl) P5G_FATAL("no stream-ack from receiver " << g_cfg.receiver_id << " within " << kAckTimeoutNs / 1000000000 << " s");
      return;
    }
    if (!stats_) return;
    (void)tick_ms;
    const int64_t now = NowMonoNs();
    if (next_stats_mono_ns_ == 0) next_stats_mono_ns_ = now;
    if (now < next_stats_mono_ns_) return;                       // sample on a monotonic schedule
    next_stats_mono_ns_ += (int64_t)g_cfg.stats_period_ms * 1000000;
    if (next_stats_mono_ns_ < now) next_stats_mono_ns_ = now;    // fell behind (long stall): resync
    GObject* session = nullptr;
    g_signal_emit_by_name(rtpbin_, "get-internal-session", 0, &session);
    std::string st = "null";
    if (session) {
      GstStructure* s = nullptr;
      g_object_get(session, "stats", &s, nullptr);
      if (s) {
        gchar* str = gst_structure_to_string(s);
        st = json(std::string(str)).dump();  // the structure string, JSON-escaped
        g_free(str);
        gst_structure_free(s);
      }
      g_object_unref(session);
    }
    std::fprintf(stats_, "{\"mono_ns\":%lld,\"wall_ns\":%lld,\"frames_pushed\":%lld,\"bitrate_kbps\":%d,\"rtpsession\":%s}\n",
                 (long long)NowMonoNs(), (long long)NowWallNs(), (long long)(source_ ? source_->frames_pushed() : 0),
                 bitrate_kbps_.load(), st.c_str());
    std::fflush(stats_);
  }

  // Ordered teardown: control -> grid stops offering -> drain by observation: wait until the last pushed
  // frame has come out of the encoder (encoder probe) and its marker packet has left the sender (udpsink
  // probe), so tx-frames == tx-encoded == frames on the wire -> pipeline NULL -> join the grid thread ->
  // traces. No EOS on the normal path: rtpgccbwe (0.13.7) forwards EOS without draining its pacing queue,
  // which would lose the last frame in --cc gcc. EOS is the fallback only when the encoder does not drain
  // by itself (it also releases a push blocked on a full appsrc queue). The join comes last on purpose:
  // joining a thread blocked in gst_app_src_push_buffer would deadlock if downstream had stopped consuming.
  void Shutdown() {
    ctl_.Stop();
    if (source_) {
      source_->RequestStop();
      // up to 3 frame intervals for the loop to leave its current slot (a push blocked on a full queue
      // does not finish here; the EOS fallback below releases it)
      if (!source_->WaitStopped(3 * 1000 / (g_cfg.video.fps > 0 ? g_cfg.video.fps : 30) + 5))
        P5G_LOG_WARN << "grid thread still pushing at shutdown (blocked push?)";
    }
    if (pipeline_ && started_) {
      const uint32_t last_pushed = source_->last_pushed_rtp_ts();
      auto wait_for = [](std::function<bool()> ok, int ms) { for (int i = 0; i < ms / 10 && !ok(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10)); return ok(); };
      // 1) encoder: x264 zerolatency has no frame delay, appsrc hands the frame over within a few ms
      if (!wait_for([&] { return last_encoded_ts_.load() == last_pushed; }, 1000)) {
        P5G_LOG_WARN << "last pushed frame (rtp_ts " << last_pushed << ") not out of the encoder after 1 s; sending EOS to flush";
        gst_app_src_end_of_stream(GST_APP_SRC(src_));
        wait_for([&] { return eos_seen_.load(); }, 2000);
      }
      // 2) sender: the last frame's marker packet has passed the udpsink probe (rtpgccbwe pacing queue drained)
      if (!wait_for([&] { return last_sent_ts_.load() == last_encoded_ts_.load(); }, 3000))
        P5G_LOG_WARN << "last encoded frame (rtp_ts " << last_encoded_ts_.load() << ") not sent within 3 s; packets still queued are lost";
    }
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (source_) {
      source_->Join();
      if (source_->skipped_slots() > 0)
        P5G_LOG_WARN << source_->skipped_slots() << " capture slots were missed while a push was blocked (encoder overloaded):"
                     << " logged as to_encoder=0 in tx-frames.csv";
    }
    if (frames_) frames_->Close();
    if (encoded_) encoded_->Close();
    if (rates_) rates_->Close();
    if (rtp_) rtp_->Close();
    if (rtcp_) rtcp_->Close();
    if (cc_) cc_->Close();
    if (stats_) std::fclose(stats_);
    stats_ = nullptr;
    if (pipeline_) gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    if (rtcp_socket_) g_object_unref(rtcp_socket_);
    rtcp_socket_ = nullptr;
  }

 private:
  // gst-launch equivalent (rtpbin wiring as in the upstream example):
  //   appsrc name=src is-live=true format=time block=true caps=video/x-raw,format=I420,width=W,height=H,framerate=F/1
  //   ! x264enc name=enc tune=zerolatency speed-preset=P pass=cbr bitrate=B vbv-buf-capacity=V key-int-max=G
  //             threads=T byte-stream=true option-string=scenecut=0:min-keyint=G
  //   ! rtph264pay name=pay pt=PT mtu=MTU ssrc=S timestamp-offset=0 seqnum-offset=0 config-interval=-1 aggregate-mode=zero-latency
  //   ! rtpbin.send_rtp_sink_0   rtpbin.send_rtp_src_0 ! udpsink name=rtpsink sync=false async=false
  //   rtpbin.send_rtcp_src_0 ! udpsink name=rtcpsink sync=false async=false socket=S
  //   udpsrc name=rtcpsrc socket=S ! rtpbin.recv_rtcp_sink_0
  void BuildPipeline() {
    pipeline_ = gst_pipeline_new("sender");
    GstElement* src = Make("appsrc", "src");
    src_ = src;
    GstElement* enc = Make("x264enc", "enc");
    GstElement* pay = Make("rtph264pay", "pay");
    pay_ = pay;
    rtpbin_ = Make("rtpbin", "rtpbin");
    rtpsink_ = Make("udpsink", "rtpsink");
    rtcpsink_ = Make("udpsink", "rtcpsink");
    GstElement* rtcpsrc = Make("udpsrc", "rtcpsrc");
    enc_ = enc;

    // appsrc: live, timestamps from us, block the grid thread if the encoder falls behind (visible as
    // grid gaps in tx-frames.csv instead of silently dropped frames).
    source_ = std::make_unique<GridVideoSource>(g_cfg.video, GST_APP_SRC(src), frames_.get());
    GstCaps* caps = source_->MakeCaps();
    const guint64 frame_bytes = (guint64)g_cfg.video.width * g_cfg.video.height * 3 / 2;
    g_object_set(src, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", TRUE, "do-timestamp", FALSE,
                 "max-bytes", 3 * frame_bytes, "caps", caps, nullptr);  // queue at most 3 frames, then block the grid
    gst_caps_unref(caps);

    // x264enc: zerolatency (bframes 0, rc-lookahead 0, sliced threads, no mb-tree), CBR = ABR + VBV,
    // deterministic GOP (scenecut off, min = max key interval). Only `bitrate` / `vbv-buf-capacity`
    // are changeable while PLAYING (gst-inspect flags); the rest is fixed for the run.
    const int gop = g_cfg.gop_frames > 0 ? g_cfg.gop_frames : 2 * g_cfg.video.fps;
    gst_util_set_object_arg(G_OBJECT(enc), "tune", "zerolatency");
    gst_util_set_object_arg(G_OBJECT(enc), "speed-preset", g_cfg.preset.c_str());
    gst_util_set_object_arg(G_OBJECT(enc), "pass", "cbr");
    const std::string opts = "scenecut=0:min-keyint=" + std::to_string(gop);
    g_object_set(enc, "bitrate", (guint)g_cfg.bitrate_kbps, "vbv-buf-capacity", (guint)g_cfg.vbv_ms, "key-int-max", (guint)gop,
                 "threads", (guint)g_cfg.threads, "byte-stream", TRUE, "option-string", opts.c_str(), nullptr);
    bitrate_kbps_ = g_cfg.bitrate_kbps;
    gop_ = gop;

    // rtph264pay: RFC 6184, FU-A above mtu, STAP-A aggregation of an access unit, SPS/PPS with every
    // IDR, deterministic RTP timestamp/sequence origin (timestamp-offset 0 -> rtp_ts = PTS * 90 kHz).
    g_object_set(pay, "pt", (guint)g_cfg.pt, "mtu", (guint)g_cfg.mtu, "timestamp-offset", (guint)0, "seqnum-offset", (gint)0,
                 "config-interval", (gint)-1, nullptr);
    gst_util_set_object_arg(G_OBJECT(pay), "aggregate-mode", "zero-latency");
    // The SSRC is chosen here (random unless --ssrc) so the receiver can be told before the first
    // packet leaves (stream-start precedes PLAYING); rtph264pay would otherwise pick one at start.
    if (!g_cfg.ssrc) { g_cfg.ssrc = static_cast<uint32_t>(g_random_int()); if (!g_cfg.ssrc) g_cfg.ssrc = 1; }
    g_object_set(pay, "ssrc", (guint)g_cfg.ssrc, nullptr);

    // RTP out: plain UDP, no clock sync (a frame leaves as soon as it is payloaded = one burst).
    g_object_set(rtpsink_, "sync", FALSE, "async", FALSE, nullptr);
    // RTCP out and in on ONE socket, so the receiver's reports (sent to the source port of our SRs)
    // reach us also through a NAT.
    GError* err = nullptr;
    rtcp_socket_ = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &err);
    if (!rtcp_socket_) P5G_FATAL("rtcp socket: " << err->message);
    GSocketAddress* any = g_inet_socket_address_new_from_string("0.0.0.0", 0);
    if (!g_socket_bind(rtcp_socket_, any, TRUE, &err)) P5G_FATAL("rtcp bind: " << err->message);
    g_object_unref(any);
    GSocketAddress* local = g_socket_get_local_address(rtcp_socket_, nullptr);
    rtcp_port_local_ = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
    g_object_unref(local);
    g_object_set(rtcpsink_, "sync", FALSE, "async", FALSE, "socket", rtcp_socket_, "close-socket", FALSE, nullptr);
    g_object_set(rtcpsrc, "socket", rtcp_socket_, "close-socket", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), src, enc, pay, rtpbin_, rtpsink_, rtcpsink_, rtcpsrc, nullptr);
    if (!gst_element_link_many(src, enc, pay, nullptr)) P5G_FATAL("link appsrc -> x264enc -> rtph264pay failed");
    if (g_cfg.cc == "gcc") {
      // GCC condition: TWCC extension on every packet (the receiver's session answers with transport-wide
      // feedback, RFC 8888-style), rtpgccbwe right before the send session (rtpgccbwe docs: "must be placed
      // right before an rtpsession"; it consumes the RTPTWCCPackets upstream event), AVPF profile for early
      // feedback. The estimate is applied in OnEstimate through the same code as a `profile` message.
      if (!EnsureRustPlugins("rtpgccbwe"))
        P5G_FATAL("--cc gcc needs the rtpgccbwe element (gst-plugins-rs): run gstreamer/scripts/build_gst_rs.sh or set GST_PLUGIN_PATH");
      GstRTPHeaderExtension* twcc = gst_rtp_header_extension_create_from_uri(kTwccUri);
      if (!twcc) P5G_FATAL("TWCC header extension implementation not found (gst-plugins-good rtpmanagerbad?)");
      gst_rtp_header_extension_set_id(twcc, kTwccExtId);
      g_signal_emit_by_name(pay, "add-extension", twcc);
      gst_object_unref(twcc);
      bwe_ = Make("rtpgccbwe", "bwe");
      // bounds validated in main(): 0 < gcc_min <= bitrate <= gcc_max (rtpgccbwe clamps with (min, max) and
      // panics on reversed bounds); the profile bitrate is the estimator's start value and x264's first target
      g_object_set(bwe_, "min-bitrate", (guint)g_cfg.gcc_min_kbps * 1000, "max-bitrate", (guint)g_cfg.gcc_max_kbps * 1000,
                   "estimated-bitrate", (guint)g_cfg.bitrate_kbps * 1000, nullptr);
      gst_util_set_object_arg(G_OBJECT(rtpbin_), "rtp-profile", "avpf");
      gst_bin_add(GST_BIN(pipeline_), bwe_);
      if (!gst_element_link(pay, bwe_)) P5G_FATAL("link rtph264pay -> rtpgccbwe failed");
      LinkRequest(bwe_, "src", rtpbin_, "send_rtp_sink_0");
      g_signal_connect(bwe_, "notify::estimated-bitrate", G_CALLBACK(&Sender::OnEstimate), this);
    } else {
      LinkRequest(pay, "src", rtpbin_, "send_rtp_sink_0");
    }
    LinkStatic(rtpbin_, "send_rtp_src_0", rtpsink_, "sink");
    LinkRequestSrc(rtpbin_, "send_rtcp_src_0", rtcpsink_, "sink");
    LinkRequest(rtcpsrc, "src", rtpbin_, "recv_rtcp_sink_0");

    // Probes (streaming threads: POD copy into a ring, nothing else).
    // x264enc shifts its output PTS by a constant (gst_video_encoder_set_min_pts, so DTS never goes
    // negative) and shifts the segment by the same amount: the running time is unchanged and that is
    // what rtph264pay stamps. Track the encoder's output segment to map PTS -> running time -> rtp_ts.
    {
      GstPad* pad = gst_element_get_static_pad(enc, "src");
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                        [](GstPad*, GstPadProbeInfo* info, gpointer ud) -> GstPadProbeReturn {
                          GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
                          if (GST_EVENT_TYPE(ev) == GST_EVENT_SEGMENT) gst_event_copy_segment(ev, &static_cast<Sender*>(ud)->enc_segment_);
                          return GST_PAD_PROBE_OK;
                        }, this, nullptr);
      gst_object_unref(pad);
    }
    AddProbe(enc, "src", &Sender::OnEncoded);
    AddProbe(rtpsink_, "sink", &Sender::OnRtpOut);
    AddProbe(rtcpsink_, "sink", &Sender::OnRtcpOut);
    AddProbe(rtcpsrc, "src", &Sender::OnRtcpIn);

    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_set_sync_handler(bus, &BusSyncHandler, &eos_seen_, nullptr);
    gst_object_unref(bus);
  }

  static GstElement* Make(const char* factory, const char* name) {
    GstElement* e = gst_element_factory_make(factory, name);
    if (!e) P5G_FATAL("element '" << factory << "' not available (install the GStreamer plugin; see gstreamer/README.md)");
    return e;
  }
  static void LinkRequest(GstElement* from, const char* src, GstElement* rtpbin, const char* req) {
    GstPad* s = gst_element_request_pad_simple(rtpbin, req);
    GstPad* p = gst_element_get_static_pad(from, src);
    if (!s || !p || gst_pad_link(p, s) != GST_PAD_LINK_OK) P5G_FATAL("link " << src << " -> " << req << " failed");
    gst_object_unref(p);
  }
  // request src pad (rtpbin.send_rtcp_src_N) -> static sink pad
  static void LinkRequestSrc(GstElement* rtpbin, const char* req, GstElement* to, const char* sink) {
    GstPad* p = gst_element_request_pad_simple(rtpbin, req);
    GstPad* s = gst_element_get_static_pad(to, sink);
    if (!p || !s || gst_pad_link(p, s) != GST_PAD_LINK_OK) P5G_FATAL("link " << req << " -> " << sink << " failed");
    gst_object_unref(s);
  }
  static void LinkStatic(GstElement* from, const char* src, GstElement* to, const char* sink) {
    GstPad* p = gst_element_get_static_pad(from, src);
    GstPad* s = gst_element_get_static_pad(to, sink);
    if (!p || !s || gst_pad_link(p, s) != GST_PAD_LINK_OK) P5G_FATAL("link " << src << " -> " << sink << " failed");
    gst_object_unref(p);
    gst_object_unref(s);
  }
  using ProbeFn = void (Sender::*)(GstPad*, GstPadProbeInfo*);
  struct ProbeCtx { Sender* self; ProbeFn fn; };
  void AddProbe(GstElement* e, const char* pad_name, ProbeFn fn) {
    GstPad* pad = gst_element_get_static_pad(e, pad_name);
    auto* ctx = new ProbeCtx{this, fn};  // lives as long as the process (set-up time, not a streaming thread)
    gst_pad_add_probe(pad, kBufferProbes,
                      [](GstPad* p, GstPadProbeInfo* info, gpointer ud) -> GstPadProbeReturn {
                        auto* c = static_cast<ProbeCtx*>(ud);
                        (c->self->*(c->fn))(p, info);
                        return GST_PAD_PROBE_OK;
                      },
                      ctx, nullptr);
    gst_object_unref(pad);
  }

  void OnEncoded(GstPad* pad, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      EncodedFrameRow r{};
      const GstClockTime pts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(b))
                                   ? gst_segment_to_running_time(&enc_segment_, GST_FORMAT_TIME, GST_BUFFER_PTS(b))
                                   : GST_CLOCK_TIME_NONE;  // running time == the grid PTS the source stamped
      r.rtp_ts = GST_CLOCK_TIME_IS_VALID(pts) ? RtpTsFromRunningTime(pts) : 0;
      r.encode_done_mono_ns = NowMonoNs();
      r.encode_done_wall_ns = NowWallNs();
      r.bytes = static_cast<int32_t>(gst_buffer_get_size(b));
      r.width = enc_w_; r.height = enc_h_;
      if (!r.width) { PadResolution(pad, &enc_w_, &enc_h_); r.width = enc_w_; r.height = enc_h_; }  // first frame only (caps query)
      const bool key = !GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT);
      r.frame_type = key ? 3 : 4;
      r.qp = -1; r.temporal_idx = -1; r.spatial_idx = -1; r.simulcast_idx = -1;
      r.capture_time_ms = GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<int64_t>(pts / GST_MSECOND) : -1;
      r.ntp_time_ms = -1;
      r.codec = 4;
      r.is_idr = key ? 1 : 0;
      r.at_target_quality = -1;
      encoded_->Write(r);
      last_encoded_ts_.store(r.rtp_ts, std::memory_order_relaxed);
    });
  }
  void OnRtpOut(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      RtpPacketRow r;
      if (!FillRtpRow(b, 0, &r)) return;
      rtp_->Write(r);
      if (r.marker) last_sent_ts_.store(r.rtp_ts, std::memory_order_relaxed);
    });
  }
  void OnRtcpOut(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) { RtcpPacketRow r; FillRtcpRow(b, 0, &r); rtcp_->Write(r); });
  }
  void OnRtcpIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) { RtcpPacketRow r; FillRtcpRow(b, 1, &r); rtcp_->Write(r); });
  }

  // rtpgccbwe's thread: new bandwidth estimate -> encoder target (bounded by the profile bitrate as the
  // ceiling unless --gcc-max-kbps says otherwise). Same effect as a `profile` message.
  static void OnEstimate(GObject* obj, GParamSpec*, gpointer ud) {
    auto* self = static_cast<Sender*>(ud);
    guint est = 0; g_object_get(obj, "estimated-bitrate", &est, nullptr);
    const int kbps = static_cast<int>(est / 1000);
    if (kbps <= 0) return;
    if (self->cc_) self->cc_->Write(CcUpdateRow{NowMonoNs(), NowWallNs(), (int64_t)est});
    if (kbps == self->bitrate_kbps_.load()) return;
    g_object_set(self->enc_, "bitrate", (guint)kbps, nullptr);
    self->bitrate_kbps_ = kbps;
    self->rates_->Write(EncoderRateRow{NowMonoNs(), NowWallNs(), (int64_t)kbps * 1000, (int64_t)kbps * 1000, (int64_t)est, (double)g_cfg.video.fps, 1});
  }

  // Control channel (its own thread). GObject property sets are thread-safe.
  void OnMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "receiver-ready") {
      if (m.value("receiver", "") != g_cfg.receiver_id) return;
      std::lock_guard<std::mutex> lk(mu_);
      if (started_) return;
      if (awaiting_ack_) return;
      dest_host_ = m.value("host", g_cfg.control_host);  // receiver on the control host unless it says otherwise
      dest_rtp_port_ = m.at("rtp_port").get<int>();
      dest_rtcp_port_ = m.at("rtcp_port").get<int>();
      g_object_set(rtpsink_, "host", dest_host_.c_str(), "port", dest_rtp_port_, nullptr);
      g_object_set(rtcpsink_, "host", dest_host_.c_str(), "port", dest_rtcp_port_, nullptr);
      // Tell the receiver what is coming and wait for its `stream-ack` (traces opened, SSRC published)
      // BEFORE the first packet leaves, so no packet is missed by the receiver's ledgers.
      ctl_.Send({{"type", "stream-start"}, {"to", g_cfg.receiver_id}, {"stream", g_cfg.stream_id}, {"ssrc", g_cfg.ssrc}, {"pt", g_cfg.pt},
                 {"clock_rate", kVideoClockRate}, {"rtcp_port_local", rtcp_port_local_}, {"width", g_cfg.video.width},
                 {"height", g_cfg.video.height}, {"fps", g_cfg.video.fps}, {"bitrate_kbps", bitrate_kbps_.load()}, {"gop", gop_},
                 {"cc", g_cfg.cc}, {"twcc_ext_id", g_cfg.cc == "gcc" ? kTwccExtId : 0}});
      awaiting_ack_ = true;
      ack_deadline_mono_ns_ = NowMonoNs() + kAckTimeoutNs;
      return;  // continues when the `stream-ack` message arrives (same control thread)
    } else if (type == "stream-ack") {
      if (m.value("stream", "") != g_cfg.stream_id) return;
      std::lock_guard<std::mutex> lk(mu_);
      if (started_ || !awaiting_ack_) return;
      awaiting_ack_ = false;
      if (!m.value("ok", true)) P5G_FATAL("receiver refused the stream: " << m.value("reason", "?"));
      if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) P5G_FATAL("pipeline PLAYING failed");
      // Running time 0 of the pipeline = its base time on the (monotonic) system clock; the grid's PTS
      // are relative to it, so rtp_ts = PTS * 90 kHz is what rtph264pay puts on the wire.
      const int64_t base = static_cast<int64_t>(gst_element_get_base_time(pipeline_));
      rates_->Write(EncoderRateRow{NowMonoNs(), NowWallNs(), (int64_t)bitrate_kbps_ * 1000, (int64_t)bitrate_kbps_ * 1000, -1, (double)g_cfg.video.fps, 1});
      if (cc_) cc_->Write(CcUpdateRow{NowMonoNs(), NowWallNs(), (int64_t)bitrate_kbps_ * 1000});  // start value (the estimator notifies on change only)
      source_->Start(base);
      started_ = true;
      P5G_LOG_INFO << "streaming " << g_cfg.stream_id << " -> " << dest_host_ << ":" << dest_rtp_port_ << " (rtcp " << dest_rtcp_port_
                   << ", ours " << rtcp_port_local_ << ") ssrc=" << g_cfg.ssrc << " base_mono_ns=" << base;
    } else if (type == "profile") {
      // Edge-issued profile change (docs/SCENARIO_EDGE_PROFILES.md §6.2). Bitrate is the only knob that
      // applies without an encoder reopen; width/height/fps changes are refused here on purpose (they
      // need caps renegotiation + IDR = a new epoch, to be added with the RAN-side epoch signalling).
      if (m.value("stream", g_cfg.stream_id) != g_cfg.stream_id) return;
      if (m.contains("bitrate_kbps")) {
        if (g_cfg.cc == "gcc") { P5G_LOG_WARN << "profile: bitrate is under GCC control (--cc gcc); ignored"; return; }
        const int kbps = m.at("bitrate_kbps").get<int>();
        if (kbps <= 0) { P5G_LOG_WARN << "profile: bad bitrate_kbps " << kbps; return; }
        g_object_set(enc_, "bitrate", (guint)kbps, nullptr);
        bitrate_kbps_ = kbps;
        rates_->Write(EncoderRateRow{NowMonoNs(), NowWallNs(), (int64_t)kbps * 1000, (int64_t)kbps * 1000, -1, (double)g_cfg.video.fps, 1});
        P5G_LOG_INFO << "profile: bitrate -> " << kbps << " kbps";
      }
      for (const char* k : {"width", "height", "fps"})
        if (m.contains(k)) P5G_LOG_WARN << "profile: '" << k << "' change not supported at run time (fixed profile); ignored";
    }
  }

  std::unique_ptr<CaptureFrameTrace> frames_;
  std::unique_ptr<EncodedFrameTrace> encoded_;
  std::unique_ptr<EncoderRateTrace> rates_;
  std::unique_ptr<RtpPacketTrace> rtp_;
  std::unique_ptr<RtcpPacketTrace> rtcp_;
  std::unique_ptr<CcUpdateTrace> cc_;   // --cc gcc only
  std::FILE* stats_ = nullptr;
  GstElement* pipeline_ = nullptr;
  GstElement* rtpbin_ = nullptr;
  GstElement* src_ = nullptr;
  GstElement* enc_ = nullptr;
  GstElement* pay_ = nullptr;
  GstElement* bwe_ = nullptr;   // rtpgccbwe (--cc gcc)
  std::atomic<bool> eos_seen_{false};
  GstElement* rtpsink_ = nullptr;
  GstElement* rtcpsink_ = nullptr;
  GSocket* rtcp_socket_ = nullptr;
  int rtcp_port_local_ = 0;
  int gop_ = 0;
  int32_t enc_w_ = 0, enc_h_ = 0;
  GstSegment enc_segment_{};  // encoder output segment (encoder streaming thread only)
  std::atomic<int> bitrate_kbps_{0};
  std::atomic<uint32_t> last_encoded_ts_{0}, last_sent_ts_{0};  // drain check at shutdown
  std::unique_ptr<GridVideoSource> source_;
  ControlClient ctl_;
  std::mutex mu_;  // control-thread state (never taken on a streaming thread)
  std::atomic<bool> started_{false};
  bool awaiting_ack_ = false;  // control thread only
  static constexpr int64_t kAckTimeoutNs = 5LL * 1000000000LL;
  std::atomic<int64_t> ack_deadline_mono_ns_{0};
  int64_t next_stats_mono_ns_ = 0;  // main thread
  std::string dest_host_; int dest_rtp_port_ = 0, dest_rtcp_port_ = 0;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr,
               "video_sender --control-host H --control-port P --session S --stream-id ID --to RECV_ID\n"
               "             --trace-dir DIR [--yuv FILE | (pattern)] --width W --height H --fps F\n"
               "             [--bitrate-kbps 2500] [--gop FRAMES(=2*fps)] [--vbv-ms 600] [--preset veryfast] [--threads 4]\n"
               "             [--cc profile|gcc] [--gcc-min-kbps 300] [--gcc-max-kbps N(=bitrate)]\n"
               "             [--mtu 1200] [--pt 96] [--ssrc N] [--stats-period-ms 1000] [--duration S]\n");
}

int main(int argc, char** argv) {
  p5g::CliArgs a(argc, argv, {"help", "control-host", "control-port", "session", "stream-id", "to", "trace-dir", "yuv", "width",
                              "height", "fps", "bitrate-kbps", "gop", "vbv-ms", "preset", "threads", "mtu", "pt", "ssrc",
                              "stats-period-ms", "duration", "cc", "gcc-min-kbps", "gcc-max-kbps"});
  if (a.Has("help")) { Usage(); return 0; }
  auto& c = p5g::g_cfg;
  c.control_host = a.Get("control-host", c.control_host);
  c.control_port = a.GetInt("control-port", c.control_port);
  c.session = a.Get("session", c.session);
  c.stream_id = a.Get("stream-id", c.stream_id);
  c.receiver_id = a.Get("to", c.receiver_id);
  c.trace_dir = a.Get("trace-dir", c.trace_dir);
  c.video.yuv_path = a.Get("yuv", "");
  c.video.width = a.GetInt("width", c.video.width);
  c.video.height = a.GetInt("height", c.video.height);
  c.video.fps = a.GetInt("fps", c.video.fps);
  c.bitrate_kbps = a.GetInt("bitrate-kbps", c.bitrate_kbps);
  c.gop_frames = a.GetInt("gop", 0);
  c.vbv_ms = a.GetInt("vbv-ms", c.vbv_ms);
  c.preset = a.Get("preset", c.preset);
  c.threads = a.GetInt("threads", c.threads);
  c.mtu = a.GetInt("mtu", c.mtu);
  c.pt = a.GetInt("pt", c.pt);
  c.ssrc = static_cast<uint32_t>(std::strtoul(a.Get("ssrc", "0").c_str(), nullptr, 10));
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms);
  c.duration_s = a.GetInt("duration", 0);
  c.cc = a.Get("cc", c.cc);
  c.gcc_min_kbps = a.GetInt("gcc-min-kbps", c.gcc_min_kbps);
  c.gcc_max_kbps = a.GetInt("gcc-max-kbps", 0);
  if (c.cc != "profile" && c.cc != "gcc") P5G_FATAL("--cc must be profile or gcc");
  if (c.cc == "gcc") {
    if (c.gcc_max_kbps <= 0) c.gcc_max_kbps = c.bitrate_kbps;   // the profile bitrate is the ceiling unless said otherwise
    if (c.gcc_min_kbps <= 0 || c.gcc_min_kbps > c.gcc_max_kbps)
      P5G_FATAL("--gcc-min-kbps " << c.gcc_min_kbps << " must be > 0 and <= --gcc-max-kbps " << c.gcc_max_kbps);
    if (c.bitrate_kbps < c.gcc_min_kbps || c.bitrate_kbps > c.gcc_max_kbps)
      P5G_FATAL("--bitrate-kbps " << c.bitrate_kbps << " (the GCC start value) must lie within [" << c.gcc_min_kbps << ", " << c.gcc_max_kbps << "]");
  }
  if (c.bitrate_kbps <= 0 || c.video.fps <= 0 || c.video.width <= 0 || c.video.height <= 0) P5G_FATAL("bad profile (width/height/fps/bitrate)");

  gst_init(nullptr, nullptr);
  p5g::InitFrameMetaCaps();
  if (c.cc == "gcc") p5g::EnsureRustPlugins("rtpgccbwe");  // so the provenance line below can name its version
  // One provenance line per run: the flags that shape the stream and the library versions.
  P5G_LOG_INFO << "config: transport=gstreamer stream_id=" << c.stream_id << " to=" << c.receiver_id << " session=" << c.session
               << " control=" << c.control_host << ":" << c.control_port << " codec=H264 " << c.video.width << "x" << c.video.height
               << "@" << c.video.fps << " source=" << (c.video.yuv_path.empty() ? "pattern" : c.video.yuv_path)
               << " bitrate_kbps=" << c.bitrate_kbps << " gop=" << (c.gop_frames > 0 ? c.gop_frames : 2 * c.video.fps)
               << " vbv_ms=" << c.vbv_ms << " preset=" << c.preset << " threads=" << c.threads << " cc=" << c.cc
               << (c.cc == "gcc" ? " gcc_min_kbps=" + std::to_string(c.gcc_min_kbps) + " gcc_max_kbps=" + std::to_string(c.gcc_max_kbps) + " rtpgccbwe=" + p5g::PluginVersion("rtpgccbwe") : "")
               << " mtu=" << c.mtu << " pt=" << c.pt
               << " stats_period_ms=" << c.stats_period_ms << " " << p5g::GstProvenance();

  p5g::InstallSignalHandlers();
  {
    p5g::Sender sender;
    if (!sender.Run()) return 1;
    // The tick drives both the stats sample (every stats_period_ms, on a monotonic schedule) and the
    // stream-ack watchdog (at least every second). gcd keeps the stats period exact (1500 ms -> 500 ms tick).
    const int tick_ms = c.stats_period_ms > 0 ? std::max(20, std::gcd(c.stats_period_ms, 1000)) : 1000;
    p5g::RunUntilShutdown(c.duration_s, [&] { sender.AppendStats(tick_ms); }, tick_ms);
    sender.Shutdown();
  }
  return 0;
}
