// Video source for the sender: raw I420 file playback (or a synthetic pattern) on a fixed capture
// grid, following the structure of libwebrtc's own test capturer (test/test_video_capturer.cc) and
// the CapturerTrackSource pattern of examples/peerconnection/client/conductor.cc.
//
// Per capture slot the source:
//   1. sleeps until the slot's absolute CLOCK_MONOTONIC time (clock_nanosleep TIMER_ABSTIME, so
//      wake-up jitter does not accumulate),
//   2. stamps capture_mono_ns / capture_wall_ns,
//   3. asks cricket::VideoAdapter whether the sink wants this frame (frame-rate / resolution
//      adaptation requested by the encoder — same logic as the stock test capturer),
//   4. writes ONE trace row (also when the adapter dropped the slot: to_encoder=0) so the number of
//      offered frames is a constant of the run,
//   5. forwards the frame with timestamp_us = slot time and ntp_time_ms = slot time + fixed offset.
//
// RTP timestamp identity (CITE: M120 video/video_stream_encoder.cc, OnFrame): when the source sets
// ntp_time_ms > 0, libwebrtc uses it as the capture NTP time and sets the encoder input RTP
// timestamp to 90 * (uint32_t)ntp_time_ms. We compute the same value here, so every trace row
// already carries the rtp_ts that will identify the frame downstream (encoder ledger, RTP ledger,
// receiver). Pixels are never modified for identification.
#ifndef P5G_APPS_COMMON_VIDEO_SOURCE_H
#define P5G_APPS_COMMON_VIDEO_SOURCE_H

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "common_video/include/video_frame_buffer.h"  // WrapI420Buffer
#include "media/base/video_adapter.h"
#include "media/base/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/clock.h"

#include "app_util.h"
#include "trace_ring.h"

namespace p5g {

struct VideoSourceConfig {
  std::string yuv_path;   // raw I420 file (width x height frames back to back); empty = pattern
  int width = 1280;
  int height = 720;
  int fps = 30;
};

// Source frame storage: either an mmap'ed .yuv file or an in-memory synthetic sequence.
struct SourceFrames {
  const uint8_t* base = nullptr;
  size_t len = 0;
  int fd = -1;
  int width = 0, height = 0;
  int64_t count = 0;
  std::vector<uint8_t> owned;  // pattern mode
  ~SourceFrames() {
    if (fd >= 0) {
      if (base) ::munmap(const_cast<uint8_t*>(base), len);
      ::close(fd);
    }
  }
  size_t frame_bytes() const { return static_cast<size_t>(width) * height * 3 / 2; }
  const uint8_t* frame(int64_t i) const { return base + (i % count) * frame_bytes(); }
};

// Synthetic content that keeps the encoder busy like camera footage would: a moving diagonal
// gradient plus a bouncing block (spatial detail + temporal change). 64 unique frames, looped.
inline std::shared_ptr<SourceFrames> MakePatternFrames(int w, int h) {
  auto s = std::make_shared<SourceFrames>();
  s->width = w;
  s->height = h;
  s->count = 64;
  s->owned.resize(s->frame_bytes() * s->count);
  s->base = s->owned.data();
  s->len = s->owned.size();
  for (int64_t t = 0; t < s->count; ++t) {
    uint8_t* y = s->owned.data() + t * s->frame_bytes();
    uint8_t* u = y + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(w / 2) * (h / 2);
    const int bx = static_cast<int>((t * w) / s->count), by = static_cast<int>((t * h) / s->count);
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i) {
        uint8_t val = static_cast<uint8_t>(((i + j + t * 6) / 3) & 0xFF);
        if (i >= bx && i < bx + w / 8 && j >= by && j < by + h / 8) val = 235;
        y[static_cast<size_t>(j) * w + i] = val;
      }
    }
    for (int j = 0; j < h / 2; ++j) {
      for (int i = 0; i < w / 2; ++i) {
        u[static_cast<size_t>(j) * (w / 2) + i] = static_cast<uint8_t>(128 + (i * 64) / (w / 2) - 32);
        v[static_cast<size_t>(j) * (w / 2) + i] = static_cast<uint8_t>(128 + (j * 64) / (h / 2) - 32 + t);
      }
    }
  }
  return s;
}

inline std::shared_ptr<SourceFrames> OpenYuvFile(const std::string& path, int w, int h) {
  auto s = std::make_shared<SourceFrames>();
  s->width = w;
  s->height = h;
  s->fd = ::open(path.c_str(), O_RDONLY);
  if (s->fd < 0) P5G_FATAL("cannot open yuv " << path << ": " << std::strerror(errno));
  struct stat st {};
  if (::fstat(s->fd, &st) != 0 || st.st_size <= 0) P5G_FATAL("fstat failed for " << path);
  const size_t fb = s->frame_bytes();
  if (static_cast<size_t>(st.st_size) % fb != 0)
    P5G_FATAL("yuv size " << st.st_size << " is not a multiple of " << w << "x" << h << " I420 frames");
  s->len = static_cast<size_t>(st.st_size);
  s->count = static_cast<int64_t>(s->len / fb);
  // MAP_POPULATE: prefault outside the measurement window so page faults do not land on the
  // encoder thread only for the frames that happen to get encoded.
  void* m = ::mmap(nullptr, s->len, PROT_READ, MAP_SHARED | MAP_POPULATE, s->fd, 0);
  if (m == MAP_FAILED) P5G_FATAL("mmap failed for " << path);
  s->base = static_cast<const uint8_t*>(m);
  return s;
}

class GridVideoCapturer : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
 public:
  GridVideoCapturer(const VideoSourceConfig& cfg, CaptureFrameTrace* trace)
      : width_(cfg.width), height_(cfg.height),
        frame_interval_us_(cfg.fps > 0 ? 1000000 / cfg.fps : 33333), trace_(trace) {
    frames_ = cfg.yuv_path.empty() ? MakePatternFrames(cfg.width, cfg.height)
                                   : OpenYuvFile(cfg.yuv_path, cfg.width, cfg.height);
    P5G_LOG_INFO << "video source: " << (cfg.yuv_path.empty() ? "synthetic pattern" : cfg.yuv_path)
                 << " " << cfg.width << "x" << cfg.height << "@" << cfg.fps << " frames=" << frames_->count;
  }
  ~GridVideoCapturer() override { Stop(); }

  void Start() {
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
  }
  void Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }
  // Identical to test/test_video_capturer.cc: forward merged sink wants to the adapter unchanged.
  void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override {
    broadcaster_.AddOrUpdateSink(sink, wants);
    adapter_.OnSinkWants(broadcaster_.wants());
  }
  void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
    broadcaster_.RemoveSink(sink);
    adapter_.OnSinkWants(broadcaster_.wants());
  }

 private:
  static void SleepUntilMonoUs(int64_t target_us) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(target_us / 1000000);
    ts.tv_nsec = static_cast<long>((target_us % 1000000) * 1000);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
  }

  void Loop() {
    int64_t slot = rtc::TimeMicros() / frame_interval_us_ + 1;
    int64_t idx = 0;
    while (running_) {
      int64_t target_us = slot * frame_interval_us_;
      SleepUntilMonoUs(target_us);
      int64_t now_us = rtc::TimeMicros();
      if (now_us >= (slot + 1) * frame_interval_us_) {  // fell behind: realign to the grid
        slot = now_us / frame_interval_us_;               // (visible in tx-frames.csv as a gap in grid_slot)
        target_us = slot * frame_interval_us_;
      }
      // Only count slots as "offered" once a sink (encoder) exists — before the PeerConnection is
      // negotiated nothing could have been sent.
      if (broadcaster_.frame_wanted()) EmitSlot(slot, idx, target_us);
      ++slot;
    }
  }

  void EmitSlot(int64_t slot, int64_t& idx, int64_t target_us) {
    const int64_t capture_wall_ns = NowWallNs();
    const int64_t capture_mono_ns = NowMonoNs();
    const int64_t src_idx = idx % frames_->count;

    // Zero-copy reference into the source buffer (same pattern as test/frame_generator.cc).
    const int nw = frames_->width, nh = frames_->height;
    const uint8_t* sy = frames_->frame(src_idx);
    const uint8_t* su = sy + static_cast<size_t>(nw) * nh;
    const uint8_t* sv = su + static_cast<size_t>(nw / 2) * (nh / 2);
    rtc::scoped_refptr<webrtc::VideoFrameBuffer> src =
        webrtc::WrapI420Buffer(nw, nh, sy, nw, su, nw / 2, sv, nw / 2, [keep = frames_] {});

    int crop_w = width_, crop_h = height_, out_w = width_, out_h = height_;
    const bool to_encoder = adapter_.AdaptFrameResolution(width_, height_, target_us * 1000,
                                                          &crop_w, &crop_h, &out_w, &out_h);
    if (!to_encoder) {
      out_w = width_;
      out_h = height_;
      broadcaster_.OnDiscardedFrame();  // keeps libwebrtc's framesDropped stats consistent
    }
    rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = src;
    if (to_encoder && (out_w != width_ || out_h != height_)) {
      auto scaled = webrtc::I420Buffer::Create(out_w, out_h);
      scaled->ScaleFrom(*src->GetI420());
      buffer = scaled;
    }

    // Capture NTP for this slot (fixed monotonic->NTP offset computed once so RTP timestamps track
    // the capture cadence exactly). Strictly increasing: libwebrtc drops frames with a repeated NTP.
    int64_t ntp_ms = target_us / 1000 + ntp_delta_ms_;
    if (ntp_ms <= last_ntp_ms_) ntp_ms = last_ntp_ms_ + 1;
    last_ntp_ms_ = ntp_ms;
    const uint32_t rtp_ts = 90u * static_cast<uint32_t>(ntp_ms);  // same uint32 arithmetic as libwebrtc

    if (trace_)
      trace_->Write(CaptureFrameRow{idx, slot, src_idx, rtp_ts, capture_wall_ns, capture_mono_ns,
                                    out_w, out_h, to_encoder ? 1 : 0});
    ++idx;
    if (!to_encoder) return;

    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                   .set_video_frame_buffer(buffer)
                                   .set_timestamp_us(target_us)  // -> abs-capture-time extension
                                   .set_ntp_time_ms(ntp_ms)      // -> RTP timestamp
                                   .set_rotation(webrtc::kVideoRotation_0)
                                   .build();
    broadcaster_.OnFrame(frame);
  }

  const int width_, height_;
  const int64_t frame_interval_us_;
  CaptureFrameTrace* const trace_;
  std::shared_ptr<SourceFrames> frames_;
  int64_t last_ntp_ms_ = 0;
  const int64_t ntp_delta_ms_ =
      webrtc::Clock::GetRealTimeClock()->CurrentNtpInMilliseconds() - rtc::TimeMicros() / 1000;
  cricket::VideoAdapter adapter_;
  rtc::VideoBroadcaster broadcaster_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

// conductor.cc's CapturerTrackSource equivalent.
class GridVideoTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<GridVideoTrackSource> Create(const VideoSourceConfig& cfg,
                                                         CaptureFrameTrace* trace) {
    auto cap = std::make_unique<GridVideoCapturer>(cfg, trace);
    cap->Start();
    return rtc::make_ref_counted<GridVideoTrackSource>(std::move(cap));
  }
  explicit GridVideoTrackSource(std::unique_ptr<GridVideoCapturer> cap)
      : webrtc::VideoTrackSource(/*remote=*/false), capturer_(std::move(cap)) {}
  void StopCapture() { capturer_->Stop(); }

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override { return capturer_.get(); }
  std::unique_ptr<GridVideoCapturer> capturer_;
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_VIDEO_SOURCE_H
