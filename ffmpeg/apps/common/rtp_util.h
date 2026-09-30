// RTP / H.264 helpers of the ffmpeg tree (SMEC-style sender: pre-encoded Annex B H.264 sent as RTP over UDP):
//   * trace row types (same file names and columns as the gstreamer / webrtc trees: -tx-encoded,
//     -tx-encoder-rates, -tx/rx-rtp, -tx/rx-rtcp, -rx-decoded),
//   * RTP fixed-header parsing (RFC 3550 §5.1) and RTCP compound summary (RFC 3550 §6),
//   * an Annex B access-unit indexer (the pre-encoded file is cut into AUs at AUD NALs; per AU: bytes, IDR),
//   * an RFC 6184 depacketizer (single NAL §5.6, STAP-A §5.7.1, FU-A §5.8) that rebuilds Annex B AUs.
//
// Based on: smec-project/edge-applications smec/video-od/client/src/streamer.cpp (docs/reference_code/
// smec-edge-applications, commit b66409c): pre-encoded file -> libavformat "rtp" muxer -> UDP, paced by the
// application. Local modifications: the muxer writes into our AVIO callback (one RTP packet per call) so every
// packet is logged and sent by us; the receiver depacketizes and decodes itself (per-packet kernel arrival
// timestamps, per-frame assembly / decode times); no SEI metadata (frame identity is the RTP timestamp).
//
// Everything used on the send loop / receive loop is allocation-free and lock-free apart from the decoder's
// own frame allocations.
#ifndef P5G_APPS_COMMON_RTP_UTIL_H
#define P5G_APPS_COMMON_RTP_UTIL_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app_util.h"
#include "trace_ring.h"

namespace p5g {

inline constexpr uint32_t kVideoClockRate = 90000;  // RFC 6184 §8.2.1

// ---- <stream>-tx-encoded.csv : one row per access unit handed to the RTP muxer -------------------
struct EncodedFrameRow {
  uint32_t rtp_ts;
  int64_t encode_done_mono_ns;   // here: the moment the AU was handed to the muxer (no encoder in the loop)
  int64_t encode_done_wall_ns;
  int32_t bytes;                 // Annex B bytes of the AU (incl. AUD/SPS/PPS)
  int32_t width, height;         // from the SPS of the file (parsed once)
  int32_t frame_type;            // 3 key (IDR), 4 delta
  int32_t qp;                    // -1
  int32_t temporal_idx, spatial_idx, simulcast_idx;  // -1
  int64_t capture_time_ms;       // pts in ms (grid time)
  int64_t ntp_time_ms;           // -1
  int32_t codec;                 // 4 = H264
  int32_t is_idr;
  int32_t at_target_quality;     // -1
};
inline constexpr const char* kEncodedFrameHeader =
    "rtp_ts,encode_done_mono_ns,encode_done_wall_ns,bytes,width,height,frame_type,qp,temporal_idx,"
    "spatial_idx,simulcast_idx,capture_time_ms,ntp_time_ms,codec,is_idr,at_target_quality";
inline void FormatEncodedFrameRow(std::FILE* f, const EncodedFrameRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%d,%d,%d\n", r.rtp_ts, (long long)r.encode_done_mono_ns,
               (long long)r.encode_done_wall_ns, r.bytes, r.width, r.height, r.frame_type, r.qp, r.temporal_idx, r.spatial_idx,
               r.simulcast_idx, (long long)r.capture_time_ms, (long long)r.ntp_time_ms, r.codec, r.is_idr, r.at_target_quality);
}
using EncodedFrameTrace = TraceRing<EncodedFrameRow>;

// ---- <stream>-tx-encoder-rates.csv : one row per ladder rung in use (start + every switch) -------
struct EncoderRateRow {
  int64_t set_mono_ns, set_wall_ns;
  int64_t target_bps;               // the rung's nominal bitrate
  int64_t allocated_bps;            // same
  int64_t bandwidth_allocation_bps; // -1
  double framerate_fps;
  int32_t num_active_spatial_layers;
};
inline constexpr const char* kEncoderRateHeader =
    "set_mono_ns,set_wall_ns,target_bps,allocated_bps,bandwidth_allocation_bps,framerate_fps,num_active_spatial_layers";
inline void FormatEncoderRateRow(std::FILE* f, const EncoderRateRow& r) {
  std::fprintf(f, "%lld,%lld,%lld,%lld,%lld,%.3f,%d\n", (long long)r.set_mono_ns, (long long)r.set_wall_ns, (long long)r.target_bps,
               (long long)r.allocated_bps, (long long)r.bandwidth_allocation_bps, r.framerate_fps, r.num_active_spatial_layers);
}
using EncoderRateTrace = TraceRing<EncoderRateRow>;

// ---- <stream>-{tx,rx}-rtp.csv : one row per RTP packet ------------------------------------------
struct RtpPacketRow {
  int64_t log_mono_ns, log_wall_ns;
  uint32_t rtp_ts, ssrc;
  int32_t pkt_bytes, hdr_bytes, pad_bytes, payload_bytes;
  int32_t probe_cluster_id;  // -1
  uint16_t seq;
  uint8_t pt, marker, csrc_count, has_ext, dir;  // dir 0 out, 1 in
};
inline constexpr const char* kRtpPacketHeader =
    "log_mono_ns,log_wall_ns,dir,seq,rtp_ts,ssrc,pt,marker,pkt_bytes,hdr_bytes,pad_bytes,payload_bytes,csrc_count,has_ext,probe_cluster_id";
inline void FormatRtpPacketRow(std::FILE* f, const RtpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%d\n", (long long)r.log_mono_ns, (long long)r.log_wall_ns,
               r.dir ? "in" : "out", r.seq, r.rtp_ts, r.ssrc, r.pt, r.marker, r.pkt_bytes, r.hdr_bytes, r.pad_bytes, r.payload_bytes,
               r.csrc_count, r.has_ext, r.probe_cluster_id);
}
using RtpPacketTrace = TraceRing<RtpPacketRow, true>;

// Parsed RTP fixed header (RFC 3550 §5.1). Returns false unless version 2 and the header fits.
struct RtpHeader {
  uint8_t pt = 0, marker = 0, cc = 0, x = 0, p = 0;
  uint16_t seq = 0;
  uint32_t ts = 0, ssrc = 0;
  int hdr_len = 0, pad_len = 0, payload_len = 0;
  const uint8_t* payload = nullptr;
};
inline bool ParseRtp(const uint8_t* d, int n, RtpHeader* h) {
  if (n < 12 || (d[0] >> 6) != 2) return false;
  h->p = (d[0] >> 5) & 1; h->x = (d[0] >> 4) & 1; h->cc = d[0] & 0x0f;
  h->marker = d[1] >> 7; h->pt = d[1] & 0x7f;
  h->seq = (uint16_t)(d[2] << 8 | d[3]);
  h->ts = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
  h->ssrc = (uint32_t)d[8] << 24 | (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
  int len = 12 + 4 * h->cc;
  if (h->x) {
    if (n < len + 4) return false;
    len += 4 + 4 * ((d[len + 2] << 8) | d[len + 3]);
  }
  if (len > n) return false;
  h->hdr_len = len;
  h->pad_len = h->p ? d[n - 1] : 0;
  if (h->pad_len > n - len) return false;
  h->payload = d + len;
  h->payload_len = n - len - h->pad_len;
  return true;
}
inline void FillRtpRow(const uint8_t* d, int n, const RtpHeader& h, uint8_t dir, int64_t mono, int64_t wall, RtpPacketRow* r) {
  r->log_mono_ns = mono; r->log_wall_ns = wall;
  r->rtp_ts = h.ts; r->ssrc = h.ssrc; r->pkt_bytes = n; r->hdr_bytes = h.hdr_len; r->pad_bytes = h.pad_len;
  r->payload_bytes = h.payload_len; r->probe_cluster_id = -1; r->seq = h.seq; r->pt = h.pt; r->marker = h.marker;
  r->csrc_count = h.cc; r->has_ext = h.x; r->dir = dir;
  (void)d;
}

// ---- <stream>-{tx,rx}-rtcp.csv : one row per compound RTCP packet -------------------------------
inline constexpr int kMaxRtcpParts = 8;
struct RtcpPacketRow {
  int64_t log_mono_ns, log_wall_ns;
  uint32_t sender_ssrc;
  int32_t bytes;
  uint8_t dir, num_parts;
  uint8_t pt[kMaxRtcpParts], fmt[kMaxRtcpParts];
};
inline constexpr const char* kRtcpPacketHeader = "log_mono_ns,log_wall_ns,dir,bytes,sender_ssrc,num_parts,parts";
inline void FormatRtcpPacketRow(std::FILE* f, const RtcpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%d,%u,%u,", (long long)r.log_mono_ns, (long long)r.log_wall_ns, r.dir ? "in" : "out", r.bytes,
               r.sender_ssrc, r.num_parts);
  for (int i = 0; i < r.num_parts && i < kMaxRtcpParts; ++i) std::fprintf(f, "%s%u/%u", i ? ";" : "", r.pt[i], r.fmt[i]);
  std::fputc('\n', f);
}
using RtcpPacketTrace = TraceRing<RtcpPacketRow, true>;
inline bool IsRtcp(const uint8_t* d, int n) { return n >= 4 && (d[0] >> 6) == 2 && d[1] >= 200 && d[1] <= 206; }
inline void FillRtcpRow(const uint8_t* d, int n, uint8_t dir, int64_t mono, int64_t wall, RtcpPacketRow* r) {
  r->log_mono_ns = mono; r->log_wall_ns = wall; r->dir = dir; r->bytes = n; r->sender_ssrc = 0; r->num_parts = 0;
  int off = 0;
  while (off + 4 <= n && r->num_parts < kMaxRtcpParts) {
    const uint8_t* p = d + off;
    if ((p[0] >> 6) != 2) break;
    const int len = (((p[2] << 8) | p[3]) + 1) * 4;
    if (off + len > n) break;
    if (r->num_parts == 0 && len >= 8) r->sender_ssrc = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    r->pt[r->num_parts] = p[1]; r->fmt[r->num_parts] = p[0] & 0x1f; r->num_parts++;
    off += len;
  }
}

// ---- <stream>-rx-decoded.csv : one row per frame through the decoder -----------------------------
struct DecodedFrameLedgerRow {
  uint32_t rtp_ts;
  int64_t decode_start_mono_ns, decode_done_mono_ns, decode_done_wall_ns;
  int32_t input_bytes, frame_type;
  int64_t render_time_ms;         // -1
  int32_t decoder_decode_time_ms; // -1
  int32_t qp;                     // -1
  int32_t width, height;
};
inline constexpr const char* kDecodedFrameLedgerHeader =
    "rtp_ts,decode_start_mono_ns,decode_done_mono_ns,decode_done_wall_ns,input_bytes,frame_type,render_time_ms,decoder_decode_time_ms,qp,width,height";
inline void FormatDecodedFrameLedgerRow(std::FILE* f, const DecodedFrameLedgerRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%lld,%d,%d,%lld,%d,%d,%d,%d\n", r.rtp_ts, (long long)r.decode_start_mono_ns, (long long)r.decode_done_mono_ns,
               (long long)r.decode_done_wall_ns, r.input_bytes, r.frame_type, (long long)r.render_time_ms, r.decoder_decode_time_ms, r.qp,
               r.width, r.height);
}
using DecodedFrameLedgerTrace = TraceRing<DecodedFrameLedgerRow>;

// ---- Annex B access units of a pre-encoded file ---------------------------------------------------
// The file must be cut at Access Unit Delimiters (NAL type 9, x264 `aud=1`), one AU per frame, SPS/PPS
// repeated on every IDR (x264 `repeat-headers=1`), fixed GOP (`keyint=min-keyint`, `scenecut=0`):
// ffmpeg/scripts/prepare_video.sh produces exactly that. AUs are referenced in place (mmap).
struct AccessUnit {
  const uint8_t* data;
  int32_t bytes;
  int32_t is_idr;   // contains a NAL of type 5
};
inline int NalType(const uint8_t* p) { return p[0] & 0x1f; }
// Finds the next start code (00 00 01 / 00 00 00 01) at or after `from`; returns offset of the first byte after it,
// or -1. A start code that ends the buffer (no NAL header byte after it) is treated as "none": a truncated file's
// tail must not be dereferenced (was an out-of-bounds read).
inline int64_t NextStartCode(const uint8_t* d, int64_t n, int64_t from, int64_t* sc_begin) {
  for (int64_t i = from; i + 3 <= n; ++i) {
    if (d[i] == 0 && d[i + 1] == 0) {
      if (d[i + 2] == 1) { if (i + 3 >= n) return -1; *sc_begin = i; return i + 3; }
      if (i + 4 <= n && d[i + 2] == 0 && d[i + 3] == 1) { if (i + 4 >= n) return -1; *sc_begin = i; return i + 4; }
    }
  }
  return -1;
}
// Splits an Annex B stream into AUs at AUD NALs. Also reports the SPS-declared resolution via the
// caller's decoder (we do not parse SPS here; width/height are filled by the sender after probing).
inline std::vector<AccessUnit> IndexAccessUnits(const uint8_t* d, int64_t n, std::string* err) {
  std::vector<AccessUnit> aus;
  int64_t sc = 0, pos = NextStartCode(d, n, 0, &sc);
  if (pos < 0) { *err = "no start code"; return aus; }
  int64_t au_begin = -1; int idr = 0; int nals = 0;
  while (pos >= 0) {
    const int type = NalType(d + pos);
    if (type == 9) {  // AUD: closes the previous AU, opens a new one
      if (au_begin >= 0) aus.push_back({d + au_begin, (int32_t)(sc - au_begin), idr});
      au_begin = sc; idr = 0;
    } else if (au_begin < 0) {
      *err = "stream does not start with an AUD (encode with x264 aud=1)"; return {};
    }
    if (type == 5) idr = 1;
    ++nals;
    int64_t next_sc = 0; const int64_t next = NextStartCode(d, n, pos, &next_sc);
    if (next < 0) break;
    sc = next_sc; pos = next;
  }
  if (au_begin >= 0) aus.push_back({d + au_begin, (int32_t)(n - au_begin), idr});
  if (aus.empty()) *err = "no access units";
  return aus;
}

// ---- RFC 6184 depacketizer --------------------------------------------------------------------------
// Rebuilds Annex B access units from RTP payloads: single NAL unit packets (§5.6), STAP-A (§5.7.1),
// FU-A (§5.8). An AU ends at the marker bit (§5.1) or when a packet with a new timestamp arrives.
//
// Output contract: Push() returns the number of AUs that ended with this packet, 0..2 (2 = the previous AU
// ended by the timestamp change AND this packet carries the marker of a one-packet AU). Each ended AU has
// its own output buffer: three fixed buffers rotate (assembly + up to two emitted), so an emitted AU is
// never overwritten by the packet that ended it; data(k)/size(k) stay valid until the next Push().
//
// `complete` = ended by the marker AND no sequence gap attributed to it AND no damage (an FU-A left open, a
// malformed STAP-A, an AU larger than the buffer). Gap attribution: a gap seen at a packet that changes the
// timestamp while the previous AU is still open (its marker never arrived) is that previous AU's lost tail;
// a gap seen after a marker-ended AU, or inside an AU, belongs to the AU being assembled. (A tail+head loss
// spanning two AUs is attributed to the first; only one of them is then marked incomplete — a known limit.)
// A partial NAL (FU-A whose end or later fragment was lost) is discarded, not delivered (§5.8 permits either;
// a half NAL only makes the decoder produce garbage). FU-A fragments without an open FU (start lost) are
// dropped and counted once per run as a lost fragment.
//
// Sequence numbers follow RFC 3550 Appendix A.1: a forward step of at most kMaxDropout is progress (gap =
// step-1 counted as lost), a backward step of at most kMaxMisorder is a duplicate/late packet (dropped and
// counted, assembly state untouched), anything else is a sequence reset (accepted, counted, no loss).
// AUs whose every byte was discarded are still reported (bytes=0, complete=false) so no loss goes uncounted.
class H264Depacketizer {
 public:
  static constexpr int kBuffers = 3;
  static constexpr int kMaxDropout = 3000;   // RFC 3550 A.1
  static constexpr int kMaxMisorder = 100;   // RFC 3550 A.1
  // Buffers may be supplied by the caller (e.g. libavcodec-padded, reference-counted memory so the decoder
  // takes the AU without a copy); otherwise they are allocated once here.
  explicit H264Depacketizer(size_t max_au_bytes = 4 << 20) : cap_((int)max_au_bytes) {
    for (int k = 0; k < kBuffers; ++k) { own_[k].resize(max_au_bytes); buf_[k] = own_[k].data(); }
  }
  H264Depacketizer(uint8_t* const bufs[kBuffers], size_t cap) : cap_((int)cap) { for (int k = 0; k < kBuffers; ++k) buf_[k] = bufs[k]; }

  struct AuEvent {
    bool complete = false; uint32_t rtp_ts = 0; int bytes = 0; int packets = 0; int lost_packets = 0; int lost_fragments = 0; int is_idr = 0;
    int damaged = 0;   // malformed STAP-A / buffer overflow seen in this AU
    int buffer = 0;    // index for data(k)/size(k)
  };
  struct Counters { int64_t late_or_dup = 0, seq_resets = 0; };

  // Feed one RTP packet. Returns the number of AUs that ended (0..2), filled into out[0..n-1] in order.
  int Push(const RtpHeader& h, AuEvent out[2]) {
    int n_out = 0;
    if (have_seq_) {
      const int16_t step = (int16_t)(uint16_t)(h.seq - last_seq_);
      if (step <= 0 && step > -kMaxMisorder) { counters_.late_or_dup++; return 0; }        // duplicate or late: ignore
      if (step > 0 && step <= kMaxDropout) { pending_gap_ = step - 1; }
      else { counters_.seq_resets++; pending_gap_ = 0; }                                    // far jump: sequence reset
    }
    have_seq_ = true; last_seq_ = h.seq;
    if (have_ts_ && h.ts != cur_ts_) {                       // timestamp change while an AU is open: it lost its marker packet(s)
      if (pending_gap_) { lost_ += pending_gap_; pending_gap_ = 0; }   // -> the gap is that AU's lost tail, not this one's head
      if (Emit(&out[n_out], false)) n_out++;
    }
    if (!have_ts_ || h.ts != cur_ts_) Reset(h.ts);
    if (pending_gap_) { lost_ += pending_gap_; pending_gap_ = 0; if (fu_open_) { lost_frag_++; fu_open_ = false; DropOpenFu(); } }   // within this AU (or after a marker-ended one: this AU's head)
    packets_++;
    const uint8_t* p = h.payload; const int n = h.payload_len;
    if (n >= 1) {
      const int type = p[0] & 0x1f;
      if (type >= 1 && type <= 23) {            // single NAL unit
        AppendNal(p, n);
      } else if (type == 24) {                  // STAP-A: 1 byte header, then (2-byte size, NAL)*
        int off = 1;
        while (off < n) {
          if (off + 2 > n) { damaged_ = 1; break; }
          const int sz = (p[off] << 8) | p[off + 1]; off += 2;
          if (sz <= 0 || off + sz > n) { damaged_ = 1; break; }
          AppendNal(p + off, sz); off += sz;
        }
      } else if (type == 28 && n >= 2) {        // FU-A: FU indicator, FU header (S|E|R|type), fragment
        const uint8_t fu = p[1]; const int s = fu >> 7, e = (fu >> 6) & 1, ntype = fu & 0x1f;
        if (s) {
          if (fu_open_) { lost_frag_++; DropOpenFu(); }   // a new start while one is open: the previous end was lost
          const uint8_t nal_hdr = (uint8_t)((p[0] & 0xe0) | ntype);
          fu_start_ = len_; orphan_ = false;
          AppendStartCode(); Append(&nal_hdr, 1); fu_open_ = true;
          if (ntype == 5) idr_ = 1;
        }
        if (fu_open_) { Append(p + 2, n - 2); if (e) fu_open_ = false; }
        else if (!orphan_) { orphan_ = true; lost_frag_++; }   // continuation without a start: that NAL is lost
      } else {
        damaged_ = 1;                            // STAP-B / MTAP / FU-B (RFC 6184 §5.2 non-interleaved mode never sends them)
      }
    }
    if (h.marker) { if (Emit(&out[n_out], true)) n_out++; Reset(cur_ts_); have_ts_ = false; }   // next packet opens a new AU whatever its ts
    return n_out;
  }
  const uint8_t* data(int k) const { return buf_[k]; }
  int size(int k) const { return emitted_len_[k]; }
  const Counters& counters() const { return counters_; }

 private:
  void Reset(uint32_t ts) { have_ts_ = true; cur_ts_ = ts; len_ = 0; packets_ = 0; lost_ = 0; lost_frag_ = 0; idr_ = 0; damaged_ = 0; fu_open_ = false; orphan_ = false; }
  bool Emit(AuEvent* out, bool by_marker) {
    if (fu_open_) { lost_frag_++; fu_open_ = false; DropOpenFu(); }   // partial NAL: discarded, counted
    if (len_ == 0 && packets_ == 0 && lost_ == 0) return false;        // nothing happened under this timestamp
    out->complete = by_marker && lost_ == 0 && lost_frag_ == 0 && !damaged_ && len_ > 0;
    out->rtp_ts = cur_ts_; out->bytes = len_; out->packets = packets_; out->lost_packets = lost_; out->lost_fragments = lost_frag_;
    out->is_idr = idr_; out->damaged = damaged_; out->buffer = cur_;
    emitted_len_[cur_] = len_;
    cur_ = (cur_ + 1) % kBuffers;   // the emitted buffer stays untouched for the next two emissions
    len_ = 0;
    return true;
  }
  void AppendStartCode() { static const uint8_t sc[4] = {0, 0, 0, 1}; Append(sc, 4); }
  void AppendNal(const uint8_t* p, int n) { AppendStartCode(); Append(p, n); if ((p[0] & 0x1f) == 5) idr_ = 1; }
  void Append(const uint8_t* p, int n) { if (len_ + n <= cap_) { std::memcpy(buf_[cur_] + len_, p, n); len_ += n; } else damaged_ = 1; }
  void DropOpenFu() { len_ = fu_start_; }  // discard the partial NAL

  std::vector<uint8_t> own_[kBuffers];
  uint8_t* buf_[kBuffers] = {nullptr, nullptr, nullptr};
  int cap_ = 0, cur_ = 0;
  int emitted_len_[kBuffers] = {0, 0, 0};
  int len_ = 0, packets_ = 0, lost_ = 0, lost_frag_ = 0, idr_ = 0, fu_start_ = 0, damaged_ = 0, pending_gap_ = 0;
  uint32_t cur_ts_ = 0; uint16_t last_seq_ = 0;
  bool have_ts_ = false, have_seq_ = false, fu_open_ = false, orphan_ = false;
  Counters counters_;
};

}  // namespace p5g

#endif  // P5G_APPS_COMMON_RTP_UTIL_H
