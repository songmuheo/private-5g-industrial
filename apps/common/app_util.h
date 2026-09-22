// Small process-level helpers shared by video_sender and video_receiver:
//   clocks (mono/wall), logging macros, --key value parser, signal-driven lifecycle.
// Kept in one header on purpose: none of these is big enough to justify its own file.

#ifndef P5G_APPS_COMMON_APP_UTIL_H
#define P5G_APPS_COMMON_APP_UTIL_H

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <thread>

// Clock helpers shared by the sender/receiver apps and their trace writers.
//
// Two clocks are recorded everywhere:
//   * mono_ns  — CLOCK_MONOTONIC. libwebrtc's own clock (rtc::TimeMicros, rtc_base/system_time.cc)
//                is the same clock, so app timestamps and libwebrtc-derived timestamps are directly
//                subtractable *on the same host*. It is boot-relative and NOT comparable across hosts.
//   * wall_ns  — CLOCK_REALTIME (UNIX epoch). Comparable across hosts once they are NTP/PTP-synced,
//                which is the situation on the real testbed (UE laptop vs. receiver PC). Subject to
//                clock steps, so it is never used for same-host latency math.

namespace p5g {

inline int64_t NowMonoNs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

inline int64_t NowWallNs() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace p5g

// Minimal always-visible logging for the apps' own messages.
//
// libwebrtc's release build defaults to LS_NONE (rtc_base/logging.cc), so RTC_LOG(LS_ERROR) prints
// nothing. We keep libwebrtc silent on purpose during measurements (its hot paths log more when the
// link is bad, which would make I/O load depend on the condition under test) and route only our own
// lifecycle / fatal messages through here. Enable libwebrtc logs for debugging with
// P5G_WEBRTC_LOG=warning|info|verbose (see webrtc_session.h).

namespace p5g {

// Builds the whole line first and emits it with a single fputs so concurrent threads do not
// interleave fragments.
class LogLine {
 public:
  explicit LogLine(const char* level) { os_ << "[p5g][" << level << "] "; }
  ~LogLine() {
    os_ << '\n';
    const std::string s = os_.str();
    std::fputs(s.c_str(), stderr);
  }
  LogLine(const LogLine&) = delete;
  LogLine& operator=(const LogLine&) = delete;
  template <typename T>
  LogLine& operator<<(const T& v) {
    os_ << v;
    return *this;
  }

 private:
  std::ostringstream os_;
};

}  // namespace p5g

#define P5G_LOG_INFO p5g::LogLine("info")
#define P5G_LOG_WARN p5g::LogLine("warn")
#define P5G_LOG_ERROR p5g::LogLine("error")

// Fatal: print and terminate immediately. Used for measurement-integrity violations where continuing
// would silently produce an unusable run (e.g. header extension not negotiable, trace path reuse).
#define P5G_FATAL(msg)                                     \
  do {                                                     \
    p5g::LogLine("FATAL") << msg;                          \
    std::_Exit(1);                                         \
  } while (0)

// Tiny command line parser (avoids linking absl::flags): "--key value", "--key=value" or a bare
// "--flag" (= "1"). Anything else is an error: a misspelled or malformed flag must never fall back to
// a default silently (a 2-UE run was lost to exactly that).

namespace p5g {

class CliArgs {
 public:
  // `known`: every flag the program accepts (without the leading "--"). Unknown flags and stray
  // positional words terminate the program with a message naming the offending token.
  CliArgs(int argc, char** argv, std::initializer_list<const char*> known) {
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      if (k.rfind("--", 0) != 0) P5G_FATAL("unexpected argument '" << k << "' (flags are --key value); see --help");
      k = k.substr(2);
      std::string v;
      const size_t eq = k.find('=');
      if (eq != std::string::npos) {           // --key=value
        v = k.substr(eq + 1);
        k = k.substr(0, eq);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        v = argv[++i];                          // --key value
      } else {
        v = "1";                                // bare flag
      }
      bool ok = false;
      for (const char* kn : known) ok = ok || (k == kn);
      if (!ok) P5G_FATAL("unknown flag --" << k << "; see --help");
      kv_[k] = v;
    }
  }
  bool Has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string Get(const std::string& k, const std::string& def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  int GetInt(const std::string& k, int def) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::atoi(it->second.c_str());
  }

 private:
  std::map<std::string, std::string> kv_;
};

}  // namespace p5g

// Process lifecycle: SIGTERM / SIGINT -> orderly shutdown (traces are flushed; a hard kill would lose
// the tail). The poll loop also drives the periodic getStats() sampling.

namespace p5g {

inline volatile std::sig_atomic_t& ShutdownFlag() {
  static volatile std::sig_atomic_t f = 0;
  return f;
}

inline void OnShutdownSignal(int) { ShutdownFlag() = 1; }

inline void InstallSignalHandlers() {
  std::signal(SIGTERM, OnShutdownSignal);
  std::signal(SIGINT, OnShutdownSignal);
}

// Poll period of the lifecycle loop.
inline constexpr int kLifecyclePollMs = 20;

// Blocks until a shutdown signal arrives or duration_s elapses (duration_s <= 0: signal only).
// on_tick runs every tick_ms on this thread (periodic getStats() sampling; 0 disables).

inline void RunUntilShutdown(int duration_s, const std::function<void()>& on_tick = {}, int tick_ms = 0) {
  using clock = std::chrono::steady_clock;
  const auto deadline =
      duration_s > 0 ? clock::now() + std::chrono::seconds(duration_s) : clock::time_point::max();
  auto next_tick = clock::now() + std::chrono::milliseconds(tick_ms > 0 ? tick_ms : 1);
  while (ShutdownFlag() == 0 && clock::now() < deadline) {
    if (on_tick && tick_ms > 0 && clock::now() >= next_tick) {
      on_tick();
      next_tick += std::chrono::milliseconds(tick_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kLifecyclePollMs));
  }
}

}  // namespace p5g

#endif  // P5G_APPS_COMMON_APP_UTIL_H
