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
//   lost_fragments  : for every file case, exactly 1 if the dropped packet was an FU-A (start, middle or end), else 0
// Synthetic FU-A section (hand-built packets, so the roles are certain, RFC 6184 §5.8): middle fragment lost (one
// discarded NAL, counted once), end lost then new timestamp (partial NAL discarded), start lost (orphans dropped,
// counted once), sequence reset in the middle of an FU (old AU closed incomplete, the end is an orphan), an AU made
// only of orphans (bytes=0 event), a malformed STAP-A (sticky damage), RTP timestamp wrap (0xFFFFF000 -> 0x00000BB8).
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
  std::vector<int> dropped_type(N, -1);   // NAL-unit type byte of the packet each case dropped (28 = FU-A)
  auto check_event = [&](const H264Depacketizer::AuEvent& ev, size_t i) {
    events++; got[i]++;
    if (case_of(i) != NORMAL && case_of(i) != DUP) {
      const int want_frag = dropped_type[i] == 28 ? 1 : 0;
      CHECK(ev.lost_fragments == want_frag, "AU %zu: lost_fragments=%d, expected %d (dropped packet type %d)", i, ev.lost_fragments, want_frag, dropped_type[i]);
    } else {
      CHECK(ev.lost_fragments == 0 && ev.packets == (int)g_pkts.size(), "AU %zu: lost_fragments=%d packets=%d/%zu", i, ev.lost_fragments, ev.packets, g_pkts.size());
    }
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
    auto ptype = [&](size_t k) { return g_pkts[k][12] & 0x1f; };
    if (c == MID_DROP) { dropped_type[i] = ptype(1); order.erase(order.begin() + 1); }
    if (c == TAIL_DROP || c == TWO_EVENTS) { dropped_type[i] = ptype(np - 1); order.pop_back(); }
    if (c == HEAD_DROP) { dropped_type[i] = ptype(0); order.erase(order.begin()); }
    if (c == REORDER) dropped_type[i] = ptype(1);   // the late one is dropped
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
  { int roles = 0; for (size_t i = 0; i < N; ++i) if (dropped_type[i] == 28) roles++; CHECK(roles >= 1, "no file case dropped an FU-A packet; the fixture no longer exercises fragment loss"); }

  // ---- synthetic FU-A section: hand-built packets so every role is certain ----
  {
    H264Depacketizer d2; int64_t late0 = 0;
    uint16_t seq = 0; uint32_t ts = 0xFFFFF000u;   // wraps to 0x00000BB8 after 2 steps of 3000... (ts += 3000 each AU)
    auto mk = [&](std::vector<uint8_t> payload, bool marker, uint16_t sq, uint32_t t) {
      std::vector<uint8_t> p = {0x80, (uint8_t)((marker ? 0x80 : 0) | 96), (uint8_t)(sq >> 8), (uint8_t)sq, (uint8_t)(t >> 24), (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t, 1, 2, 3, 4};
      p.insert(p.end(), payload.begin(), payload.end()); return p; };
    auto push = [&](const std::vector<uint8_t>& p, H264Depacketizer::AuEvent ev[2]) { RtpHeader h; CHECK(ParseRtp(p.data(), (int)p.size(), &h), "synthetic: bad RTP"); return d2.Push(h, ev); };
    // one AU = [single NAL type 1 (4 bytes)] + FU-A of a type-1 NAL split in 3 (S, middle, E), 5 payload bytes each
    const std::vector<uint8_t> single = {0x41, 0xAA, 0xBB, 0xCC};
    auto fu = [](int s, int e, std::vector<uint8_t> frag) { std::vector<uint8_t> p = {0x5c, (uint8_t)((s << 7) | (e << 6) | 1)}; p.insert(p.end(), frag.begin(), frag.end()); return p; };
    const std::vector<uint8_t> f1 = fu(1, 0, {1, 1, 1, 1, 1}), f2 = fu(0, 0, {2, 2, 2, 2, 2}), f3 = fu(0, 1, {3, 3, 3, 3, 3});
    const int whole = 4 + 4 + 4 + 1 + 15;   // start code + single, start code + NAL header + 3 fragments
    H264Depacketizer::AuEvent ev[2]; int n;
    auto au = [&](bool drop1, bool drop2, bool drop3, bool marker_on_last, uint32_t t) {   // sends [single, f1, f2, f3] with drops; seq advances for dropped ones too
      int cnt = 0;
      n = push(mk(single, false, seq++, t), ev); cnt += n;
      if (drop1) seq++; else { n = push(mk(f1, false, seq++, t), ev); cnt += n; }
      if (drop2) seq++; else { n = push(mk(f2, false, seq++, t), ev); cnt += n; }
      if (drop3) seq++; else { n = push(mk(f3, marker_on_last, seq++, t), ev); cnt += n; }
      return cnt; };
    // (a) intact AU, marker
    n = au(false, false, false, true, ts); CHECK(n == 1 && ev[0].complete && ev[0].bytes == whole && ev[0].lost_fragments == 0 && ev[0].rtp_ts == ts, "synthetic a: n=%d complete=%d bytes=%d frag=%d", n, ev[0].complete, ev[0].bytes, ev[0].lost_fragments);
    // (b) middle fragment lost: one NAL discarded, counted exactly once; the single NAL survives
    ts += 3000; n = au(false, true, false, true, ts);
    CHECK(n == 1 && !ev[0].complete && ev[0].lost_packets == 1 && ev[0].lost_fragments == 1 && ev[0].bytes == 8, "synthetic b: n=%d complete=%d lost=%d frag=%d bytes=%d", n, ev[0].complete, ev[0].lost_packets, ev[0].lost_fragments, ev[0].bytes);
    // (c) end lost, then a new timestamp: the open FU is discarded at emission; ts wraps here (0xFFFFF000+6000 -> 0x770)
    ts += 3000; CHECK(ts < 0x1000, "ts should have wrapped (0x%x)", ts);
    n = au(false, false, true, true, ts); CHECK(n == 0, "synthetic c: nothing should be emitted yet (n=%d)", n);
    ts += 3000; n = push(mk(single, true, seq++, ts), ev);   // next AU: its first packet ends the previous one; its own marker ends it -> 2 events
    CHECK(n == 2 && !ev[0].complete && ev[0].lost_packets == 1 && ev[0].lost_fragments == 1 && ev[0].bytes == 8 && ev[0].rtp_ts == ts - 3000
          && ev[1].complete && ev[1].bytes == 8 && ev[1].rtp_ts == ts, "synthetic c: n=%d [0]{complete=%d lost=%d frag=%d bytes=%d} [1]{complete=%d bytes=%d}", n, ev[0].complete, ev[0].lost_packets, ev[0].lost_fragments, ev[0].bytes, ev[1].complete, ev[1].bytes);
    // (d) start lost: the two continuations are orphans, counted once
    ts += 3000; n = au(true, false, false, true, ts);
    CHECK(n == 1 && !ev[0].complete && ev[0].lost_packets == 1 && ev[0].lost_fragments == 1 && ev[0].bytes == 8, "synthetic d: n=%d complete=%d lost=%d frag=%d bytes=%d", n, ev[0].complete, ev[0].lost_packets, ev[0].lost_fragments, ev[0].bytes);
    // (e) sequence reset inside an FU: start at seq s, end at s+4001 (same ts): the old AU closes incomplete at the
    //     reset, the end is an orphan of the fresh AU (never concatenated, never complete)
    ts += 3000; n = push(mk(single, false, seq++, ts), ev); n += push(mk(f1, false, seq++, ts), ev);
    CHECK(n == 0, "synthetic e: premature emission");
    seq = (uint16_t)(seq + 4000); n = push(mk(f3, true, seq++, ts), ev);
    CHECK(n == 2 && !ev[0].complete && ev[0].lost_fragments == 1 && ev[0].bytes == 8 && !ev[1].complete && ev[1].lost_fragments == 1 && ev[1].bytes == 0
          && d2.counters().seq_resets.load() == 1, "synthetic e: n=%d [0]{complete=%d frag=%d bytes=%d} [1]{complete=%d frag=%d bytes=%d} resets=%lld", n, ev[0].complete, ev[0].lost_fragments, ev[0].bytes, ev[1].complete, ev[1].lost_fragments, ev[1].bytes, (long long)d2.counters().seq_resets.load());
    // (e2) two fragmented NALs in one AU, first loses its middle and second loses its start: two NALs discarded,
    //      counted separately (the first orphan run ends at its E fragment); arrival span = of the packets that built it
    ts += 3000; { int c2 = 0; RtpHeader h;
      auto pa = [&](const std::vector<uint8_t>& p, int64_t arr) { CHECK(ParseRtp(p.data(), (int)p.size(), &h), "e2: bad RTP"); return d2.Push(h, ev, arr); };
      c2 += pa(mk(f1, false, seq++, ts), 100); seq++;                  // start, middle lost
      c2 += pa(mk(f3, false, seq++, ts), 200);                         // end: orphan run ends here
      seq++; c2 += pa(mk(f2, false, seq++, ts), 300);                  // second NAL: start lost, middle orphan
      c2 += pa(mk(f3, true, seq++, ts), 400);                          // its end, marker
      CHECK(c2 == 1 && !ev[0].complete && ev[0].lost_fragments == 2 && ev[0].lost_packets == 2 && ev[0].bytes == 0 && ev[0].packets == 4
            && ev[0].first_arrival_ns == 100 && ev[0].last_arrival_ns == 400, "synthetic e2: n=%d frag=%d lost=%d bytes=%d packets=%d arrival %lld..%lld",
            c2, ev[0].lost_fragments, ev[0].lost_packets, ev[0].bytes, ev[0].packets, (long long)ev[0].first_arrival_ns, (long long)ev[0].last_arrival_ns); }
    // (e3) same-timestamp reset: the two events carry their OWN arrival spans and packet counts
    ts += 3000; { int c3 = 0; RtpHeader h;
      auto pa = [&](const std::vector<uint8_t>& p, int64_t arr) { CHECK(ParseRtp(p.data(), (int)p.size(), &h), "e3: bad RTP"); return d2.Push(h, ev, arr); };
      c3 += pa(mk(single, false, seq++, ts), 10); c3 += pa(mk(single, false, seq++, ts), 20);
      seq = (uint16_t)(seq + 4000); c3 += pa(mk(single, true, seq++, ts), 30);
      CHECK(c3 == 2 && ev[0].packets == 2 && ev[0].first_arrival_ns == 10 && ev[0].last_arrival_ns == 20 && ev[1].packets == 1 && ev[1].first_arrival_ns == 30 && ev[1].last_arrival_ns == 30 && ev[1].complete,
            "synthetic e3: n=%d [0]{packets=%d %lld..%lld} [1]{packets=%d %lld..%lld complete=%d}", c3, ev[0].packets, (long long)ev[0].first_arrival_ns, (long long)ev[0].last_arrival_ns, ev[1].packets, (long long)ev[1].first_arrival_ns, (long long)ev[1].last_arrival_ns, ev[1].complete); }
    // (f) an AU of orphans only: bytes=0 event, still reported
    ts += 3000; n = push(mk(f2, false, seq++, ts), ev); n += push(mk(f3, true, seq++, ts), ev);
    CHECK(n == 1 && !ev[0].complete && ev[0].bytes == 0 && ev[0].packets == 2 && ev[0].lost_fragments == 1, "synthetic f: n=%d complete=%d bytes=%d packets=%d frag=%d", n, ev[0].complete, ev[0].bytes, ev[0].packets, ev[0].lost_fragments);
    // (g) malformed STAP-A (declared size beyond the packet): damage is sticky, the marker cannot make it complete
    ts += 3000; n = push(mk({0x58, 0x00, 0x40, 0x41, 0x01}, false, seq++, ts), ev); n += push(mk(single, true, seq++, ts), ev);
    CHECK(n == 1 && !ev[0].complete && ev[0].damaged == 1 && ev[0].lost_packets == 0, "synthetic g: n=%d complete=%d damaged=%d lost=%d", n, ev[0].complete, ev[0].damaged, ev[0].lost_packets);
    // (h) duplicate and late packets are rejected (last_accepted false) and do not touch the AU
    ts += 3000; n = au(false, false, false, false, ts); CHECK(n == 0, "synthetic h: premature emission");
    late0 = d2.counters().late_or_dup.load();
    n = push(mk(f2, false, (uint16_t)(seq - 2), ts), ev); CHECK(n == 0 && !d2.last_accepted(), "synthetic h: late packet must be rejected");
    n = push(mk(single, true, seq++, ts), ev);   // marker: the AU is intact
    CHECK(n == 1 && ev[0].complete && ev[0].packets == 5 && d2.counters().late_or_dup.load() == late0 + 1, "synthetic h: n=%d complete=%d packets=%d late=%lld", n, ev[0].complete, ev[0].packets, (long long)d2.counters().late_or_dup.load());
    // (i) a step of exactly -kMaxMisorder is late (rejected); -kMaxMisorder-1 is a reset
    ts += 3000; n = push(mk(single, true, seq, ts), ev); CHECK(n == 1, "synthetic i: setup");
    n = push(mk(single, true, (uint16_t)(seq - H264Depacketizer::kMaxMisorder), ts), ev); CHECK(n == 0 && !d2.last_accepted(), "synthetic i: -kMaxMisorder must be rejected as late");
    const int64_t r0 = d2.counters().seq_resets.load();
    n = push(mk(single, true, (uint16_t)(seq - H264Depacketizer::kMaxMisorder - 1), ts + 3000), ev); CHECK(d2.last_accepted() && d2.counters().seq_resets.load() == r0 + 1, "synthetic i: -kMaxMisorder-1 must be a reset");
  }
  CHECK(complete == (int)N - 5, "complete=%d, expected %zu (all but MID/TAIL/HEAD/REORDER/TWO_EVENTS)", complete, N - 5);
  CHECK(dp.counters().late_or_dup == 2, "late_or_dup=%lld, expected 2 (duplicate + reordered)", (long long)dp.counters().late_or_dup);
  CHECK(dp.counters().seq_resets == 0, "seq_resets=%lld, expected 0 (wraparound is not a reset)", (long long)dp.counters().seq_resets);
  std::printf("aus=%zu packets=%d events=%d complete=%d file-cases={mid,tail,head,dup,reorder,two-events,seq-wrap} synthetic={fu-mid,fu-end,fu-start,reset,two-nals-lost,same-ts-reset-arrivals,orphans-only,bad-stap,late,misorder-bound,ts-wrap} bad=%d\n", N, pkts, events, complete, g_bad);
  return g_bad ? 1 : 0;
}
