// video_receiver (ffmpeg tree) — RTP/H.264 receiver: our UDP socket (kernel arrival timestamps), our RFC 6184
// depacketizer, libavcodec decode; per-packet / per-frame tracing.
//
// Based on: smec-project/edge-applications smec/video-od/server/main.cpp @ b66409c (docs/reference_code/
//           smec-edge-applications): FFmpeg RTP demux of the SDP the client wrote, one port per client, frames
//           handed to the analytics task, late frames dropped on the edge scheduler's signal.
// Local modifications: the RTP session is read from the socket by us (SO_TIMESTAMPNS kernel arrival per
//           packet) and depacketized by us (rtp_util.h) so that per-frame assembly (packets, first/last arrival,
//           lost fragments) is known exactly; decode with libavcodec (low delay, 1 thread); no analytics task
//           (the app "consumes" the frame the moment it is decoded); the control channel of the gstreamer tree
//           announces ports, SSRC and profile so no SDP file is needed. No RTCP is sent back (as in SMEC);
//           the sender's RTCP SR is logged.
//
// Traces (all in --trace-dir, opened when the sender announces the stream):
//   <stream>-rx-rtp.csv       every RTP packet in — log_*_ns = KERNEL arrival (SO_TIMESTAMPNS), see docs/TRACE_SCHEMA.md
//   <stream>-rx-rtcp.csv      every RTCP compound packet in (sender SR)
//   <stream>-rx-decoded.csv   every AU through the decoder (start / done)
//   <stream>-rx-frames.csv    every decoded frame reaching the app
//   <stream>-rx-stats.jsonl   counters every --stats-period-ms (packets, frames, incomplete AUs, lost fragments)
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/log.h>
}

#include "app_util.h"
#include "control_client.h"
#include "rtp_util.h"
#include "trace_ring.h"

namespace p5g {

struct ReceiverConfig {
  std::string control_host = "127.0.0.1";
  int control_port = 8765;
  std::string session = "s1";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  std::string advertise_host;
  int rtp_port = 0;             // 0 = ephemeral
  int stats_period_ms = 1000;
  int duration_s = 0;
};
static ReceiverConfig g_cfg;

// libavcodec's default log callback takes a mutex and writes to stderr — on the receive thread, and only when
// packets were damaged (rule 2: the measurement load must not depend on loss). Default here: count only, report
// from the main thread. P5G_AV_LOG=1 restores FFmpeg's callback for debugging.
static std::atomic<int64_t> g_av_log_errors{0};
static void CountingAvLog(void*, int level, const char*, va_list) { if (level <= AV_LOG_WARNING) g_av_log_errors.fetch_add(1, std::memory_order_relaxed); }

class Receiver {
 public:
  Receiver() { OpenSockets(); OpenDecoder(); }

  bool Run() {
    rx_thread_ = std::thread([this] { RxLoop(); });
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

  void Tick() {  // main thread: stats sample
    std::FILE* f = stats_.load(std::memory_order_acquire);
    if (!f) return;
    const int64_t now = NowMonoNs();
    if (next_stats_ == 0) next_stats_ = now;
    if (now < next_stats_) return;
    next_stats_ += (int64_t)g_cfg.stats_period_ms * 1000000; if (next_stats_ < now) next_stats_ = now;
    const int64_t late = depack_ ? depack_->counters().late_or_dup.load(std::memory_order_relaxed) : 0;
    const int64_t resets = depack_ ? depack_->counters().seq_resets.load(std::memory_order_relaxed) : 0;
    std::fprintf(f, "{\"mono_ns\":%lld,\"wall_ns\":%lld,\"packets\":%lld,\"bytes\":%lld,\"frames\":%lld,\"aus_incomplete\":%lld,\"aus_empty\":%lld,\"aus_damaged\":%lld,"
                    "\"lost_packets\":%lld,\"lost_fragments\":%lld,\"late_or_dup_packets\":%lld,\"seq_resets\":%lld,\"decode_failures\":%lld,\"av_log_errors\":%lld,\"decoder_held_buffers\":%lld}\n",
                 (long long)now, (long long)NowWallNs(), (long long)pkts_.load(), (long long)bytes_.load(), (long long)frame_idx_.load(),
                 (long long)incomplete_.load(), (long long)empty_aus_.load(), (long long)damaged_.load(), (long long)lost_pkts_.load(), (long long)lost_frag_.load(),
                 (long long)late, (long long)resets, (long long)decode_fail_.load(), (long long)g_av_log_errors.load(), (long long)buf_held_.load());
    std::fflush(f);
  }

  void Shutdown() {
    ctl_.Stop();
    running_ = false;
    if (rx_thread_.joinable()) rx_thread_.join();   // returns within the 100 ms socket timeout
    CloseTraces();
    if (const int64_t e = g_av_log_errors.load()) P5G_LOG_WARN << e << " libavcodec error/warning messages were suppressed on the receive thread (P5G_AV_LOG=1 prints them)";
    if (buf_held_.load()) P5G_LOG_WARN << buf_held_.load() << " AUs were still referenced by the decoder after avcodec_send_packet (unexpected; see rx-stats decoder_held_buffers)";
    P5G_LOG_INFO << "frames " << frame_idx_.load() << ", AUs incomplete " << incomplete_.load() << " (empty " << empty_aus_.load() << ", damaged " << damaged_.load()
                 << "), lost packets " << lost_pkts_.load() << ", lost fragments " << lost_frag_.load() << ", late/dup " << (depack_ ? depack_->counters().late_or_dup.load() : 0)
                 << ", decode failures " << decode_fail_.load();
    if (dec_) avcodec_free_context(&dec_);
    if (frame_) av_frame_free(&frame_);
    for (auto& p : pkt_) if (p) av_packet_free(&p);
    depack_.reset();
    for (auto& b : au_buf_) if (b) av_buffer_unref(&b);
    if (rtp_fd_ >= 0) ::close(rtp_fd_);
    if (rtcp_fd_ >= 0) ::close(rtcp_fd_);
  }

 private:
  void OpenSockets() {
    rtp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0); rtcp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_fd_ < 0 || rtcp_fd_ < 0) P5G_FATAL("socket: " << std::strerror(errno));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_cfg.rtp_port);
    if (::bind(rtp_fd_, (sockaddr*)&a, sizeof(a)) != 0) P5G_FATAL("bind rtp port " << g_cfg.rtp_port << ": " << std::strerror(errno));
    a.sin_port = 0;
    if (::bind(rtcp_fd_, (sockaddr*)&a, sizeof(a)) != 0) P5G_FATAL("bind rtcp: " << std::strerror(errno));
    sockaddr_in l{}; socklen_t sl = sizeof(l);
    ::getsockname(rtp_fd_, (sockaddr*)&l, &sl); rtp_port_ = ntohs(l.sin_port);
    ::getsockname(rtcp_fd_, (sockaddr*)&l, &sl); rtcp_port_ = ntohs(l.sin_port);
    const int one = 1;
    ::setsockopt(rtp_fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));   // kernel arrival timestamp per datagram
    const int rcvbuf = 8 << 20; ::setsockopt(rtp_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    timeval tv{0, 100000};
    ::setsockopt(rtp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(rtcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int flags = ::fcntl(rtcp_fd_, F_GETFL, 0); ::fcntl(rtcp_fd_, F_SETFL, flags | O_NONBLOCK);
  }
  void OpenDecoder() {
    const AVCodec* d = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!d) P5G_FATAL("libavcodec has no H.264 decoder");
    dec_ = avcodec_alloc_context3(d);
    dec_->thread_count = 1;                       // deterministic, no frame-threading delay
    dec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec_->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    if (avcodec_open2(dec_, d, nullptr) < 0) P5G_FATAL("h264 decoder open");
    frame_ = av_frame_alloc();
    // The depacketizer assembles straight into libavcodec-owned, padded, reference-counted buffers, and one
    // pre-built AVPacket per buffer holds a permanent reference: the decoder takes the AU by reference (no copy of
    // the AU bytes; a non-refcounted packet would be copied into a fresh allocation per AU — libavcodec/avpacket.c
    // av_packet_ref) and the application allocates nothing per AU.
    uint8_t* ptrs[H264Depacketizer::kBuffers];
    for (int k = 0; k < H264Depacketizer::kBuffers; ++k) {
      au_buf_[k] = av_buffer_alloc(kMaxAuBytes + AV_INPUT_BUFFER_PADDING_SIZE);
      if (!au_buf_[k]) P5G_FATAL("av_buffer_alloc");
      pkt_[k] = av_packet_alloc(); pkt_[k]->buf = av_buffer_ref(au_buf_[k]); pkt_[k]->data = au_buf_[k]->data; pkt_[k]->stream_index = 0;
      ptrs[k] = au_buf_[k]->data;
    }
    depack_ = std::make_unique<H264Depacketizer>(ptrs, kMaxAuBytes);
  }

  // Receive thread: one datagram per iteration; kernel arrival from SO_TIMESTAMPNS (CLOCK_REALTIME),
  // converted to CLOCK_MONOTONIC with the offset sampled at the read (both clocks read within ~50 ns).
  void RxLoop() {
    uint8_t buf[65536]; uint8_t cbuf[256];
    while (running_) {
      // RTCP first (non-blocking)
      for (;;) {
        const ssize_t n = ::recv(rtcp_fd_, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (auto* t = rtcp_p_.load(std::memory_order_acquire)) { if (IsRtcp(buf, (int)n)) { RtcpPacketRow r; FillRtcpRow(buf, (int)n, 1, NowMonoNs(), NowWallNs(), &r); t->Write(r); } }
      }
      iovec iov{buf, sizeof(buf)}; msghdr msg{}; msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
      const ssize_t n = ::recvmsg(rtp_fd_, &msg, 0);
      if (n <= 0) continue;
      const int64_t mono_now = NowMonoNs(), wall_now = NowWallNs();
      int64_t arrival_wall = wall_now;
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPNS) { timespec ts; std::memcpy(&ts, CMSG_DATA(c), sizeof(ts)); arrival_wall = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec; }
      const int64_t arrival_mono = mono_now - (wall_now - arrival_wall);
      RtpHeader h;
      if (!ParseRtp(buf, (int)n, &h)) continue;
      auto* rt = rtp_p_.load(std::memory_order_acquire);
      if (rt) { RtpPacketRow r; FillRtpRow(buf, (int)n, h, 1, arrival_mono, arrival_wall, &r); rt->Write(r); }
      if (h.ssrc != ssrc_.load(std::memory_order_relaxed)) continue;   // not our announced stream
      pkts_++; bytes_ += n;
      if (h.pt != pt_.load(std::memory_order_relaxed)) continue;
      H264Depacketizer::AuEvent ev[2];
      const int n_ev = depack_->Push(h, ev);      // 0..2 AUs ended with this packet (each in its own buffer)
      if (!depack_->last_accepted()) continue;    // duplicate / late packet: counted by the depacketizer, not part of any AU
      // per-timestamp arrival bookkeeping of ACCEPTED packets: an AU closed by the marker is `cur`, one closed by a new
      // timestamp is `prev` (the events of this push refer to one of the two by rtp_ts)
      if (!have_cur_ || h.ts != cur_.ts) { prev_ = cur_; cur_ = {h.ts, arrival_mono, arrival_mono, 1}; have_cur_ = true; }
      else { cur_.last = arrival_mono; cur_.n++; }
      for (int k = 0; k < n_ev; ++k) {
        const Arr& a = (ev[k].rtp_ts == cur_.ts) ? cur_ : prev_;
        DecodeAu(ev[k], a.first, a.last, a.n);
      }
    }
  }

  void DecodeAu(const H264Depacketizer::AuEvent& ev, int64_t first, int64_t last, int npk) {
    if (!ev.complete) { incomplete_++; lost_frag_ += ev.lost_fragments; lost_pkts_ += ev.lost_packets; damaged_ += ev.damaged; }
    if (ev.bytes == 0) { empty_aus_++; return; }   // every byte of this AU was lost or discarded: counted, nothing to decode
    const int64_t t0 = NowMonoNs();
    // hand the AU to the decoder by reference: pkt_[k] permanently owns a ref of buffer k (set up once), so this
    // side allocates nothing per AU. Inside libavcodec, av_packet_ref() wraps our buffer in a new AVBufferRef
    // (av_buffer_ref -> av_mallocz of the wrapper, libavutil/buffer.c) and the decoder allocates RBSP/frame-pool
    // memory as it always does — per AU, independent of loss, and not avoidable with the stock decoder.
    AVPacket* pkt = pkt_[ev.buffer];
    std::memset(pkt->data + ev.bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);   // input-buffer contract: zeroed padding after the AU
    pkt->size = ev.bytes; pkt->pts = ev.rtp_ts; pkt->dts = ev.rtp_ts;
    pkt->flags = ev.is_idr ? AV_PKT_FLAG_KEY : 0;
    int ret = avcodec_send_packet(dec_, pkt);
    // the h264 decoder copies NALs into its own RBSP buffers and releases the packet before returning; if it ever
    // held on to ours the buffer would be overwritten two AUs later — count it so that a run can be judged
    if (av_buffer_get_ref_count(au_buf_[ev.buffer]) != 2) buf_held_++;   // 2 = ours + pkt_[k]'s permanent ref
    if (ret < 0) { decode_fail_++; return; }
    while ((ret = avcodec_receive_frame(dec_, frame_)) == 0) {
      const int64_t t1 = NowMonoNs(), w1 = NowWallNs();
      const uint32_t ts = (uint32_t)frame_->pts;                   // == the AU's rtp_ts (pts passed through)
      if (auto* t = decoded_p_.load(std::memory_order_acquire))
        t->Write(DecodedFrameLedgerRow{ts, t0, t1, w1, ev.bytes, ev.is_idr ? 3 : 4, -1, -1, -1, frame_->width, frame_->height});
      if (auto* t = frames_p_.load(std::memory_order_acquire))
        t->Write(DecodedFrameRow{frame_idx_++, ts, -1, (int64_t)ts, 0, w1, t1, frame_->width, frame_->height, npk, first, last, ssrc_.load()});
      av_frame_unref(frame_);
    }
  }

  void OnMessage(const json& m) {
    if (m.value("type", "") != "stream-start") return;
    std::lock_guard<std::mutex> lk(mu_);
    const std::string stream = m.value("stream", "");
    if (!stream_.empty()) {
      if (stream != stream_) {
        P5G_LOG_ERROR << "stream-start for a second stream '" << stream << "' on receiver " << g_cfg.receiver_id << " (serving '" << stream_ << "') -> refused";
        ctl_.Send({{"type", "stream-ack"}, {"stream", stream}, {"generation", m.value("generation", 0)}, {"ok", false}, {"reason", "receiver already serves " + stream_}});
      }
      return;
    }
    stream_ = stream;
    const std::string prefix = g_cfg.trace_dir + "/" + stream_ + "-rx";
    rtp_ = std::make_unique<RtpPacketTrace>(prefix + "-rtp.csv", kRtpPacketHeader, &FormatRtpPacketRow, kPacketTraceCapacity);
    rtcp_ = std::make_unique<RtcpPacketTrace>(prefix + "-rtcp.csv", kRtcpPacketHeader, &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    decoded_ = std::make_unique<DecodedFrameLedgerTrace>(prefix + "-decoded.csv", kDecodedFrameLedgerHeader, &FormatDecodedFrameLedgerRow, kFrameTraceCapacity);
    frames_ = std::make_unique<DecodedFrameTrace>(prefix + "-frames.csv", kDecodedFrameHeader, &FormatDecodedFrameRow, kFrameTraceCapacity);
    if (g_cfg.stats_period_ms > 0) stats_.store(std::fopen((prefix + "-stats.jsonl").c_str(), "w"), std::memory_order_release);
    ssrc_.store(m.value("ssrc", 0u), std::memory_order_relaxed);
    pt_.store((uint8_t)m.value("pt", 96), std::memory_order_relaxed);
    rtp_p_.store(rtp_.get(), std::memory_order_release); rtcp_p_.store(rtcp_.get(), std::memory_order_release);
    decoded_p_.store(decoded_.get(), std::memory_order_release); frames_p_.store(frames_.get(), std::memory_order_release);
    P5G_LOG_INFO << "stream " << stream_ << " announced: ssrc=" << ssrc_.load() << " pt=" << (int)pt_.load() << " " << m.value("width", 0) << "x"
                 << m.value("height", 0) << "@" << m.value("fps", 0) << " " << m.value("bitrate_kbps", 0) << " kbps rungs=" << m.value("rungs_kbps", json::array()).dump()
                 << " aus=" << m.value("aus", 0) << " sender=" << m.value("sender_host", "?") << ":" << m.value("rtcp_port_local", 0);
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

  int rtp_fd_ = -1, rtcp_fd_ = -1, rtp_port_ = 0, rtcp_port_ = 0;
  AVCodecContext* dec_ = nullptr; AVFrame* frame_ = nullptr;
  AVPacket* pkt_[H264Depacketizer::kBuffers] = {nullptr, nullptr, nullptr};   // one per assembly buffer, permanent ref
  static constexpr int kMaxAuBytes = 4 << 20;   // one access unit (an 8 Mbps IDR is ~100 KB; generous)
  AVBufferRef* au_buf_[H264Depacketizer::kBuffers] = {nullptr, nullptr, nullptr};
  std::unique_ptr<H264Depacketizer> depack_;
  struct Arr { uint32_t ts = 0; int64_t first = 0, last = 0; int n = 0; };
  Arr cur_, prev_; bool have_cur_ = false;   // receive thread only
  std::atomic<uint32_t> ssrc_{0}; std::atomic<uint8_t> pt_{96};
  std::atomic<int64_t> pkts_{0}, bytes_{0}, frame_idx_{0}, incomplete_{0}, empty_aus_{0}, damaged_{0}, lost_pkts_{0}, lost_frag_{0}, decode_fail_{0}, buf_held_{0};
  std::atomic<RtpPacketTrace*> rtp_p_{nullptr}; std::atomic<RtcpPacketTrace*> rtcp_p_{nullptr};
  std::atomic<DecodedFrameLedgerTrace*> decoded_p_{nullptr}; std::atomic<DecodedFrameTrace*> frames_p_{nullptr};
  std::unique_ptr<RtpPacketTrace> rtp_; std::unique_ptr<RtcpPacketTrace> rtcp_;
  std::unique_ptr<DecodedFrameLedgerTrace> decoded_; std::unique_ptr<DecodedFrameTrace> frames_;
  std::atomic<std::FILE*> stats_{nullptr}; int64_t next_stats_ = 0;
  std::string stream_;
  std::thread rx_thread_; std::atomic<bool> running_{true};
  std::mutex mu_; ControlClient ctl_;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr, "video_receiver --control-host H --control-port P --session S --receiver-id RID --trace-dir DIR\n"
                       "               [--rtp-port N(=ephemeral)] [--advertise-host A] [--stats-period-ms 1000] [--duration S]\n");
}

int main(int argc, char** argv) {
  p5g::CliArgs a(argc, argv, {"help", "control-host", "control-port", "session", "receiver-id", "trace-dir", "rtp-port", "advertise-host",
                              "stats-period-ms", "duration"});
  if (a.Has("help")) { Usage(); return 0; }
  auto& c = p5g::g_cfg;
  c.control_host = a.Get("control-host", c.control_host); c.control_port = a.GetInt("control-port", c.control_port);
  c.session = a.Get("session", c.session); c.receiver_id = a.Get("receiver-id", c.receiver_id); c.trace_dir = a.Get("trace-dir", c.trace_dir);
  c.advertise_host = a.Get("advertise-host", ""); c.rtp_port = a.GetInt("rtp-port", 0);
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms); c.duration_s = a.GetInt("duration", 0);
  av_log_set_level(AV_LOG_ERROR);
  if (!std::getenv("P5G_AV_LOG")) av_log_set_callback(&p5g::CountingAvLog);
  P5G_LOG_INFO << "config: transport=ffmpeg receiver_id=" << c.receiver_id << " session=" << c.session << " control=" << c.control_host << ":"
               << c.control_port << " rtp_port=" << c.rtp_port << " stats_period_ms=" << c.stats_period_ms
               << " libavcodec=" << LIBAVCODEC_VERSION_MAJOR << "." << LIBAVCODEC_VERSION_MINOR << "." << LIBAVCODEC_VERSION_MICRO;
  p5g::InstallSignalHandlers();
  {
    p5g::Receiver receiver;
    if (!receiver.Run()) return 1;
    p5g::RunUntilShutdown(c.duration_s, [&] { receiver.Tick(); }, 100);
    receiver.Shutdown();
  }
  return 0;
}
