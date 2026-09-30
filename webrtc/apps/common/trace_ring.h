// Lock-free circular trace ring + the per-frame row types written through it.

#ifndef P5G_APPS_COMMON_TRACE_RING_H
#define P5G_APPS_COMMON_TRACE_RING_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

#include "app_util.h"

// Allocation-free, lock-free circular trace ring for the media hot paths.
//
// Producers (capture thread, encoder callback, pacer / network threads, decode queue) copy one POD
// row into a slot and publish it; a background thread (nice 19) formats the published rows to CSV
// every kFlushPeriodMs and frees the slots. Memory stays at `capacity * sizeof(Row)` for the whole
// run regardless of run length. If producers outrun the flusher the row is dropped and COUNTED
// (footer + `<path>.ERROR` sidecar), never silently.
//
// Producer cost: one CAS to claim a sequence number, one POD copy, one release store (row and flag
// in the same slot; capacity is a power of two so the slot index is a mask). Works for any number of
// producer threads; the flusher is the only consumer.

namespace p5g {

inline constexpr int kFlushPeriodMs = 500;
// Ring depths = rows that may accumulate within one flush period (plus slack for a delayed flusher).
inline constexpr size_t kFrameTraceCapacity = 4096;    // 30 fps -> 15 rows per flush period
inline constexpr size_t kPacketTraceCapacity = 65536;  // ~130k packets/s sustained

template <typename Row, bool kMultiWriter = false>  // kMultiWriter kept for call-site clarity only
class TraceRing {
 public:
  using Formatter = void (*)(std::FILE*, const Row&);

  // capacity is rounded up to a power of two so slot selection is a mask, not a 64-bit division.
  TraceRing(std::string path, std::string header, Formatter fmt, size_t capacity)
      : path_(std::move(path)), fmt_(fmt), cap_(RoundUpPow2(capacity)), mask_(cap_ - 1),
        slots_(new Slot[cap_]) {
    // A trace that cannot be created would silently lose the run (the app itself keeps working), so
    // this is fatal: the usual cause is a wrong / missing --trace-dir.
    std::FILE* f = std::fopen(path_.c_str(), "w");
    if (!f || std::fprintf(f, "%s\n", header.c_str()) < 0 || std::fclose(f) != 0)
      P5G_FATAL("cannot create trace file " << path_ << " (check --trace-dir exists and is writable)");
    enabled_ = true;
    running_ = true;
    flusher_ = std::thread([this] { FlushLoop(); });
  }
  ~TraceRing() { Close(); }

  // Hot path: one CAS to claim a slot, one POD copy, one release store. The row and its ready flag
  // live in the same slot, so a write touches one cache line (two if the row straddles a boundary).
  void Write(const Row& r) {
    if (!enabled_) return;
    if (closed_.load(std::memory_order_acquire)) {
      late_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    size_t i = claimed_.load(std::memory_order_relaxed);
    for (;;) {
      if (i - consumed_.load(std::memory_order_acquire) >= cap_) {  // slot still holds an unflushed row
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (claimed_.compare_exchange_weak(i, i + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
        break;
    }
    Slot& s = slots_[i & mask_];
    s.row = r;
    s.ready.store(1, std::memory_order_release);
  }

  const std::string& path() const { return path_; }
  int64_t count() const { return static_cast<int64_t>(claimed_.load(std::memory_order_relaxed)); }

  // Call once after the producers have stopped.
  void Close() {
    if (!enabled_ || closed_.exchange(true, std::memory_order_acq_rel)) return;
    running_.store(false, std::memory_order_release);
    if (flusher_.joinable()) flusher_.join();
    // Let producers that passed the closed_ check publish their slot.
    for (int i = 0; i < 100 && consumed_.load() != claimed_.load(); ++i) {
      FlushOnce(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FlushOnce(true);
  }

 private:
  struct Slot {
    Row row;
    std::atomic<uint8_t> ready{0};
  };

  static size_t RoundUpPow2(size_t n) {
    size_t c = 1;
    while (c < n) c <<= 1;
    return c;
  }

  static std::string BootId() {
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string v;
    return (f && std::getline(f, v) && !v.empty()) ? v : "unknown";
  }

  // Flusher only. Writes the contiguous run of published rows and frees their slots.
  void FlushOnce(bool final) {
    size_t m = consumed_.load(std::memory_order_relaxed);
    const size_t end = claimed_.load(std::memory_order_acquire);
    if (m == end && !final) return;
    std::FILE* f = std::fopen(path_.c_str(), "a");
    if (!f) {
      std::ofstream(path_ + ".ERROR") << "append-failed\n";
      return;
    }
    for (; m != end && slots_[m & mask_].ready.load(std::memory_order_acquire); ++m) {
      fmt_(f, slots_[m & mask_].row);
      slots_[m & mask_].ready.store(0, std::memory_order_release);
      written_++;
    }
    consumed_.store(m, std::memory_order_release);
    if (final) {
      const size_t ov = overflow_.load(), lt = late_.load(), unpub = end - m;
      std::fprintf(f, "# rows=%zu clock_domain=%s\n", written_, BootId().c_str());
      if (ov || lt || unpub) {
        std::fprintf(f, "# overflow=%zu late=%zu unpublished=%zu\n", ov, lt, unpub);
        std::ofstream(path_ + ".ERROR") << "overflow=" << ov << " late=" << lt << " unpublished=" << unpub << "\n";
      }
    }
    if (std::fclose(f) != 0) std::ofstream(path_ + ".ERROR") << "close-failed\n";
  }

  void FlushLoop() {
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);  // this thread only
    while (running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kFlushPeriodMs));
      FlushOnce(false);
    }
  }

  static constexpr size_t kCacheLine = 64;
  const std::string path_;
  const Formatter fmt_;
  const size_t cap_;
  const size_t mask_;
  std::unique_ptr<Slot[]> slots_;
  // Producer-side counter and flusher-side counter on separate cache lines: the flusher's
  // consumed_ store (every 500 ms) never invalidates the line the producers CAS on.
  alignas(kCacheLine) std::atomic<size_t> claimed_{0};
  alignas(kCacheLine) std::atomic<size_t> consumed_{0};
  alignas(kCacheLine) std::atomic<size_t> overflow_{0};
  std::atomic<size_t> late_{0};
  std::atomic<bool> closed_{false}, running_{false};
  std::thread flusher_;
  size_t written_ = 0;  // flusher private
  bool enabled_ = false;
};

}  // namespace p5g

// Per-frame trace rows written by the sender (capture side) and receiver (decoded side).
// Column meanings: docs/TRACE_SCHEMA.md. Join key between the two files is rtp_ts; the receiver
// sees rtp_ts + K where K is the per-SSRC random RTP timestamp offset libwebrtc adds on the wire
// (RFC 3550 §5.1); the receiver logs sender_rtp_ts_est / wire_offset_est live from the abs-capture-time
// extension so both sides can be matched without post-processing.

namespace p5g {

// <stream>-tx-frames.csv : one row per capture grid slot (also for slots the source did not hand
// to the encoder, to_encoder=0, so the offered-frame denominator is constant).
struct CaptureFrameRow {
  int64_t frame_idx;         // running capture index
  int64_t grid_slot;         // capture grid slot (slot * frame_interval = nominal capture time)
  int64_t src_frame_idx;     // index into the source file / pattern sequence
  uint32_t rtp_ts;           // RTP timestamp libwebrtc will assign (sender space, pre-offset)
  int64_t capture_wall_ns;
  int64_t capture_mono_ns;
  int32_t width, height;     // resolution handed downstream (capture res. if to_encoder=0)
  int32_t to_encoder;        // 1 = forwarded to encoder, 0 = dropped by VideoAdapter (frame-rate adaptation)
};

inline constexpr const char* kCaptureFrameHeader =
    "frame_idx,grid_slot,src_frame_idx,rtp_ts,capture_wall_ns,capture_mono_ns,width,height,to_encoder";

inline void FormatCaptureFrameRow(std::FILE* f, const CaptureFrameRow& r) {
  std::fprintf(f, "%lld,%lld,%lld,%u,%lld,%lld,%d,%d,%d\n", (long long)r.frame_idx,
               (long long)r.grid_slot, (long long)r.src_frame_idx, r.rtp_ts,
               (long long)r.capture_wall_ns, (long long)r.capture_mono_ns, r.width, r.height,
               r.to_encoder);
}

// <stream>-rx-frames.csv : one row per decoded frame delivered to the app sink.
struct DecodedFrameRow {
  int64_t frame_idx;
  uint32_t rtp_ts;              // wire space (sender rtp_ts + K)
  int64_t abs_capture_ntp_ms;   // sender capture NTP from abs-capture-time ext (-1 if absent)
  // Sender-space RTP timestamp reconstructed live from abs_capture_ntp_ms with the same formula the
  // sender uses (90 * (uint32)ntp_ms); equals the sender's tx-frames.rtp_ts within +-90 ticks (1 ms
  // rounding between the two NTP paths). -1 if the extension was absent. Lets a human match a
  // received frame to the sender's row without any offline processing.
  int64_t sender_rtp_ts_est;
  int64_t wire_offset_est;      // rtp_ts - sender_rtp_ts_est (mod 2^32): the per-SSRC constant K
  int64_t recv_wall_ns;
  int64_t recv_mono_ns;         // time the app could use the frame (sink OnFrame entry)
  int32_t width, height;
  int32_t num_packets;          // RTP packets that formed this frame
  int64_t first_pkt_mono_ns;    // earliest / latest payload-processing time of those packets
  int64_t last_pkt_mono_ns;     //   (NOT socket arrival; see docs/TRACE_SCHEMA.md)
  uint32_t ssrc;
};

inline constexpr const char* kDecodedFrameHeader =
    "frame_idx,rtp_ts,abs_capture_ntp_ms,sender_rtp_ts_est,wire_offset_est,recv_wall_ns,recv_mono_ns,"
    "width,height,num_packets,first_pkt_mono_ns,last_pkt_mono_ns,ssrc";

inline void FormatDecodedFrameRow(std::FILE* f, const DecodedFrameRow& r) {
  std::fprintf(f, "%lld,%u,%lld,%lld,%lld,%lld,%lld,%d,%d,%d,%lld,%lld,%u\n", (long long)r.frame_idx,
               r.rtp_ts, (long long)r.abs_capture_ntp_ms, (long long)r.sender_rtp_ts_est,
               (long long)r.wire_offset_est, (long long)r.recv_wall_ns,
               (long long)r.recv_mono_ns, r.width, r.height, r.num_packets,
               (long long)r.first_pkt_mono_ns, (long long)r.last_pkt_mono_ns, r.ssrc);
}

using CaptureFrameTrace = TraceRing<CaptureFrameRow>;
using DecodedFrameTrace = TraceRing<DecodedFrameRow>;

}  // namespace p5g

#endif  // P5G_APPS_COMMON_TRACE_RING_H
