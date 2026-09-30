// Unit test of the AU indexer + RFC 6184 depacketizer round trip: an Annex B file is indexed into AUs, each AU is
// packetized by libavformat's rtp muxer (the sender's path) into an in-memory list, fed to H264Depacketizer, and the
// rebuilt AU must be byte-identical (start codes normalised to 4 bytes) and carry the same rtp_ts / IDR flag.
// Also: a dropped FU-A fragment marks the AU incomplete without crashing. Run by scripts/build_apps.sh with a
// small test stream produced by x264 on the fly.
#include <cstdio>
#include <cstdlib>
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
int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: depack_test <annexb.h264>\n"); return 2; }
  const int fd = ::open(argv[1], O_RDONLY); struct stat st{}; ::fstat(fd, &st);
  auto* d = (const uint8_t*)::mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  std::string err; auto aus = IndexAccessUnits(d, st.st_size, &err);
  if (aus.empty()) { std::fprintf(stderr, "index: %s\n", err.c_str()); return 1; }
  AVFormatContext* oc = nullptr; avformat_alloc_output_context2(&oc, nullptr, "rtp", nullptr);
  AVStream* s = avformat_new_stream(oc, nullptr); s->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; s->codecpar->codec_id = AV_CODEC_ID_H264; s->codecpar->width = 640; s->codecpar->height = 360;
  s->time_base = {1, 90000}; oc->packet_size = 1200; oc->flags |= AVFMT_FLAG_CUSTOM_IO;
  oc->pb = avio_alloc_context((uint8_t*)av_malloc(65536), 65536, 1, nullptr, nullptr, &WritePkt, nullptr);
  AVDictionary* o = nullptr; av_dict_set_int(&o, "payload_type", 96, 0); av_dict_set_int(&o, "seq", 0, 0);
  if (avformat_write_header(oc, &o) < 0) return 1;
  H264Depacketizer dp; int bad = 0, complete = 0, pkts = 0, dropped_case = 0;
  for (size_t i = 0; i < aus.size(); ++i) {
    g_pkts.clear();
    AVPacket p; av_init_packet(&p); p.data = const_cast<uint8_t*>(aus[i].data); p.size = aus[i].bytes; p.pts = p.dts = (int64_t)i * 3000; p.stream_index = 0;
    if (av_write_frame(oc, &p) < 0) { std::fprintf(stderr, "write_frame failed at AU %zu\n", i); return 1; }
    pkts += g_pkts.size();
    const bool drop = (i == 5 && g_pkts.size() > 2);   // drop a middle fragment of AU 5 once
    for (size_t k = 0; k < g_pkts.size(); ++k) {
      if (drop && k == 1) { dropped_case++; continue; }
      RtpHeader h; if (!ParseRtp(g_pkts[k].data(), g_pkts[k].size(), &h)) { bad++; continue; }
      H264Depacketizer::AuEvent ev;
      if (dp.Push(h, &ev)) {
        if (drop) { if (ev.complete) { std::fprintf(stderr, "AU %zu: expected incomplete\n", i); bad++; } }
        else {
          auto a = Norm(aus[i].data, aus[i].bytes), b = Norm(dp.data(), dp.size());
          if (!ev.complete || a != b || ev.is_idr != aus[i].is_idr || (int)ev.packets != (int)g_pkts.size()) { std::fprintf(stderr, "AU %zu mismatch (complete=%d bytes %zu vs %zu idr %d/%d pkts %d/%zu)\n", i, ev.complete, a.size(), b.size(), ev.is_idr, aus[i].is_idr, ev.packets, g_pkts.size()); bad++; }
          else complete++;
        }
      }
    }
  }
  std::printf("aus=%zu packets=%d round-trip-ok=%d dropped-fragment-case=%d bad=%d\n", aus.size(), pkts, complete, dropped_case, bad);
  return bad ? 1 : 0;
}
