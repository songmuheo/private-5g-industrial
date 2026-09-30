// video_sender (ffmpeg tree) — SMEC-style sender: a PRE-ENCODED H.264 file (one or more bitrate rungs) is sent
// as RTP over UDP on a fixed capture grid, with per-frame / per-packet tracing.
//
// Based on: smec-project/edge-applications smec/video-od/client/src/streamer.cpp @ b66409c
//           (docs/reference_code/smec-edge-applications): libavformat "rtp" muxer fed with the packets of a
//           pre-encoded file, paced by the application (av_usleep until the packet's PTS), no encoder, no
//           congestion control, no pacer, no RTCP handling.
// Local modifications: absolute-time capture grid (clock_nanosleep) instead of PTS sleeps; the muxer writes
//           into our AVIO callback so every RTP/RTCP packet is logged and sent by us (sockets we own, so the
//           receiver's RTCP can come back); several rungs of the same content (identical frame count and IDR
//           positions) switched at IDR boundaries on an edge `profile` message; no SEI metadata (frame
//           identity = RTP timestamp, which the sender learns from the muxer's first packet); the control
//           channel of the gstreamer tree (ports, stream-start/ack, profile).
//
// What is fixed and how: fps = the grid; resolution / GOP / bytes per frame = the file (bit-identical across
// runs and cameras); bitrate = the rung. Nothing in this process adapts to the network.
//
// Traces (all in --trace-dir; same names/columns as the other trees):
//   <stream>-tx-frames.csv          every capture slot (to_encoder=0 for a slot missed behind a blocked send)
//   <stream>-tx-encoded.csv         every AU handed to the muxer (bytes, IDR, resolution)
//   <stream>-tx-encoder-rates.csv   the rung in use (start + every switch)
//   <stream>-tx-rtp.csv             every RTP packet out (the muxer's packets, at our socket send)
//   <stream>-tx-rtcp.csv            RTCP SR out (from the muxer) / RTCP in from the receiver
//   <stream>-tx-stats.jsonl         counters every --stats-period-ms
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/opt.h>
}

#include "app_util.h"
#include "control_client.h"
#include "rtp_util.h"
#include "trace_ring.h"

namespace p5g {

struct Rung { std::string path; int kbps = 0; const uint8_t* map = nullptr; size_t len = 0; std::vector<AccessUnit> aus; };

struct SenderConfig {
  std::string control_host = "127.0.0.1";
  int control_port = 8765;
  std::string session = "s1";
  std::string stream_id = "cam0";
  std::string receiver_id = "recv0";
  std::string trace_dir = ".";
  std::vector<Rung> rungs;      // --source a.h264@2500[,b.h264@1000...]
  int start_kbps = 0;           // --bitrate-kbps: rung to start with (0 = first)
  int fps = 30;
  int mtu = 1200;
  int pt = 96;
  uint32_t ssrc = 0;            // 0 = random
  int stats_period_ms = 1000;
  int duration_s = 0;
  int64_t start_wall_ns = 0;    // --start-at-epoch: wall-clock epoch of the capture grid (0 = now); slots are T + k/fps on every host
};
static SenderConfig g_cfg;
std::string TracePrefix() { return g_cfg.trace_dir + "/" + g_cfg.stream_id + "-tx"; }

class Sender {
 public:
  Sender() {
    frames_ = std::make_unique<CaptureFrameTrace>(TracePrefix() + "-frames.csv", kCaptureFrameHeader, &FormatCaptureFrameRow, kFrameTraceCapacity);
    encoded_ = std::make_unique<EncodedFrameTrace>(TracePrefix() + "-encoded.csv", kEncodedFrameHeader, &FormatEncodedFrameRow, kFrameTraceCapacity);
    rates_ = std::make_unique<EncoderRateTrace>(TracePrefix() + "-encoder-rates.csv", kEncoderRateHeader, &FormatEncoderRateRow, kFrameTraceCapacity);
    rtp_ = std::make_unique<RtpPacketTrace>(TracePrefix() + "-rtp.csv", kRtpPacketHeader, &FormatRtpPacketRow, kPacketTraceCapacity);
    rtcp_ = std::make_unique<RtcpPacketTrace>(TracePrefix() + "-rtcp.csv", kRtcpPacketHeader, &FormatRtcpPacketRow, kPacketTraceCapacity / 8);
    stats_ = g_cfg.stats_period_ms > 0 ? std::fopen((TracePrefix() + "-stats.jsonl").c_str(), "w") : nullptr;
    LoadRungs();
    OpenSockets();
    OpenMuxer();
  }

  bool Run() {
    if (!ctl_.Connect(g_cfg.control_host, g_cfg.control_port)) {
      P5G_LOG_ERROR << "control connect failed (" << g_cfg.control_host << ":" << g_cfg.control_port << ")";
      return false;
    }
    ctl_.Start([this](const json& m) { OnMessage(m); });
    rtcp_rx_thread_ = std::thread([this] { RtcpRxLoop(); });
    return ctl_.Send({{"type", "register"}, {"role", "sender"}, {"session", g_cfg.session}, {"stream", g_cfg.stream_id}, {"to", g_cfg.receiver_id}});
  }

  // main thread tick: ack watchdog + stats sample
  void Tick() {
    if (!started_) {
      const int64_t dl = ack_deadline_.load();
      if (dl && NowMonoNs() > dl) P5G_FATAL("no stream-ack from receiver " << g_cfg.receiver_id << " within 5 s");
      return;
    }
    if (!stats_) return;
    const int64_t now = NowMonoNs();
    if (next_stats_ == 0) next_stats_ = now;
    if (now < next_stats_) return;
    next_stats_ += (int64_t)g_cfg.stats_period_ms * 1000000; if (next_stats_ < now) next_stats_ = now;
    std::fprintf(stats_, "{\"mono_ns\":%lld,\"wall_ns\":%lld,\"frames_sent\":%lld,\"missed_slots\":%lld,\"packets_sent\":%lld,\"bytes_sent\":%lld,\"send_failures\":%lld,\"rtcp_send_failures\":%lld,\"rung_kbps\":%d}\n",
                 (long long)now, (long long)NowWallNs(), (long long)frames_sent_.load(), (long long)missed_.load(),
                 (long long)pkts_sent_.load(), (long long)bytes_sent_.load(), (long long)send_fail_.load(), (long long)rtcp_send_fail_.load(), g_cfg.rungs[cur_rung_.load()].kbps);
    std::fflush(stats_);
    // send failures are counted on the grid thread (no I/O there); reported here as they happen
    const int64_t sf = send_fail_.load();
    if (sf != send_fail_reported_) { P5G_LOG_ERROR << "sendto failed for " << (sf - send_fail_reported_) << " RTP packet(s) (total " << sf << ", last errno " << last_send_errno_.load() << " " << std::strerror(last_send_errno_.load()) << "): local failure, not path loss"; send_fail_reported_ = sf; }
  }

  // Teardown: control -> grid stops (its last send is synchronous, nothing is queued anywhere) -> join ->
  // muxer trailer (RTCP BYE) -> traces.
  void Shutdown() {
    ctl_.Stop();
    running_ = false;
    if (grid_.joinable()) grid_.join();
    rtcp_rx_running_ = false;
    if (rtcp_rx_thread_.joinable()) rtcp_rx_thread_.join();
    if (missed_.load()) P5G_LOG_WARN << missed_.load() << " capture slots missed behind a blocked send (to_encoder=0 rows)";
    if (send_fail_.load()) P5G_LOG_ERROR << send_fail_.load() << " RTP sendto failures (tx-stats send_failures): those packets never left this host";
    P5G_LOG_INFO << "sent " << frames_sent_.load() << " frames, " << pkts_sent_.load() << " packets, " << bytes_sent_.load() << " bytes";
    if (oc_) { if (header_written_) av_write_trailer(oc_); avio_context_free(&oc_->pb); avformat_free_context(oc_); oc_ = nullptr; }
    if (frames_) frames_->Close();
    if (encoded_) encoded_->Close();
    if (rates_) rates_->Close();
    if (rtp_) rtp_->Close();
    if (rtcp_) rtcp_->Close();
    if (stats_) std::fclose(stats_);
    stats_ = nullptr;
    if (rtp_fd_ >= 0) ::close(rtp_fd_);
    if (rtcp_fd_ >= 0) ::close(rtcp_fd_);
  }

 private:
  // ---- files -------------------------------------------------------------------------------------
  void LoadRungs() {
    if (g_cfg.rungs.empty()) P5G_FATAL("--source a.h264@kbps[,b.h264@kbps] required");
    for (auto& r : g_cfg.rungs) {
      const int fd = ::open(r.path.c_str(), O_RDONLY);
      if (fd < 0) P5G_FATAL("cannot open " << r.path << ": " << std::strerror(errno));
      struct stat st {};
      if (::fstat(fd, &st) != 0 || st.st_size <= 0) P5G_FATAL("fstat " << r.path);
      r.len = (size_t)st.st_size;
      void* m = ::mmap(nullptr, r.len, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
      ::close(fd);
      if (m == MAP_FAILED) P5G_FATAL("mmap " << r.path);
      r.map = static_cast<const uint8_t*>(m);
      std::string err;
      r.aus = IndexAccessUnits(r.map, (int64_t)r.len, &err);
      if (r.aus.empty()) P5G_FATAL(r.path << ": " << err);
      if (!r.aus[0].is_idr) P5G_FATAL(r.path << ": the first access unit is not an IDR");
    }
    // Rungs must be frame-aligned (same count, same IDR positions) so a switch at an IDR is seamless.
    const auto& a0 = g_cfg.rungs[0].aus;
    for (size_t k = 1; k < g_cfg.rungs.size(); ++k) {
      const auto& ak = g_cfg.rungs[k].aus;
      if (ak.size() != a0.size()) P5G_FATAL("rung " << g_cfg.rungs[k].path << " has " << ak.size() << " AUs, rung 0 has " << a0.size());
      for (size_t i = 0; i < a0.size(); ++i)
        if (ak[i].is_idr != a0[i].is_idr) P5G_FATAL("rung " << g_cfg.rungs[k].path << ": IDR positions differ from rung 0 at AU " << i);
    }
    int idx = 0;
    if (g_cfg.start_kbps > 0) {
      idx = -1;
      for (size_t k = 0; k < g_cfg.rungs.size(); ++k) if (g_cfg.rungs[k].kbps == g_cfg.start_kbps) idx = (int)k;
      if (idx < 0) P5G_FATAL("--bitrate-kbps " << g_cfg.start_kbps << " is not one of the rungs");
    }
    cur_rung_ = idx; want_rung_ = idx;
    ProbeResolution(g_cfg.rungs[0].aus[0]);
    int64_t idr = 0; for (auto& au : a0) idr += au.is_idr;
    P5G_LOG_INFO << "source: " << g_cfg.rungs.size() << " rung(s), " << a0.size() << " AUs (" << idr << " IDR), " << width_ << "x" << height_
                 << ", start rung " << g_cfg.rungs[idx].kbps << " kbps; bytes/rung: " << RungBytes();
  }
  std::string RungBytes() { std::string s; for (auto& r : g_cfg.rungs) s += std::to_string(r.kbps) + "k=" + std::to_string(r.len / 1000) + "KB "; return s; }
  // Decode the first (IDR) AU once to learn the coded resolution (start-up only).
  void ProbeResolution(const AccessUnit& au) {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (avcodec_open2(c, dec, nullptr) < 0) P5G_FATAL("h264 decoder open");
    AVPacket* pkt = av_packet_alloc(); pkt->data = const_cast<uint8_t*>(au.data); pkt->size = au.bytes;
    AVFrame* fr = av_frame_alloc();
    if (avcodec_send_packet(c, pkt) == 0 && avcodec_receive_frame(c, fr) == 0) { width_ = fr->width; height_ = fr->height; }
    else { avcodec_send_packet(c, nullptr); if (avcodec_receive_frame(c, fr) == 0) { width_ = fr->width; height_ = fr->height; } }
    if (!width_) P5G_FATAL("could not decode the first access unit to learn the resolution");
    av_frame_free(&fr); pkt->data = nullptr; pkt->size = 0; av_packet_free(&pkt); avcodec_free_context(&c);
  }

  // ---- sockets -----------------------------------------------------------------------------------
  void OpenSockets() {
    rtp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    rtcp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_fd_ < 0 || rtcp_fd_ < 0) P5G_FATAL("socket: " << std::strerror(errno));
    sockaddr_in any{}; any.sin_family = AF_INET; any.sin_addr.s_addr = htonl(INADDR_ANY); any.sin_port = 0;
    if (::bind(rtp_fd_, (sockaddr*)&any, sizeof(any)) != 0 || ::bind(rtcp_fd_, (sockaddr*)&any, sizeof(any)) != 0) P5G_FATAL("bind: " << std::strerror(errno));
    sockaddr_in local{}; socklen_t sl = sizeof(local);
    ::getsockname(rtcp_fd_, (sockaddr*)&local, &sl); rtcp_port_local_ = ntohs(local.sin_port);
    ::getsockname(rtp_fd_, (sockaddr*)&local, &sl); rtp_port_local_ = ntohs(local.sin_port);
    const int sndbuf = 4 << 20; ::setsockopt(rtp_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    timeval tv{0, 100000}; ::setsockopt(rtcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  // ---- muxer -------------------------------------------------------------------------------------
  // libavformat "rtp" muxer (rtpenc, RFC 6184 packetization: single NAL / STAP-A / FU-A at packet_size),
  // writing into our AVIO callback: rtpenc flushes after every packet, so each callback is one packet.
  void OpenMuxer() {
    if (avformat_alloc_output_context2(&oc_, nullptr, "rtp", nullptr) < 0 || !oc_) P5G_FATAL("rtp muxer alloc");
    AVStream* st = avformat_new_stream(oc_, nullptr);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->width = width_; st->codecpar->height = height_;
    st->time_base = AVRational{1, (int)kVideoClockRate};
    oc_->packet_size = g_cfg.mtu;
    oc_->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int bufsz = 65536;
    uint8_t* buf = static_cast<uint8_t*>(av_malloc(bufsz));
    oc_->pb = avio_alloc_context(buf, bufsz, 1, this, nullptr, &Sender::WritePacket, nullptr);
    if (!g_cfg.ssrc) { std::srand((unsigned)NowMonoNs()); g_cfg.ssrc = ((uint32_t)std::rand() << 16) ^ (uint32_t)std::rand(); if (!g_cfg.ssrc) g_cfg.ssrc = 1; }
    AVDictionary* opts = nullptr;
    av_dict_set_int(&opts, "ssrc", (int64_t)(int32_t)g_cfg.ssrc, 0);
    av_dict_set_int(&opts, "payload_type", g_cfg.pt, 0);
    av_dict_set_int(&opts, "seq", 0, 0);
    av_dict_free(&mux_opts_); mux_opts_ = opts;   // header is written when the grid starts (WriteHeader)
  }
  // rtpenc fixes the RTCP SR epoch (first_rtcp_ntp_time) when the header is written, from start_time_realtime
  // if set, and maps SR rtp_ts = base + (ntp_now - epoch): media pts 0 must coincide with that epoch or every
  // SR carries an rtp_ts offset equal to the handshake delay (libavformat/rtpenc.c n4.4.2 rtp_write_header,
  // rtcp_send_sr; RFC 3550 §6.4.1). So: header written from the grid thread once the grid epoch is known.
  void WriteHeader(int64_t epoch_wall_ns) {
    oc_->start_time_realtime = epoch_wall_ns / 1000;   // µs since the Unix epoch
    if (avformat_write_header(oc_, &mux_opts_) < 0) P5G_FATAL("rtp muxer write_header");
    av_dict_free(&mux_opts_);
    header_written_ = true;
  }
  // sendto with EINTR retry; false = the kernel refused the datagram (EMSGSIZE, ENETUNREACH, EAGAIN on a full
  // SNDBUF, ...). Such a packet never left the host and must not appear in the tx-rtp trace as sent.
  static bool SendAll(int fd, const uint8_t* buf, int size, const sockaddr_in& to, std::atomic<int>* err) {
    for (;;) {
      const ssize_t n = ::sendto(fd, buf, size, 0, (const sockaddr*)&to, sizeof(to));
      if (n == size) return true;
      if (n < 0 && errno == EINTR) continue;
      err->store(errno, std::memory_order_relaxed);
      return false;
    }
  }
  // One RTP or RTCP packet from the muxer (grid thread, synchronous): send it on our socket, then log it. The
  // muxer always gets `size` back: a socket failure is counted (tx-stats send_failures, ERROR from the main
  // thread), not turned into a muxer error that would desynchronise rtpenc's sequence numbers from the wire.
  static int WritePacket(void* opaque, uint8_t* buf, int size) {
    auto* self = static_cast<Sender*>(opaque);
    const int64_t mono = NowMonoNs(), wall = NowWallNs();
    if (IsRtcp(buf, size)) {
      if (!self->dest_set_ || !SendAll(self->rtcp_fd_, buf, size, self->rtcp_dest_, &self->last_send_errno_)) { self->rtcp_send_fail_++; return size; }
      RtcpPacketRow r; FillRtcpRow(buf, size, 0, mono, wall, &r); self->rtcp_->Write(r);
      return size;
    }
    RtpHeader h;
    if (!ParseRtp(buf, size, &h)) { self->send_fail_++; return size; }
    if (!self->base_known_) { self->wire_base_ = h.ts - (uint32_t)self->cur_pts_; self->base_known_ = true; }
    if (!SendAll(self->rtp_fd_, buf, size, self->rtp_dest_, &self->last_send_errno_)) { self->send_fail_++; self->au_send_failed_ = true; return size; }
    RtpPacketRow r; FillRtpRow(buf, size, h, 0, mono, wall, &r); self->rtp_->Write(r);   // log_*_ns = just before the send
    if (h.marker) self->last_sent_ts_.store(h.ts, std::memory_order_relaxed);
    self->pkts_sent_++; self->bytes_sent_ += size;
    return size;
  }

  // ---- grid --------------------------------------------------------------------------------------
  static void SleepUntilMonoUs(int64_t t) { timespec ts{(time_t)(t / 1000000), (long)((t % 1000000) * 1000)}; while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {} }
  // The capture grid is anchored to a WALL-CLOCK epoch T (--start-at-epoch, default: now): slot k is at
  // T + k/fps on every host, so senders on different laptops (chrony, ~50 µs) capture in phase and their IDRs
  // coincide; pts 0 = T. T is converted to this host's monotonic clock once, here. A late start (T already
  // past) begins at the next slot on the same grid and says so; nothing before the start is a "missed" slot.
  void GridLoop() {
    const int64_t interval_us = 1000000 / g_cfg.fps;
    const int64_t wall_now = NowWallNs(), mono_now = NowMonoNs();
    const int64_t epoch_wall = g_cfg.start_wall_ns ? g_cfg.start_wall_ns : wall_now;
    start_us_ = (mono_now - (wall_now - epoch_wall)) / 1000;              // epoch in CLOCK_MONOTONIC µs
    int64_t slot = 0; int64_t idx = 0;
    if (mono_now / 1000 > start_us_) {
      slot = (mono_now / 1000 - start_us_) / interval_us + 1;
      if (g_cfg.start_wall_ns) P5G_LOG_WARN << "capture epoch was " << (mono_now / 1000 - start_us_) / 1000 << " ms ago; starting at slot " << slot;
    }
    WriteHeader(epoch_wall);
    P5G_LOG_INFO << "capture grid: epoch wall_ns=" << epoch_wall << " (" << (g_cfg.start_wall_ns ? "--start-at-epoch" : "now") << "), first slot " << slot
                 << " in " << (start_us_ + slot * interval_us - mono_now / 1000) / 1000 << " ms";
    rates_->Write(EncoderRateRow{NowMonoNs(), NowWallNs(), (int64_t)g_cfg.rungs[cur_rung_].kbps * 1000, (int64_t)g_cfg.rungs[cur_rung_].kbps * 1000, -1, (double)g_cfg.fps, 1});
    while (running_) {
      int64_t target_us = start_us_ + slot * interval_us;
      SleepUntilMonoUs(target_us);
      const int64_t now_us = NowMonoNs() / 1000;
      if (now_us >= start_us_ + (slot + 1) * interval_us) {  // fell behind (a send blocked): record the missed slots
        const int64_t resume = (now_us - start_us_) / interval_us;
        for (; slot < resume && running_; ++slot) EmitMissed(slot, idx, interval_us);
        target_us = start_us_ + slot * interval_us;
      }
      EmitSlot(slot, idx, target_us, interval_us);
      ++slot;
    }
    const int64_t now_us = NowMonoNs() / 1000;
    for (; start_us_ + (slot + 1) * interval_us <= now_us; ++slot) EmitMissed(slot, idx, interval_us);
  }
  uint32_t RtpTsForPts(int64_t pts) const { return (uint32_t)pts + wire_base_; }
  void EmitMissed(int64_t slot, int64_t& idx, int64_t interval_us) {
    const int64_t pts = slot * interval_us * 90 / 1000;
    frames_->Write(CaptureFrameRow{idx, slot, idx % (int64_t)g_cfg.rungs[0].aus.size(), RtpTsForPts(pts), NowWallNs(), NowMonoNs(), width_, height_, 0});
    ++idx; missed_++;
  }
  void EmitSlot(int64_t slot, int64_t& idx, int64_t target_us, int64_t interval_us) {
    const int64_t capture_wall = NowWallNs(), capture_mono = NowMonoNs();
    const int64_t n = (int64_t)g_cfg.rungs[0].aus.size();
    const int64_t src = idx % n;
    // rung switch only at an IDR (all rungs share IDR positions), so the decoder never sees a reference gap
    const int want = want_rung_.load(std::memory_order_relaxed);
    if (want != cur_rung_.load() && g_cfg.rungs[0].aus[src].is_idr) {
      cur_rung_.store(want);
      rates_->Write(EncoderRateRow{NowMonoNs(), NowWallNs(), (int64_t)g_cfg.rungs[want].kbps * 1000, (int64_t)g_cfg.rungs[want].kbps * 1000, -1, (double)g_cfg.fps, 1});
    }
    const AccessUnit& au = g_cfg.rungs[cur_rung_.load()].aus[src];
    const int64_t pts = (target_us - start_us_) * 90 / 1000;
    cur_pts_ = pts; au_send_failed_ = false;
    AVPacket pkt; av_init_packet(&pkt);
    pkt.data = const_cast<uint8_t*>(au.data); pkt.size = au.bytes; pkt.pts = pts; pkt.dts = pts; pkt.stream_index = 0;
    pkt.flags = au.is_idr ? AV_PKT_FLAG_KEY : 0;
    const int ret = av_write_frame(oc_, &pkt);        // synchronous: every RTP packet of this AU went through WritePacket
    const bool ok = ret >= 0 && !au_send_failed_;     // false: the muxer failed or at least one packet was refused by the socket
    const uint32_t rtp_ts = RtpTsForPts(pts);           // wire value (base learned from the first packet)
    frames_->Write(CaptureFrameRow{idx, slot, src, rtp_ts, capture_wall, capture_mono, width_, height_, ok ? 1 : 0});
    if (ok) {
      encoded_->Write(EncodedFrameRow{rtp_ts, capture_mono, capture_wall, au.bytes, width_, height_, au.is_idr ? 3 : 4, -1, -1, -1, -1,
                                      pts / 90, -1, 4, au.is_idr, -1});
      frames_sent_++;
    }
    ++idx;
  }

  // ---- RTCP from the receiver (RR, if it sends any) --------------------------------------------------
  void RtcpRxLoop() {
    uint8_t buf[2048];
    while (rtcp_rx_running_) {
      sockaddr_in from{}; socklen_t fl = sizeof(from);
      const ssize_t n = ::recvfrom(rtcp_fd_, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
      if (n <= 0) continue;
      if (!IsRtcp(buf, (int)n)) continue;
      RtcpPacketRow r; FillRtcpRow(buf, (int)n, 1, NowMonoNs(), NowWallNs(), &r); rtcp_->Write(r);
    }
  }

  // ---- control (its own thread) ----------------------------------------------------------------------
  void OnMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "receiver-ready") {
      if (m.value("receiver", "") != g_cfg.receiver_id) return;
      std::lock_guard<std::mutex> lk(mu_);
      if (started_ || awaiting_ack_) return;
      const std::string host = m.value("host", g_cfg.control_host);
      rtp_dest_ = {}; rtp_dest_.sin_family = AF_INET; rtp_dest_.sin_port = htons((uint16_t)m.at("rtp_port").get<int>());
      rtcp_dest_ = {}; rtcp_dest_.sin_family = AF_INET; rtcp_dest_.sin_port = htons((uint16_t)m.at("rtcp_port").get<int>());
      if (::inet_pton(AF_INET, host.c_str(), &rtp_dest_.sin_addr) != 1) P5G_FATAL("bad receiver host " << host);
      rtcp_dest_.sin_addr = rtp_dest_.sin_addr; dest_set_ = true; dest_host_ = host;
      json rungs = json::array(); for (auto& r : g_cfg.rungs) rungs.push_back(r.kbps);
      ctl_.Send({{"type", "stream-start"}, {"to", g_cfg.receiver_id}, {"stream", g_cfg.stream_id}, {"ssrc", g_cfg.ssrc}, {"pt", g_cfg.pt},
                 {"clock_rate", kVideoClockRate}, {"rtcp_port_local", rtcp_port_local_}, {"width", width_}, {"height", height_},
                 {"fps", g_cfg.fps}, {"bitrate_kbps", g_cfg.rungs[cur_rung_].kbps}, {"rungs_kbps", rungs}, {"cc", "profile"},
                 {"transport", "ffmpeg"}, {"source", g_cfg.rungs[cur_rung_].path}, {"aus", g_cfg.rungs[0].aus.size()}});
      awaiting_ack_ = true; ack_deadline_ = NowMonoNs() + 5LL * 1000000000LL;
    } else if (type == "stream-ack") {
      if (m.value("stream", "") != g_cfg.stream_id) return;
      std::lock_guard<std::mutex> lk(mu_);
      if (started_ || !awaiting_ack_) return;
      awaiting_ack_ = false;
      if (!m.value("ok", true)) P5G_FATAL("receiver refused the stream: " << m.value("reason", "?"));
      running_ = true; started_ = true;
      grid_ = std::thread([this] { GridLoop(); });
      P5G_LOG_INFO << "streaming " << g_cfg.stream_id << " -> " << dest_host_ << ":" << ntohs(rtp_dest_.sin_port) << " (rtcp " << ntohs(rtcp_dest_.sin_port)
                   << ", ours rtp " << rtp_port_local_ << " rtcp " << rtcp_port_local_ << ") ssrc=" << g_cfg.ssrc << " rung " << g_cfg.rungs[cur_rung_].kbps << " kbps";
    } else if (type == "profile") {
      // Edge-issued profile: pick the rung with this bitrate (exact match); applied at the next IDR.
      if (m.value("stream", g_cfg.stream_id) != g_cfg.stream_id) return;
      if (m.contains("bitrate_kbps")) {
        const int kbps = m.at("bitrate_kbps").get<int>(); int idx = -1;
        for (size_t k = 0; k < g_cfg.rungs.size(); ++k) if (g_cfg.rungs[k].kbps == kbps) idx = (int)k;
        if (idx < 0) { P5G_LOG_WARN << "profile: no rung with " << kbps << " kbps (have " << RungBytes() << ")"; return; }
        want_rung_.store(idx);
        P5G_LOG_INFO << "profile: rung -> " << kbps << " kbps (switches at the next IDR)";
      }
      for (const char* k : {"width", "height", "fps"})
        if (m.contains(k)) P5G_LOG_WARN << "profile: '" << k << "' cannot change at run time in this tree (pre-encoded); ignored";
    }
  }

  std::unique_ptr<CaptureFrameTrace> frames_;
  std::unique_ptr<EncodedFrameTrace> encoded_;
  std::unique_ptr<EncoderRateTrace> rates_;
  std::unique_ptr<RtpPacketTrace> rtp_;
  std::unique_ptr<RtcpPacketTrace> rtcp_;
  std::FILE* stats_ = nullptr;
  int64_t next_stats_ = 0;
  int width_ = 0, height_ = 0;
  AVFormatContext* oc_ = nullptr;
  bool header_written_ = false;
  int rtp_fd_ = -1, rtcp_fd_ = -1, rtp_port_local_ = 0, rtcp_port_local_ = 0;
  sockaddr_in rtp_dest_{}, rtcp_dest_{}; bool dest_set_ = false; std::string dest_host_;
  int64_t start_us_ = 0, cur_pts_ = 0; bool au_send_failed_ = false;   // grid thread
  AVDictionary* mux_opts_ = nullptr;
  std::atomic<int64_t> send_fail_{0}, rtcp_send_fail_{0}; std::atomic<int> last_send_errno_{0}; int64_t send_fail_reported_ = 0;
  uint32_t wire_base_ = 0; bool base_known_ = false;
  std::atomic<int> cur_rung_{0}, want_rung_{0};
  std::atomic<int64_t> frames_sent_{0}, missed_{0}, pkts_sent_{0}, bytes_sent_{0};
  std::atomic<uint32_t> last_sent_ts_{0};
  std::thread grid_, rtcp_rx_thread_;
  std::atomic<bool> running_{false}, rtcp_rx_running_{true}, started_{false};
  bool awaiting_ack_ = false; std::atomic<int64_t> ack_deadline_{0};
  ControlClient ctl_;
  std::mutex mu_;
};

}  // namespace p5g

static void Usage() {
  std::fprintf(stderr,
               "video_sender --control-host H --control-port P --session S --stream-id ID --to RECV_ID --trace-dir DIR\n"
               "             --source a.h264@2500[,b.h264@1000,...] [--bitrate-kbps START_RUNG] [--fps 30]\n"
               "             [--mtu 1200] [--pt 96] [--ssrc N] [--stats-period-ms 1000] [--duration S] [--start-at-epoch T.sss]\n"
               "  --start-at-epoch: wall-clock (UNIX seconds) epoch of the capture grid, shared by all senders of a run\n");
}

int main(int argc, char** argv) {
  p5g::CliArgs a(argc, argv, {"help", "control-host", "control-port", "session", "stream-id", "to", "trace-dir", "source", "bitrate-kbps",
                              "fps", "mtu", "pt", "ssrc", "stats-period-ms", "duration", "start-at-epoch"});
  if (a.Has("help")) { Usage(); return 0; }
  auto& c = p5g::g_cfg;
  c.control_host = a.Get("control-host", c.control_host); c.control_port = a.GetInt("control-port", c.control_port);
  c.session = a.Get("session", c.session); c.stream_id = a.Get("stream-id", c.stream_id); c.receiver_id = a.Get("to", c.receiver_id);
  c.trace_dir = a.Get("trace-dir", c.trace_dir); c.fps = a.GetInt("fps", c.fps); c.mtu = a.GetInt("mtu", c.mtu); c.pt = a.GetInt("pt", c.pt);
  c.ssrc = (uint32_t)std::strtoul(a.Get("ssrc", "0").c_str(), nullptr, 10);
  c.stats_period_ms = a.GetInt("stats-period-ms", c.stats_period_ms); c.duration_s = a.GetInt("duration", 0);
  c.start_kbps = a.GetInt("bitrate-kbps", 0);
  if (a.Has("start-at-epoch")) { const double t = std::atof(a.Get("start-at-epoch", "0").c_str()); if (t <= 0) P5G_FATAL("--start-at-epoch must be UNIX seconds"); c.start_wall_ns = (int64_t)(t * 1e9); }
  {  // --source a@kbps,b@kbps
    std::string s = a.Get("source", ""); size_t pos = 0;
    while (pos <= s.size() && !s.empty()) {
      size_t comma = s.find(',', pos); if (comma == std::string::npos) comma = s.size();
      const std::string item = s.substr(pos, comma - pos); const size_t at = item.rfind('@');
      if (at == std::string::npos) P5G_FATAL("--source item '" << item << "' must be path@kbps");
      p5g::Rung r; r.path = item.substr(0, at); r.kbps = std::atoi(item.c_str() + at + 1);
      if (r.kbps <= 0) P5G_FATAL("--source item '" << item << "': bad kbps");
      c.rungs.push_back(std::move(r));
      pos = comma + 1;
    }
  }
  if (c.fps <= 0) P5G_FATAL("--fps must be > 0");
  if (c.start_wall_ns && c.duration_s > 0) {   // --duration counts from the capture epoch, not from the launch
    const int64_t wait_ns = c.start_wall_ns - p5g::NowWallNs();
    if (wait_ns > 0) c.duration_s += (int)((wait_ns + 999999999LL) / 1000000000LL);
  }
  av_log_set_level(AV_LOG_ERROR);
  if (const char* lv = std::getenv("P5G_AV_LOG")) av_log_set_level(!std::strcmp(lv, "debug") ? AV_LOG_DEBUG : !std::strcmp(lv, "verbose") ? AV_LOG_VERBOSE : AV_LOG_INFO);
  P5G_LOG_INFO << "config: transport=ffmpeg stream_id=" << c.stream_id << " to=" << c.receiver_id << " session=" << c.session << " control="
               << c.control_host << ":" << c.control_port << " codec=H264 fps=" << c.fps << " source=" << a.Get("source", "")
               << " start_rung_kbps=" << c.start_kbps << " start_at_epoch_wall_ns=" << c.start_wall_ns << " mtu=" << c.mtu << " pt=" << c.pt << " stats_period_ms=" << c.stats_period_ms
               << " libavformat=" << LIBAVFORMAT_VERSION_MAJOR << "." << LIBAVFORMAT_VERSION_MINOR << "." << LIBAVFORMAT_VERSION_MICRO
               << " libavcodec=" << LIBAVCODEC_VERSION_MAJOR << "." << LIBAVCODEC_VERSION_MINOR << "." << LIBAVCODEC_VERSION_MICRO;
  p5g::InstallSignalHandlers();
  {
    p5g::Sender sender;
    if (!sender.Run()) return 1;
    p5g::RunUntilShutdown(c.duration_s, [&] { sender.Tick(); }, 100);
    sender.Shutdown();
  }
  return 0;
}
