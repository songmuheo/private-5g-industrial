// Unit test of the AU indexer + RFC 6184 depacketizer against libavformat's rtp muxer (the sender's path).
// Every AU of an Annex B file is packetized in memory and fed to H264Depacketizer; each case below asserts the
// exact events (count, bytes, rtp_ts progression, IDR flag, packet/loss counters):
//   normal AUs      : exactly one complete event per AU, byte-identical (start codes normalised), ts step 3000
//   AU 5  mid drop  : one packet dropped inside -> incomplete, lost_packets=1, fewer bytes, no partial NAL delivered
//   AU 8  tail drop : marker packet dropped -> emitted by AU 9's first packet, incomplete, lost_packets=1; AU 9 complete
//   AU 12 head drop : first packet dropped -> incomplete, lost_packets=1
//   AU 15 duplicate : packet 1 repeated -> complete, no loss, late_or_dup=1
//   AU 18 reorder   : packets 1,2 swapped -> the late one is dropped: incomplete, lost_packets=1, late_or_dup=2 total
//   two events      : AU N-1's marker dropped, then a hand-made one-packet AU with marker -> Push returns 2
//   wraparound      : sequence numbers start at 65500 (muxer `seq`), so they wrap during the file: seq_resets=0
// Run by scripts/build_apps.sh with a small test stream produced by x264 on the fly.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include "rtp_util.h"
using namespace p5g;
static std::vector<std::vector<uint8_t>> g_pkts;
static int WritePkt(void*, uint8_t* b, int n) { if (!IsRtcp(b, n)) g_pkts.emplace_back(b, b + n); return n; }
// normalise start codes to 4 bytes so the comparison is layout-independent
static std::vector<uint8_t> Norm(const uint8_t* d, int n) {
  std::vector<uint8_t> out; int64_t sc = 0, pos = NextStartCode(d, n, 0, &sc);
  while (pos >= 0) { int64_t nsc = 0; int64_t nx = NextStartCode(d, n, pos, &nsc); int64_t end = nx < 0 ? n : nsc;
    out.insert(out.end(), {0, 0, 0, 1}); out.insert(out.end(), d + pos, d + end); if (nx < 0) break; pos = nx; }
  return out;
}
// every NAL in `out` must be one of the NALs of `ref` (no partial NAL delivered)
static bool OnlyWholeNals(const std::vector<uint8_t>& ref, const std::vector<uint8_t>& out) {
  auto split = [](const std::vector<uint8_t>& v) { std::vector<std::vector<uint8_t>> r; size_t i = 0;
    while (i + 4 <= v.size()) { size_t j = i + 4; while (j + 4 <= v.size() && !(v[j] == 0 && v[j+1] == 0 && v[j+2] == 0 && v[j+3] == 1)) ++j; if (j + 4 > v.size()) j = v.size(); r.emplace_back(v.begin() + i, v.begin() + j); i = j; } return r; };
  auto rn = split(ref), on = split(out);
  for (auto& n : on) { bool found = false; for (auto& r : rn) if (r == n) { found = true; break; } if (!found) return false; }
  return true;
}
static int g_bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); g_bad++; } } while (0)

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: depack_test <annexb.h264>\n"); return 2; }
  const int fd = ::open(argv[1], O_RDONLY); struct stat st{}; ::fstat(fd, &st);
  auto* d = (const uint8_t*)::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  std::string err; auto aus = IndexAccessUnits(d, st.st_size, &err);
  if (aus.size() < 24) { std::fprintf(stderr, "index: %s (%zu AUs; need >= 24)\n", err.c_str(), aus.size()); return 1; }
  AVFormatContext* oc = nullptr; avformat_alloc_output_context2(&oc, nullptr, "rtp", nullptr);
  AVStream* s = avformat_new_stream(oc, nullptr); s->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; s->codecpar->codec_id = AV_CODEC_ID_H264; s->codecpar->width = 640; s->codecpar->height = 360;
  s->time_base = {1, 90000}; oc->packet_size = 1200; oc->flags |= AVFMT_FLAG_CUSTOM_IO;
  oc->pb = avio_alloc_context((uint8_t*)av_malloc(65536), 65536, 1, nullptr, nullptr, &WritePkt, nullptr);
  AVDictionary* o = nullptr; av_dict_set_int(&o, "payload_type", 96, 0); av_dict_set_int(&o, "seq", 65500, 0);   // wraps during the file
  if (avformat_write_header(oc, &o) < 0) return 1;

  const size_t N = aus.size(), LAST = N - 1;
  enum Case { NORMAL, MID_DROP, TAIL_DROP, HEAD_DROP, DUP, REORDER, TWO_EVENTS };
  auto case_of = [&](size_t i) { return i == 5 ? MID_DROP : i == 8 ? TAIL_DROP : i == 12 ? HEAD_DROP : i == 15 ? DUP : i == 18 ? REORDER : i == LAST ? TWO_EVENTS : NORMAL; };

  H264Depacketizer dp; int pkts = 0, events = 0, complete = 0; std::vector<int> got(N + 1, 0);   // events per AU index (N = synthetic)
  uint32_t base = 0; bool base_known = false;   // wire rtp_ts of AU i = base + i*3000 (rtpenc picks a random base)
  auto check_event = [&](const H264Depacketizer::AuEvent& ev, size_t i) {
    events++; got[i]++;
    if (!base_known) { base = ev.rtp_ts - (uint32_t)(i * 3000); base_known = true; }
    CHECK(ev.rtp_ts == base + (uint32_t)(i * 3000), "AU %zu: rtp_ts %u, expected base+%zu", i, ev.rtp_ts, i * 3000);
    CHECK(ev.is_idr == aus[i].is_idr, "AU %zu: idr %d vs %d", i, ev.is_idr, aus[i].is_idr);
    auto ref = Norm(aus[i].data, aus[i].bytes), out = Norm(dp.data(ev.buffer), dp.size(ev.buffer));
    CHECK(OnlyWholeNals(ref, out), "AU %zu: a partial NAL was delivered", i);
    switch (case_of(i)) {
      case NORMAL: case DUP:
        CHECK(ev.complete && ref == out, "AU %zu: expected complete & identical (complete=%d bytes %zu vs %zu)", i, ev.complete, out.size(), ref.size());
        if (ev.complete) complete++;
        break;
      case MID_DROP: case HEAD_DROP: case REORDER:
        CHECK(!ev.complete && ev.lost_packets == 1 && out.size() < ref.size(), "AU %zu: expected incomplete/lost=1/fewer bytes (complete=%d lost=%d %zu vs %zu)", i, ev.complete, ev.lost_packets, out.size(), ref.size());
        break;
      case TAIL_DROP: case TWO_EVENTS:
        CHECK(!ev.complete && ev.lost_packets == 1, "AU %zu: expected incomplete with lost=1 (complete=%d lost=%d)", i, ev.complete, ev.lost_packets);
        break;
    }
  };
  for (size_t i = 0; i < N; ++i) {
    g_pkts.clear();
    AVPacket p; av_init_packet(&p); p.data = const_cast<uint8_t*>(aus[i].data); p.size = aus[i].bytes; p.pts = p.dts = (int64_t)i * 3000; p.stream_index = 0;
    if (av_write_frame(oc, &p) < 0) { std::fprintf(stderr, "write_frame failed at AU %zu\n", i); return 1; }
    pkts += g_pkts.size();
    const size_t np = g_pkts.size(); const Case c = case_of(i);
    CHECK(np >= 3, "AU %zu has only %zu packets; the test stream must fragment every AU", i, np);
    std::vector<size_t> order; for (size_t k = 0; k < np; ++k) order.push_back(k);
    if (c == MID_DROP) order.erase(order.begin() + 1);
    if (c == TAIL_DROP || c == TWO_EVENTS) order.pop_back();
    if (c == HEAD_DROP) order.erase(order.begin());
    if (c == DUP) order.insert(order.begin() + 2, 1);
    if (c == REORDER) std::swap(order[1], order[2]);
    for (size_t k : order) {
      RtpHeader h; CHECK(ParseRtp(g_pkts[k].data(), g_pkts[k].size(), &h), "AU %zu pkt %zu: bad RTP", i, k);
      H264Depacketizer::AuEvent ev[2];
      const int n = dp.Push(h, ev);
      for (int e = 0; e < n; ++e) {
        // an event for the previous AU arrives with this AU's first packet (its ts differs)
        const size_t idx = (base_known && ev[e].rtp_ts != base + (uint32_t)(i * 3000)) ? i - 1 : i;
        if (i == 0 && !base_known) { check_event(ev[e], 0); continue; }
        check_event(ev[e], idx);
      }
    }
  }
  // two events in one Push: the last AU is still open (its marker was dropped); a one-packet AU with the marker
  // ends it and itself. Hand-made RTP: V=2, M=1, PT=96, seq = last+1, ts = base + N*3000, payload = one NAL (type 1).
  {
    uint8_t pkt[12 + 8] = {0x80, 0x80 | 96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0x9a, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    RtpHeader last; ParseRtp(g_pkts.back().data(), g_pkts.back().size(), &last);
    const uint16_t seq = (uint16_t)(last.seq + 1); const uint32_t ts = base + (uint32_t)(N * 3000);
    pkt[2] = seq >> 8; pkt[3] = seq & 0xff; pkt[4] = ts >> 24; pkt[5] = ts >> 16; pkt[6] = ts >> 8; pkt[7] = ts;
    RtpHeader h; CHECK(ParseRtp(pkt, sizeof(pkt), &h), "synthetic packet: bad RTP");
    H264Depacketizer::AuEvent ev[2]; const int n = dp.Push(h, ev);
    CHECK(n == 2, "expected 2 events from one push, got %d", n);
    if (n == 2) {
      check_event(ev[0], LAST);
      events++; got[N]++;
      CHECK(ev[1].complete && ev[1].rtp_ts == ts && ev[1].packets == 1 && ev[1].lost_packets == 0 && dp.size(ev[1].buffer) == 4 + 8, "synthetic AU: complete=%d ts ok=%d pkts=%d lost=%d bytes=%d", ev[1].complete, ev[1].rtp_ts == ts, ev[1].packets, ev[1].lost_packets, dp.size(ev[1].buffer));
      CHECK(ev[0].buffer != ev[1].buffer && dp.size(ev[0].buffer) > 0, "the two events must live in different buffers");
    }
  }
  for (size_t i = 0; i <= N; ++i) CHECK(got[i] == 1, "AU %zu: %d events, expected exactly 1", i, got[i]);
  CHECK(complete == (int)N - 5, "complete=%d, expected %zu (all but MID/TAIL/HEAD/REORDER/TWO_EVENTS)", complete, N - 5);
  CHECK(dp.counters().late_or_dup == 2, "late_or_dup=%lld, expected 2 (duplicate + reordered)", (long long)dp.counters().late_or_dup);
  CHECK(dp.counters().seq_resets == 0, "seq_resets=%lld, expected 0 (wraparound is not a reset)", (long long)dp.counters().seq_resets);
  std::printf("aus=%zu packets=%d events=%d complete=%d cases={mid,tail,head,dup,reorder,two-events,wraparound} bad=%d\n", N, pkts, events, complete, g_bad);
  return g_bad ? 1 : 0;
}
