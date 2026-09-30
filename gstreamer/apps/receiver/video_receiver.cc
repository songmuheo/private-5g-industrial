// video_receiver (gstreamer tree) — RTP/H.264 receiver with per-packet / per-frame tracing.
//
// Based on: subprojects/gst-plugins-good/tests/examples/rtp/client-PCMA.c @ GStreamer 1.20.3
//           (rtpbin receive session: udpsrc -> recv_rtp_sink_0, udpsrc -> recv_rtcp_sink_0,
//           send_rtcp_src_0 -> udpsink sync=false async=false, depayloader linked in pad-added).
// Local modifications: video (rtph264depay -> avdec_h264 -> fakesink), RTP/RTCP sockets created by us
//           so the ports are known before the pipeline starts and RTCP is symmetric (RFC 4961: reports
//           leave from the port the sender's RTCP arrives on), sender address from the control channel,
//           per-frame side information carried as metas (gst_util.h) so no lookup table is needed
//           between the jitter buffer and the app sink, pad probes for the traces.
//
// One receiver process per stream (the same rule as the webrtc tree): a second SSRC on the port is
// logged and discarded, never mixed into the traces.
//
// Traces (all in --trace-dir, opened when the sender announces the stream):
//   <stream>-rx-rtp.csv       every RTP packet in (udpsrc output: after the kernel socket read)
//   <stream>-rx-rtcp.csv      every RTCP compound packet in / out
//   <stream>-rx-decoded.csv   every frame through the decoder (in: jitter-buffer exit; out: decoded)
//   <stream>-rx-frames.csv    every decoded frame reaching the app (fakesink)
//   <stream>-rx-stats.jsonl   rtpsession stats every --stats-period-ms (0 = off)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <gio/gio.h>
#include <gst/gst.h>

#include "app_util.h"
#include "control_client.h"
#include "gst_util.h"
#include "trace_ring.h"

namespace p5g {

struct ReceiverConfig {
  std::string control_host = "127.0.0.1";
  int control_port = 8765;
  std::string session = "s1";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  std::string advertise_host;  // address senders should send to; empty = they use the control host
  int rtp_port = 0;            // 0 = ephemeral (reported to the control server); RTCP = separate socket
  int jitter_ms = 50;          // rtpbin latency (rtpjitterbuffer); stock default is 200
  int drop_late = 0;           // rtpjitterbuffer drop-on-latency
  int stats_period_ms = 1000;
  int duration_s = 0;
};
static ReceiverConfig g_cfg;

class Receiver {
 public:
  Receiver() { BuildPipeline(); }

  bool Run() {
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) P5G_FATAL("pipeline PLAYING failed");
    if (!ctl_.Connect(g_cfg.control_host, g_cfg.control_port)) {
      P5G_LOG_ERROR << "control connect failed (" << g_cfg.control_host << ":" << g_cfg.control_port << ")";
      return false;
    }
    ctl_.Start([this](const json& m) { OnMessage(m); });
    json reg = {{"type", "register"}, {"role", "receiver"}, {"session", g_cfg.session}, {"receiver", g_cfg.receiver_id},
                {"rtp_port", rtp_port_}, {"rtcp_port", rtcp_port_}};
    if (!g_cfg.advertise_host.empty()) reg["host"] = g_cfg.advertise_host;
    P5G_LOG_INFO << "listening rtp=" << rtp_port_ << " rtcp=" << rtcp_port_ << " as " << g_cfg.receiver_id;
    return ctl_.Send(reg);
  }

  void AppendStats() {  // main thread
    if (!stats_) return;
    GObject* session = nullptr;
    g_signal_emit_by_name(rtpbin_, "get-internal-session", 0, &session);
    std::string st = "null";
    if (session) {
      GstStructure* s = nullptr;
      g_object_get(session, "stats", &s, nullptr);
      if (s) { gchar* str = gst_structure_to_string(s); st = json(std::string(str)).dump(); g_free(str); gst_structure_free(s); }
      g_object_unref(session);
    }
    std::fprintf(stats_, "{\"mono_ns\":%lld,\"wall_ns\":%lld,\"frames\":%lld,\"rtpsession\":%s}\n", (long long)NowMonoNs(),
                 (long long)NowWallNs(), (long long)frame_idx_.load(), st.c_str());
    std::fflush(stats_);
  }

  void Shutdown() {
    ctl_.Stop();
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    // Traces after the pipeline is NULL: no streaming thread can still write.
    CloseTraces();
    if (pipeline_) gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    if (rtp_socket_) g_object_unref(rtp_socket_);
    if (rtcp_socket_) g_object_unref(rtcp_socket_);
    rtp_socket_ = rtcp_socket_ = nullptr;
  }

 private:
  // gst-launch equivalent (as in the upstream client example):
  //   udpsrc name=rtpsrc socket=A caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264" ! rtpbin.recv_rtp_sink_0
  //   udpsrc name=rtcpsrc socket=B ! rtpbin.recv_rtcp_sink_0
  //   rtpbin.send_rtcp_src_0 ! udpsink name=rtcpsink socket=B sync=false async=false
  //   rtpbin. (pad-added) ! rtph264depay name=depay ! avdec_h264 name=dec ! fakesink name=sink sync=false
  void BuildPipeline() {
    pipeline_ = gst_pipeline_new("receiver");
    rtpbin_ = Make("rtpbin", "rtpbin");
    GstElement* rtpsrc = Make("udpsrc", "rtpsrc");
    GstElement* rtcpsrc = Make("udpsrc", "rtcpsrc");
    rtcpsink_ = Make("udpsink", "rtcpsink");
    depay_ = Make("rtph264depay", "depay");
    dec_ = Make("avdec_h264", "dec");
    sink_ = Make("fakesink", "sink");

    rtp_socket_ = BindUdp(g_cfg.rtp_port, &rtp_port_);
    rtcp_socket_ = BindUdp(0, &rtcp_port_);
    GstCaps* caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "video", "clock-rate", G_TYPE_INT, (int)kVideoClockRate,
                                        "encoding-name", G_TYPE_STRING, "H264", nullptr);
    g_object_set(rtpsrc, "socket", rtp_socket_, "close-socket", FALSE, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(rtcpsrc, "socket", rtcp_socket_, "close-socket", FALSE, nullptr);
    g_object_set(rtcpsink_, "socket", rtcp_socket_, "close-socket", FALSE, "sync", FALSE, "async", FALSE, "host", g_cfg.control_host.c_str(),
                 "port", 5005, nullptr);  // real destination set on stream-start
    g_object_set(rtpbin_, "latency", (guint)g_cfg.jitter_ms, "drop-on-latency", g_cfg.drop_late ? TRUE : FALSE, nullptr);
    g_object_set(sink_, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), rtpbin_, rtpsrc, rtcpsrc, rtcpsink_, depay_, dec_, sink_, nullptr);
    if (!gst_element_link_many(depay_, dec_, sink_, nullptr)) P5G_FATAL("link depay -> avdec_h264 -> sink failed");
    LinkToRequest(rtpsrc, "src", rtpbin_, "recv_rtp_sink_0");
    LinkToRequest(rtcpsrc, "src", rtpbin_, "recv_rtcp_sink_0");
    GstPad* rtcp_src = gst_element_request_pad_simple(rtpbin_, "send_rtcp_src_0");
    GstPad* rtcp_sinkpad = gst_element_get_static_pad(rtcpsink_, "sink");
    if (gst_pad_link(rtcp_src, rtcp_sinkpad) != GST_PAD_LINK_OK) P5G_FATAL("link send_rtcp_src_0 -> rtcpsink failed");
    gst_object_unref(rtcp_sinkpad);
    g_signal_connect(rtpbin_, "pad-added", G_CALLBACK(&Receiver::OnPadAdded), this);

    AddProbe(rtpsrc, "src", &Receiver::OnRtpIn);
    AddProbe(rtcpsrc, "src", &Receiver::OnRtcpIn);
    AddProbe(rtcpsink_, "sink", &Receiver::OnRtcpOut);
    AddProbe(depay_, "sink", &Receiver::OnDepayIn);
    AddProbe(depay_, "src", &Receiver::OnDepayOut);
    AddProbe(dec_, "sink", &Receiver::OnDecodeIn);
    AddProbe(dec_, "src", &Receiver::OnDecodeOut);
    AddProbe(sink_, "sink", &Receiver::OnFrame);

    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_set_sync_handler(bus, &BusSyncHandler, nullptr, nullptr);
    gst_object_unref(bus);
  }

  static GstElement* Make(const char* factory, const char* name) {
    GstElement* e = gst_element_factory_make(factory, name);
    if (!e) P5G_FATAL("element '" << factory << "' not available (install the GStreamer plugin; see gstreamer/README.md)");
    return e;
  }
  static GSocket* BindUdp(int port, int* bound_port) {
    GError* err = nullptr;
    GSocket* s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &err);
    if (!s) P5G_FATAL("udp socket: " << err->message);
    GSocketAddress* a = g_inet_socket_address_new_from_string("0.0.0.0", port);
    if (!g_socket_bind(s, a, TRUE, &err)) P5G_FATAL("udp bind port " << port << ": " << err->message);
    g_object_unref(a);
    GSocketAddress* local = g_socket_get_local_address(s, nullptr);
    *bound_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
    g_object_unref(local);
    return s;
  }
  static void LinkToRequest(GstElement* from, const char* src, GstElement* rtpbin, const char* req) {
    GstPad* s = gst_element_request_pad_simple(rtpbin, req);
    GstPad* p = gst_element_get_static_pad(from, src);
    if (!s || !p || gst_pad_link(p, s) != GST_PAD_LINK_OK) P5G_FATAL("link " << src << " -> " << req << " failed");
    gst_object_unref(p);
  }
  using ProbeFn = void (Receiver::*)(GstPad*, GstPadProbeInfo*);
  struct ProbeCtx { Receiver* self; ProbeFn fn; };
  void AddProbe(GstElement* e, const char* pad_name, ProbeFn fn) {
    GstPad* pad = gst_element_get_static_pad(e, pad_name);
    auto* ctx = new ProbeCtx{this, fn};  // lives as long as the process
    gst_pad_add_probe(pad, kBufferProbes,
                      [](GstPad* p, GstPadProbeInfo* info, gpointer ud) -> GstPadProbeReturn {
                        auto* c = static_cast<ProbeCtx*>(ud);
                        (c->self->*(c->fn))(p, info);
                        return GST_PAD_PROBE_OK;
                      },
                      ctx, nullptr);
    gst_object_unref(pad);
  }

  // rtpbin creates recv_rtp_src_0_<ssrc>_<pt> when the first packet of an SSRC arrives.
  static void OnPadAdded(GstElement*, GstPad* new_pad, gpointer ud) {
    auto* self = static_cast<Receiver*>(ud);
    gchar* name = gst_pad_get_name(new_pad);
    if (g_str_has_prefix(name, "recv_rtp_src_0_")) {
      if (self->linked_.exchange(true)) {
        // A second SSRC on this receiver's port: refuse (one stream per process), sink it silently.
        P5G_LOG_ERROR << "second RTP source " << name << " on this receiver -> discarded (one stream per receiver process)";
        GstElement* trash = gst_element_factory_make("fakesink", nullptr);
        g_object_set(trash, "sync", FALSE, "async", FALSE, nullptr);
        gst_bin_add(GST_BIN(self->pipeline_), trash);
        gst_element_sync_state_with_parent(trash);
        GstPad* s = gst_element_get_static_pad(trash, "sink");
        gst_pad_link(new_pad, s);
        gst_object_unref(s);
      } else {
        GstPad* s = gst_element_get_static_pad(self->depay_, "sink");
        if (gst_pad_link(new_pad, s) != GST_PAD_LINK_OK) P5G_LOG_ERROR << "link " << name << " -> depay failed";
        gst_object_unref(s);
        P5G_LOG_INFO << "rtp source linked: " << name;
      }
    }
    g_free(name);
  }

  // ---- probes (streaming threads) ----------------------------------------------------------------
  // udpsrc thread: every packet; also the per-frame arrival summary published for the depay thread.
  void OnRtpIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      RtpPacketRow r;
      if (!FillRtpRow(b, 1, &r)) return;
      if (auto* t = rtp_p_.load(std::memory_order_acquire)) t->Write(r);
      if (r.ssrc != ssrc_.load(std::memory_order_relaxed)) return;
      if (cur_n_ == 0 || r.rtp_ts != cur_ts_) {
        if (cur_n_ > 0) PublishArrival();
        cur_ts_ = r.rtp_ts; cur_first_ = r.log_mono_ns; cur_n_ = 0;
      }
      cur_last_ = r.log_mono_ns;
      cur_n_++;
      if (r.marker) { PublishArrival(); cur_n_ = 0; }
    });
  }
  // Arrival summary ring: written by the udpsrc thread, read by the jitter-buffer thread. Seqlock-lite:
  // fields first, then the key with release; the reader re-checks the key after reading the fields.
  struct Arrival { std::atomic<uint64_t> key{~0ull}; int64_t first = 0, last = 0; int32_t n = 0; };
  static constexpr int kArrivalRing = 64;
  void PublishArrival() {
    Arrival& a = arrivals_[cur_ts_ % kArrivalRing];
    a.key.store(~0ull, std::memory_order_release);
    a.first = cur_first_; a.last = cur_last_; a.n = cur_n_;
    a.key.store(cur_ts_, std::memory_order_release);
  }
  bool LookupArrival(uint32_t ts, int64_t* first, int64_t* last, int32_t* n) {
    const Arrival& a = arrivals_[ts % kArrivalRing];
    if (a.key.load(std::memory_order_acquire) != ts) return false;
    *first = a.first; *last = a.last; *n = a.n;
    return a.key.load(std::memory_order_acquire) == ts;
  }

  // Jitter-buffer output thread: RTP packets entering the depayloader. The marker packet completes
  // the access unit, which rtph264depay pushes synchronously inside this same chain call, so the
  // values kept here are read by OnDepayOut on the same thread.
  void OnDepayIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
      if (!gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) return;
      au_ts_ = gst_rtp_buffer_get_timestamp(&rtp);
      gst_rtp_buffer_unmap(&rtp);
    });
  }
  void OnDepayOut(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      SetFrameMeta(b, FrameMeta::kRtpTs, au_ts_);
      int64_t first = -1, last = -1; int32_t n = -1;
      if (LookupArrival(au_ts_, &first, &last, &n)) {
        SetFrameMeta(b, FrameMeta::kNumPackets, (uint64_t)n);
        SetFrameMeta(b, FrameMeta::kFirstPktMonoNs, (uint64_t)first);
        SetFrameMeta(b, FrameMeta::kLastPktMonoNs, (uint64_t)last);
      }
    });
  }
  void OnDecodeIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      SetFrameMeta(b, FrameMeta::kDecodeStartMonoNs, (uint64_t)NowMonoNs());
      SetFrameMeta(b, FrameMeta::kInputBytes, gst_buffer_get_size(b));
      SetFrameMeta(b, FrameMeta::kIsKey, GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT) ? 0 : 1);
    });
  }
  void OnDecodeOut(GstPad* pad, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      auto* t = decoded_p_.load(std::memory_order_acquire);
      if (!t) return;
      if (!dec_w_) PadResolution(pad, &dec_w_, &dec_h_);  // first frame only
      DecodedFrameLedgerRow r{};
      r.rtp_ts = static_cast<uint32_t>(GetFrameMetaOr(b, FrameMeta::kRtpTs, 0));
      r.decode_start_mono_ns = GetFrameMetaOr(b, FrameMeta::kDecodeStartMonoNs, -1);
      r.decode_done_mono_ns = NowMonoNs();
      r.decode_done_wall_ns = NowWallNs();
      r.input_bytes = static_cast<int32_t>(GetFrameMetaOr(b, FrameMeta::kInputBytes, -1));
      r.frame_type = GetFrameMetaOr(b, FrameMeta::kIsKey, 0) ? 3 : 4;
      r.render_time_ms = -1; r.decoder_decode_time_ms = -1; r.qp = -1;
      r.width = dec_w_; r.height = dec_h_;
      t->Write(r);
    });
  }
  void OnFrame(GstPad* pad, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      auto* t = frames_p_.load(std::memory_order_acquire);
      if (!t) return;
      if (!dec_w_) PadResolution(pad, &dec_w_, &dec_h_);
      DecodedFrameRow r{};
      r.frame_idx = frame_idx_++;
      r.rtp_ts = static_cast<uint32_t>(GetFrameMetaOr(b, FrameMeta::kRtpTs, 0));
      r.abs_capture_ntp_ms = -1;
      r.sender_rtp_ts_est = r.rtp_ts;
      r.wire_offset_est = 0;
      r.recv_wall_ns = NowWallNs();
      r.recv_mono_ns = NowMonoNs();
      r.width = dec_w_; r.height = dec_h_;
      r.num_packets = static_cast<int32_t>(GetFrameMetaOr(b, FrameMeta::kNumPackets, -1));
      r.first_pkt_mono_ns = GetFrameMetaOr(b, FrameMeta::kFirstPktMonoNs, -1);
      r.last_pkt_mono_ns = GetFrameMetaOr(b, FrameMeta::kLastPktMonoNs, -1);
      r.ssrc = ssrc_.load(std::memory_order_relaxed);
      t->Write(r);
    });
  }
  void OnRtcpIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) { auto* t = rtcp_p_.load(std::memory_order_acquire); if (!t) return; RtcpPacketRow r; FillRtcpRow(b, 1, &r); t->Write(r); });
  }
  void OnRtcpOut(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) { auto* t = rtcp_p_.load(std::memory_order_acquire); if (!t) return; RtcpPacketRow r; FillRtcpRow(b, 0, &r); t->Write(r); });
  }

  // ---- control channel (its own thread) -------------------------------------------------------------
  void OnMessage(const json& m) {
    if (m.value("type", "") != "stream-start") return;
    std::lock_guard<std::mutex> lk(mu_);
    const std::string stream = m.value("stream", "");
    if (!stream_.empty()) {
      if (stream != stream_) P5G_LOG_ERROR << "stream-start for a second stream '" << stream << "' on receiver " << g_cfg.receiver_id
                                            << " (already serving '" << stream_ << "') -> refused";
      return;
    }
    stream_ = stream;
    const std::string prefix = g_cfg.trace_dir + "/" + stream_ + "-rx";
    // Open the traces before the first packet arrives (the sender announces, then starts).
    rtp_ = std::make_unique<RtpPacketTrace>(prefix + "-rtp.csv", kRtpPacketHeader, &FormatRtpPacketRow, kPacketTraceCapacity);
    rtcp_ = std::make_unique<RtcpPacketTrace>(prefix + "-rtcp.csv", kRtcpPacketHeader, &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    decoded_ = std::make_unique<DecodedFrameLedgerTrace>(prefix + "-decoded.csv", kDecodedFrameLedgerHeader, &FormatDecodedFrameLedgerRow, kFrameTraceCapacity);
    frames_ = std::make_unique<DecodedFrameTrace>(prefix + "-frames.csv", kDecodedFrameHeader, &FormatDecodedFrameRow, kFrameTraceCapacity);
    if (g_cfg.stats_period_ms > 0) stats_ = std::fopen((prefix + "-stats.jsonl").c_str(), "w");
    ssrc_.store(m.value("ssrc", 0u), std::memory_order_relaxed);
    // Publish to the streaming threads only after the rings exist (release / acquire pairs).
    rtp_p_.store(rtp_.get(), std::memory_order_release);
    rtcp_p_.store(rtcp_.get(), std::memory_order_release);
    decoded_p_.store(decoded_.get(), std::memory_order_release);
    frames_p_.store(frames_.get(), std::memory_order_release);
    const std::string sender_host = m.value("sender_host", g_cfg.control_host);
    const int sender_rtcp = m.value("rtcp_port_local", 0);
    if (sender_rtcp > 0) g_object_set(rtcpsink_, "host", sender_host.c_str(), "port", sender_rtcp, nullptr);
    P5G_LOG_INFO << "stream " << stream_ << " announced: ssrc=" << ssrc_.load() << " " << m.value("width", 0) << "x" << m.value("height", 0)
                 << "@" << m.value("fps", 0) << " " << m.value("bitrate_kbps", 0) << " kbps gop=" << m.value("gop", 0)
                 << "; rtcp reports -> " << sender_host << ":" << sender_rtcp;
  }
  void CloseTraces() {
    std::lock_guard<std::mutex> lk(mu_);
    if (rtp_) rtp_->Close();
    if (rtcp_) rtcp_->Close();
    if (decoded_) decoded_->Close();
    if (frames_) frames_->Close();
    if (stats_) std::fclose(stats_);
    stats_ = nullptr;
  }

  GstElement* pipeline_ = nullptr;
  GstElement* rtpbin_ = nullptr;
  GstElement* rtcpsink_ = nullptr;
  GstElement* depay_ = nullptr;
  GstElement* dec_ = nullptr;
  GstElement* sink_ = nullptr;
  GSocket* rtp_socket_ = nullptr;
  GSocket* rtcp_socket_ = nullptr;
  int rtp_port_ = 0, rtcp_port_ = 0;
  std::atomic<bool> linked_{false};
  std::atomic<uint32_t> ssrc_{0};
  // udpsrc-thread state (per-frame arrival aggregation)
  uint32_t cur_ts_ = 0; int64_t cur_first_ = 0, cur_last_ = 0; int32_t cur_n_ = 0;
  Arrival arrivals_[kArrivalRing];
  // jitter-buffer-thread state
  uint32_t au_ts_ = 0;
  int32_t dec_w_ = 0, dec_h_ = 0;
  std::atomic<int64_t> frame_idx_{0};
  // traces (created on stream-start; the probes see them through the atomic pointers)
  std::atomic<RtpPacketTrace*> rtp_p_{nullptr};
  std::atomic<RtcpPacketTrace*> rtcp_p_{nullptr};
  std::atomic<DecodedFrameLedgerTrace*> decoded_p_{nullptr};
  std::atomic<DecodedFrameTrace*> frames_p_{nullptr};
  std::unique_ptr<RtpPacketTrace> rtp_;
  std::unique_ptr<RtcpPacketTrace> rtcp_;
  std::unique_ptr<DecodedFrameLedgerTrace> decoded_;
  std::unique_ptr<DecodedFrameTrace> frames_;
  std::FILE* stats_ = nullptr;
  std::string stream_;
  std::mutex mu_;  // control-thread state (never taken on a streaming thread)
  ControlClient ctl_;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr,
               "video_receiver --control-host H --control-port P --session S --receiver-id RID --trace-dir DIR\n"
               "               [--rtp-port N(=ephemeral)] [--advertise-host A] [--jitter-ms 50] [--drop-late 0|1]\n"
               "               [--stats-period-ms 1000] [--duration S]\n");
}

int main(int argc, char** argv) {
  p5g::CliArgs a(argc, argv, {"help", "control-host", "control-port", "session", "receiver-id", "trace-dir", "rtp-port",
                              "advertise-host", "jitter-ms", "drop-late", "stats-period-ms", "duration"});
  if (a.Has("help")) { Usage(); return 0; }
  auto& c = p5g::g_cfg;
  c.control_host = a.Get("control-host", c.control_host);
  c.control_port = a.GetInt("control-port", c.control_port);
  c.session = a.Get("session", c.session);
  c.receiver_id = a.Get("receiver-id", c.receiver_id);
  c.trace_dir = a.Get("trace-dir", c.trace_dir);
  c.advertise_host = a.Get("advertise-host", "");
  c.rtp_port = a.GetInt("rtp-port", 0);
  c.jitter_ms = a.GetInt("jitter-ms", c.jitter_ms);
  c.drop_late = a.GetInt("drop-late", 0);
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms);
  c.duration_s = a.GetInt("duration", 0);

  gst_init(nullptr, nullptr);
  P5G_LOG_INFO << "config: transport=gstreamer receiver_id=" << c.receiver_id << " session=" << c.session << " control="
               << c.control_host << ":" << c.control_port << " rtp_port=" << c.rtp_port << " jitter_ms=" << c.jitter_ms
               << " drop_late=" << c.drop_late << " stats_period_ms=" << c.stats_period_ms << " " << p5g::GstProvenance();
  p5g::InstallSignalHandlers();
  {
    p5g::Receiver receiver;
    if (!receiver.Run()) return 1;
    p5g::RunUntilShutdown(c.duration_s, [&] { receiver.AppendStats(); }, c.stats_period_ms);
    receiver.Shutdown();
  }
  return 0;
}
