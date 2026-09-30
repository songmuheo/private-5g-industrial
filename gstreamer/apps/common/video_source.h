// Video source for the gstreamer sender: raw I420 file playback (or a synthetic pattern) on a fixed
// capture grid, pushed into an `appsrc` element.
//
// Based on: subprojects/gst-plugins-base/tests/examples/app/appsrc-stream.c @ GStreamer 1.20.3
//           (appsrc in push mode: gst_app_src_push_buffer / end-of-stream). That example streams a file
//           from need-data/idle callbacks and is not live; the live, format=time, self-timestamped use
//           here follows the appsrc API documentation (gst-plugins-base/gst-libs/gst/app/gstappsrc.c,
//           "is-live", "format", "block", "max-bytes") — our variant drives the push from its own
//           real-time thread because the grid is the experiment's clock and must not depend on the
//           pipeline's scheduling.
// Local modifications: absolute-time capture grid (clock_nanosleep TIMER_ABSTIME), zero-copy
//           buffers wrapping the mmap'ed source, one trace row per slot, rtp_ts precomputed with the
//           payloader's arithmetic (gst_util.h RtpTsFromRunningTime).
//
// Per capture slot the thread:
//   1. sleeps until the slot's absolute CLOCK_MONOTONIC time (wake-up jitter does not accumulate),
//   2. stamps capture_mono_ns / capture_wall_ns,
//   3. wraps the source frame in a GstBuffer (no copy) with PTS = slot time - pipeline start,
//   4. writes ONE trace row (to_encoder = whether appsrc accepted the buffer) so the number of offered
//      frames is a constant of the run,
//   5. pushes it (gst_app_src_push_buffer, takes ownership; blocks if `block=true` and the queue is full).
//
// Overload: if a push blocked long enough that whole grid slots passed, those slots are NOT silently
// skipped — one row per missed slot is written with to_encoder=0 and the count is reported at stop
// (`skipped_slots()`), so the offered-frame denominator stays fps x duration and an encoder that cannot
// keep up shows up in tx-frames.csv instead of as a lower fps. Nothing else in this stack drops frames.
//
// Frame layout: tightly packed I420 (Y then U then V, no padding), which is GStreamer's default I420
// layout only when width % 4 == 0 and height % 2 == 0 (GstVideoInfo strides are rounded up to 4);
// other sizes are refused at start-up rather than mis-read.
//
// Frame identity: the payloader puts rtp_ts = PTS * 90 kHz on the wire (timestamp-offset 0), so the
// value logged here is the wire timestamp of the frame, and the receiver logs the same number. Pixels
// are never modified for identification. fps and resolution are fixed by construction: the grid
// defines the frame times and appsrc's caps pin the size; nothing downstream can change either.
#ifndef P5G_APPS_COMMON_VIDEO_SOURCE_H
#define P5G_APPS_COMMON_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
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

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video-info.h>

#include "app_util.h"
#include "gst_util.h"
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

class GridVideoSource {
 public:
  GridVideoSource(const VideoSourceConfig& cfg, GstAppSrc* appsrc, CaptureFrameTrace* trace)
      : cfg_(cfg), frame_interval_us_(cfg.fps > 0 ? 1000000 / cfg.fps : 33333), appsrc_(appsrc), trace_(trace) {
    // Our tightly packed frames must be exactly what downstream expects for these caps.
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (!gst_video_info_set_format(&info, GST_VIDEO_FORMAT_I420, cfg.width, cfg.height))
      P5G_FATAL("unsupported video size " << cfg.width << "x" << cfg.height);
    const size_t packed = static_cast<size_t>(cfg.width) * cfg.height * 3 / 2;
    if (info.size != packed || cfg.width % 4 != 0 || cfg.height % 2 != 0)
      P5G_FATAL("I420 " << cfg.width << "x" << cfg.height << " is not tightly packed in GStreamer's default layout (size "
                        << info.size << " vs " << packed << "); use width % 4 == 0 and height % 2 == 0");
    frames_ = cfg.yuv_path.empty() ? MakePatternFrames(cfg.width, cfg.height) : OpenYuvFile(cfg.yuv_path, cfg.width, cfg.height);
    P5G_LOG_INFO << "video source: " << (cfg.yuv_path.empty() ? "synthetic pattern" : cfg.yuv_path) << " " << cfg.width
                 << "x" << cfg.height << "@" << cfg.fps << " frames=" << frames_->count;
  }
  ~GridVideoSource() { Stop(); }

  // Caps appsrc must be configured with (pins format, size and nominal rate of every buffer).
  GstCaps* MakeCaps() const {
    return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT, cfg_.width,
                               "height", G_TYPE_INT, cfg_.height, "framerate", GST_TYPE_FRACTION, cfg_.fps, 1, nullptr);
  }

  // start_mono_ns: the pipeline's base time (running time 0) in CLOCK_MONOTONIC ns — PTS are relative to it.
  void Start(int64_t start_mono_ns) {
    start_mono_ns_ = start_mono_ns;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
  }
  // Two-step stop: RequestStop() only raises the flag (the thread may be blocked inside
  // gst_app_src_push_buffer when the queue is full; appsrc releases it on end-of-stream or when the
  // pipeline leaves PLAYING), Join() is called by the owner after the pipeline was drained / set to NULL.
  void RequestStop() { running_ = false; }
  // Waits (bounded) until the grid loop has left its last slot, so that an end-of-stream sent afterwards
  // comes after the last real push instead of racing it. Returns false if the thread is still inside a
  // (blocked) push after the timeout.
  bool WaitStopped(int timeout_ms) {
    for (int i = 0; i < timeout_ms; ++i) {
      if (loop_done_.load()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return loop_done_.load();
  }
  void Join() {
    if (thread_.joinable()) thread_.join();
  }
  void Stop() { RequestStop(); Join(); }
  int64_t frames_pushed() const { return pushed_.load(); }
  int64_t skipped_slots() const { return skipped_.load(); }
  uint32_t last_pushed_rtp_ts() const { return last_pushed_ts_.load(); }  // for the owner's drain check at shutdown

 private:
  static void SleepUntilMonoUs(int64_t target_us) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(target_us / 1000000);
    ts.tv_nsec = static_cast<long>((target_us % 1000000) * 1000);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
  }

  void Loop() {
    int64_t slot = (NowMonoNs() / 1000) / frame_interval_us_ + 1;
    int64_t idx = 0;
    while (running_) {
      int64_t target_us = slot * frame_interval_us_;
      SleepUntilMonoUs(target_us);
      const int64_t now_us = NowMonoNs() / 1000;
      if (now_us >= (slot + 1) * frame_interval_us_) {  // fell behind (blocked push): the missed slots are
        const int64_t resume = now_us / frame_interval_us_;  // recorded (to_encoder=0), then realign
        for (; slot < resume && running_; ++slot) EmitMissedSlot(slot, idx);
        target_us = slot * frame_interval_us_;
      }
      EmitSlot(slot, idx, target_us);
      ++slot;
    }
    // Slots that elapsed while the LAST push was blocked (stop arrived during the block): still recorded,
    // so the offered-frame denominator is exact up to the moment the grid stopped.
    const int64_t now_us = NowMonoNs() / 1000;
    for (; (slot + 1) * frame_interval_us_ <= now_us; ++slot) EmitMissedSlot(slot, idx);
    loop_done_ = true;
  }

  // A slot that passed while the previous push was blocked: no buffer, one row, counted.
  void EmitMissedSlot(int64_t slot, int64_t& idx) {
    const int64_t target_us = slot * frame_interval_us_;
    const GstClockTime pts = static_cast<GstClockTime>(target_us * 1000 - start_mono_ns_);
    if (trace_)
      trace_->Write(CaptureFrameRow{idx, slot, idx % frames_->count, RtpTsFromRunningTime(pts), NowWallNs(), NowMonoNs(),
                                    cfg_.width, cfg_.height, 0});
    ++idx;
    skipped_++;
  }

  void EmitSlot(int64_t slot, int64_t& idx, int64_t target_us) {
    const int64_t capture_wall_ns = NowWallNs();
    const int64_t capture_mono_ns = NowMonoNs();
    const int64_t src_idx = idx % frames_->count;
    const GstClockTime pts = static_cast<GstClockTime>(target_us * 1000 - start_mono_ns_);
    const uint32_t rtp_ts = RtpTsFromRunningTime(pts);

    // Zero-copy: the buffer references the mmap'ed / pattern memory; `frames_` outlives the pipeline.
    GstBuffer* buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, const_cast<uint8_t*>(frames_->frame(src_idx)),
                                                 frames_->frame_bytes(), 0, frames_->frame_bytes(), nullptr, nullptr);
    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = static_cast<GstClockTime>(frame_interval_us_) * 1000;
    GST_BUFFER_OFFSET(buf) = idx;
    const GstFlowReturn fr = gst_app_src_push_buffer(appsrc_, buf);  // takes ownership
    const bool ok = fr == GST_FLOW_OK;
    if (ok) { pushed_++; last_pushed_ts_.store(rtp_ts); }
    if (trace_)
      trace_->Write(CaptureFrameRow{idx, slot, src_idx, rtp_ts, capture_wall_ns, capture_mono_ns, cfg_.width, cfg_.height, ok ? 1 : 0});
    ++idx;
  }

  const VideoSourceConfig cfg_;
  const int64_t frame_interval_us_;
  GstAppSrc* const appsrc_;
  CaptureFrameTrace* const trace_;
  std::shared_ptr<SourceFrames> frames_;
  int64_t start_mono_ns_ = 0;
  std::atomic<int64_t> pushed_{0};
  std::atomic<int64_t> skipped_{0};
  std::atomic<uint32_t> last_pushed_ts_{0};
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> loop_done_{false};
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_VIDEO_SOURCE_H
