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
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/net/gstnetaddressmeta.h>

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
    std::FILE* stats = stats_.load(std::memory_order_acquire);
    if (!stats) return;
    GObject* session = nullptr;
    g_signal_emit_by_name(rtpbin_, "get-internal-session", 0, &session);
    std::string st = "null";
    if (session) {
      GstStructure* s = nullptr;
      g_object_get(session, "stats", &s, nullptr);
      if (s) { gchar* str = gst_structure_to_string(s); st = json(std::string(str)).dump(); g_free(str); gst_structure_free(s); }
      g_object_unref(session);
    }
    std::fprintf(stats, "{\"mono_ns\":%lld,\"wall_ns\":%lld,\"frames\":%lld,\"rtpsession\":%s}\n", (long long)NowMonoNs(),
                 (long long)NowWallNs(), (long long)frame_idx_.load(), st.c_str());
    std::fflush(stats);
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
  // udpsrc thread: every packet, and the per-frame arrival summary (first / last arrival, packet count)
  // keyed by rtp_ts for the jitter-buffer thread. Single writer (this thread), readers elsewhere; every
  // shared field is a std::atomic (relaxed stores, ordered by the release/acquire on `key`), so there is
  // no data race in the C++ sense. A packet that belongs to an already published frame (reordering, or a
  // packet after the marker) updates that frame's entry in place while the key still matches.
  void OnRtpIn(GstPad*, GstPadProbeInfo* info) {
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      RtpPacketRow r;
      if (!FillRtpRow(b, 1, &r)) return;
      if (auto* t = rtp_p_.load(std::memory_order_acquire)) t->Write(r);
      if (r.ssrc != ssrc_.load(std::memory_order_relaxed)) return;
      Arrival& a = arrivals_[ArrivalSlot(r.rtp_ts)];
      const bool same_frame = a.key.load(std::memory_order_relaxed) == r.rtp_ts;
      // seqlock: odd seq = write in progress; readers retry / reject until it is even and unchanged
      const uint32_t seq0 = a.seq.load(std::memory_order_relaxed);
      a.seq.store(seq0 + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);  // the odd seq is visible before any field store below
      if (same_frame) {                           // frame already open or published: extend it coherently
        a.n.store(a.n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        a.last.store(r.log_mono_ns, std::memory_order_relaxed);
        if (r.log_mono_ns < a.first.load(std::memory_order_relaxed)) a.first.store(r.log_mono_ns, std::memory_order_relaxed);
      } else {                                    // new frame in this slot (an older frame's entry is recycled: 1024 ms window)
        a.key.store(r.rtp_ts, std::memory_order_relaxed);
        a.first.store(r.log_mono_ns, std::memory_order_relaxed);
        a.last.store(r.log_mono_ns, std::memory_order_relaxed);
        a.n.store(1, std::memory_order_relaxed);
      }
      a.seq.store(seq0 + 2, std::memory_order_release);      // fields visible before the even seq
    });
  }
  // Slot = rtp_ts in ms modulo 1024: adjacent frames (>= 1 ms apart) never share a slot and an entry lives
  // ~1 s; --jitter-ms is capped at kMaxJitterMs so a frame is always looked up while its entry is alive.
  // Single writer (udpsrc thread); readers take a seqlock snapshot: all fields atomic (no UB), the
  // sequence counter guarantees the snapshot is one coherent write.
  struct Arrival {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> key{~0ull};
    std::atomic<int64_t> first{0}, last{0};
    std::atomic<int32_t> n{0};
  };
  static constexpr int kArrivalRing = 1024;
  static constexpr int kMaxJitterMs = 800;
  static int ArrivalSlot(uint32_t rtp_ts) { return static_cast<int>((rtp_ts / (kVideoClockRate / 1000)) % kArrivalRing); }
  bool LookupArrival(uint32_t ts, int64_t* first, int64_t* last, int32_t* n) {
    const Arrival& a = arrivals_[ArrivalSlot(ts)];
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t s0 = a.seq.load(std::memory_order_acquire);
      if (s0 & 1u) continue;                                   // write in progress
      if (a.key.load(std::memory_order_relaxed) != ts) return false;
      *first = a.first.load(std::memory_order_relaxed);
      *last = a.last.load(std::memory_order_relaxed);
      *n = a.n.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (a.seq.load(std::memory_order_relaxed) == s0) return true;  // coherent snapshot
    }
    return false;  // writer kept updating (packets of this frame still arriving): report unknown rather than torn
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
    ForEachProbeBuffer(info, [&](GstBuffer* b) {
      RtcpPacketRow r; FillRtcpRow(b, 1, &r);
      if (auto* t = rtcp_p_.load(std::memory_order_acquire)) t->Write(r);
      // Symmetric RTCP (RFC 4961): reply to the endpoint the sender's RTCP actually comes from. The
      // control channel's guess (TCP peer address + the sender's local port) is wrong behind a NAT/port
      // translation; udpsrc attaches the source address to every buffer (GstNetAddressMeta). Only a
      // well-formed compound whose first packet is an SR/RR carrying the announced sender SSRC may move
      // the destination (anything else on this port is ignored). Done once per new endpoint.
      const uint32_t want = ssrc_.load(std::memory_order_relaxed);
      if (want == 0 || !IsValidRtcpCompoundFrom(b, want)) return;
      GstNetAddressMeta* am = gst_buffer_get_net_address_meta(b);
      if (!am || !G_IS_INET_SOCKET_ADDRESS(am->addr)) return;
      GInetSocketAddress* isa = G_INET_SOCKET_ADDRESS(am->addr);
      GInetAddress* ia = g_inet_socket_address_get_address(isa);
      if (g_inet_address_get_family(ia) != G_SOCKET_FAMILY_IPV4) return;
      const guint16 port = g_inet_socket_address_get_port(isa);
      uint32_t ip4; std::memcpy(&ip4, g_inet_address_to_bytes(ia), 4);       // no allocation: internal bytes
      const uint64_t id = (static_cast<uint64_t>(ip4) << 16) | port;
      if (rtcp_peer_id_.load(std::memory_order_acquire) == id) return;      // usual case: nothing to do
      rtcp_peer_id_.store(id, std::memory_order_release);
      gchar* host = g_inet_address_to_string(ia);                            // only when the endpoint changes
      g_object_set(rtcpsink_, "host", host, "port", (gint)port, nullptr);
      P5G_LOG_INFO << "rtcp reports -> " << host << ":" << port << " (learned from incoming RTCP)";
      g_free(host);
    });
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
      if (stream != stream_) {
        P5G_LOG_ERROR << "stream-start for a second stream '" << stream << "' on receiver " << g_cfg.receiver_id
                      << " (already serving '" << stream_ << "') -> refused";
        ctl_.Send({{"type", "stream-ack"}, {"stream", stream}, {"generation", m.value("generation", 0)}, {"ok", false},
                   {"reason", "receiver already serves " + stream_}});
      }
      return;
    }
    stream_ = stream;
    const std::string prefix = g_cfg.trace_dir + "/" + stream_ + "-rx";
    // Open the traces before the first packet arrives (the sender announces, then starts).
    rtp_ = std::make_unique<RtpPacketTrace>(prefix + "-rtp.csv", kRtpPacketHeader, &FormatRtpPacketRow, kPacketTraceCapacity);
    rtcp_ = std::make_unique<RtcpPacketTrace>(prefix + "-rtcp.csv", kRtcpPacketHeader, &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    decoded_ = std::make_unique<DecodedFrameLedgerTrace>(prefix + "-decoded.csv", kDecodedFrameLedgerHeader, &FormatDecodedFrameLedgerRow, kFrameTraceCapacity);
    frames_ = std::make_unique<DecodedFrameTrace>(prefix + "-frames.csv", kDecodedFrameHeader, &FormatDecodedFrameRow, kFrameTraceCapacity);
    if (g_cfg.stats_period_ms > 0) stats_.store(std::fopen((prefix + "-stats.jsonl").c_str(), "w"), std::memory_order_release);
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
                 << "; rtcp reports -> " << sender_host << ":" << sender_rtcp << " (until learned from incoming RTCP)";
    // Traces open and SSRC published: the sender may start (it waits for this before PLAYING).
    ctl_.Send({{"type", "stream-ack"}, {"stream", stream_}, {"generation", m.value("generation", 0)}, {"ok", true}});
  }
  void CloseTraces() {
    std::lock_guard<std::mutex> lk(mu_);
    if (rtp_) rtp_->Close();
    if (rtcp_) rtcp_->Close();
    if (decoded_) decoded_->Close();
    if (frames_) frames_->Close();
    if (std::FILE* f = stats_.exchange(nullptr)) std::fclose(f);
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
  std::atomic<uint64_t> rtcp_peer_id_{0};
  Arrival arrivals_[kArrivalRing];  // written by the udpsrc thread, read by the jitter-buffer thread
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
  std::atomic<std::FILE*> stats_{nullptr};  // written on the control thread, read on the main thread
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
  if (c.jitter_ms < 0 || c.jitter_ms > 800) P5G_FATAL("--jitter-ms must be 0..800 (per-frame arrival entries live ~1 s)");
  c.drop_late = a.GetInt("drop-late", 0);
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms);
  c.duration_s = a.GetInt("duration", 0);

  gst_init(nullptr, nullptr);
  p5g::InitFrameMetaCaps();
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
