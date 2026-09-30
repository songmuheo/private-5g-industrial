// GStreamer-side helpers shared by the gstreamer sender and receiver:
//   * trace row types written by pad probes (same file names and columns as the webrtc tree so that
//     analysis/ reads both: -tx-encoded, -tx-encoder-rates, -tx/rx-rtp, -tx/rx-rtcp, -rx-decoded),
//   * RTP / RTCP header parsing into those rows (GstRTPBuffer; RTCP by hand, RFC 3550 §6),
//   * the rtp_ts arithmetic of rtph264pay (so the capture thread knows a frame's wire timestamp),
//   * per-frame side information carried across elements as GstReferenceTimestampMeta (rtp_ts, packet
//     count, first/last packet arrival) — untagged metas are copied by GstVideoDecoder onto the
//     decoded frame (gstvideodecoder.c gst_video_decoder_transform_meta_default), so the receiver needs
//     no lookup table between the depayloader and the app sink,
//   * provenance (GStreamer core and plugin versions) for the run's config line.
//
// Streaming-thread rules (rule 2 in CLAUDE.md): the packet-rate paths (RTP/RTCP probes) do no allocation,
// lock or I/O — rows are POD copies into TraceRing. The frame-rate paths add up to 7 small metas per
// frame (gst_buffer_add_reference_timestamp_meta: one GstMeta allocation each, on the jitter-buffer and
// decoder threads). That is bounded (<= 7 x 30/s per stream) and small next to the decoder's own per-frame
// output-buffer allocation (1.4 MB at 720p); the reference caps are created once at start-up
// (InitFrameMetaCaps), never lazily on a streaming thread.
#ifndef P5G_APPS_COMMON_GST_UTIL_H
#define P5G_APPS_COMMON_GST_UTIL_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include <gst/gst.h>
#include <gst/rtp/rtp.h>

#include "app_util.h"
#include "trace_ring.h"

namespace p5g {

// ---- rtp_ts of a frame ---------------------------------------------------------------------------
// rtph264pay (GstRTPBasePayload, gstrtpbasepayload.c gst_rtp_base_payload_prepare_push) sets
//   rtp_ts = timestamp-offset + gst_util_uint64_scale(running_time, clock_rate, GST_SECOND)
// We run the payloader with timestamp-offset=0 and feed appsrc buffers whose PTS is the running time
// (format=time, segment start 0), so the capture thread can compute the exact wire timestamp with the
// same function before the frame is pushed. clock_rate is 90 kHz for video (RFC 6184 §8.2.1).
inline constexpr uint32_t kVideoClockRate = 90000;
inline uint32_t RtpTsFromRunningTime(GstClockTime running_time_ns) {
  return static_cast<uint32_t>(gst_util_uint64_scale(running_time_ns, kVideoClockRate, GST_SECOND));
}

// ---- per-frame side information as metas ---------------------------------------------------------
// GstReferenceTimestampMeta(reference caps, timestamp, duration). One static caps per kind; the value
// travels in `timestamp`. Attached on the depayloader's output (one AU), read at the decoder output
// and the app sink.
enum class FrameMeta { kRtpTs, kNumPackets, kFirstPktMonoNs, kLastPktMonoNs, kDecodeStartMonoNs, kInputBytes, kIsKey, kCount };

inline GstCaps** FrameMetaCapsTable() {
  static GstCaps* caps[static_cast<int>(FrameMeta::kCount)] = {};
  return caps;
}
// Call once after gst_init(), before the pipeline starts: creates the reference caps so the streaming
// threads never allocate them. (Never freed: process lifetime.)
inline void InitFrameMetaCaps() {
  static const char* names[] = {"timestamp/x-p5g-rtpts", "timestamp/x-p5g-npkts", "timestamp/x-p5g-firstpkt",
                                "timestamp/x-p5g-lastpkt", "timestamp/x-p5g-decstart", "timestamp/x-p5g-inbytes",
                                "timestamp/x-p5g-iskey"};
  GstCaps** caps = FrameMetaCapsTable();
  for (int i = 0; i < static_cast<int>(FrameMeta::kCount); ++i)
    if (!caps[i]) caps[i] = gst_caps_new_empty_simple(names[i]);
}
inline GstCaps* FrameMetaCaps(FrameMeta k) { return FrameMetaCapsTable()[static_cast<int>(k)]; }
inline void SetFrameMeta(GstBuffer* b, FrameMeta k, uint64_t value) {
  gst_buffer_add_reference_timestamp_meta(b, FrameMetaCaps(k), static_cast<GstClockTime>(value), GST_CLOCK_TIME_NONE);
}
// Returns false if absent.
inline bool GetFrameMeta(GstBuffer* b, FrameMeta k, uint64_t* value) {
  GstReferenceTimestampMeta* m = gst_buffer_get_reference_timestamp_meta(b, FrameMetaCaps(k));
  if (!m) return false;
  *value = static_cast<uint64_t>(m->timestamp);
  return true;
}
inline int64_t GetFrameMetaOr(GstBuffer* b, FrameMeta k, int64_t def) {
  uint64_t v = 0;
  return GetFrameMeta(b, k, &v) ? static_cast<int64_t>(v) : def;
}

// ---- <stream>-tx-encoded.csv : one row per encoded access unit (x264enc src pad) -----------------
struct EncodedFrameRow {
  uint32_t rtp_ts;
  int64_t encode_done_mono_ns;
  int64_t encode_done_wall_ns;
  int32_t bytes;
  int32_t width, height;
  int32_t frame_type;       // 3 key, 4 delta (numbering kept from the webrtc tree's VideoFrameType)
  int32_t qp;               // -1: x264enc does not expose per-frame QP
  int32_t temporal_idx;     // -1
  int32_t spatial_idx;      // -1
  int32_t simulcast_idx;    // -1
  int64_t capture_time_ms;  // buffer PTS (running time) in ms
  int64_t ntp_time_ms;      // -1
  int32_t codec;            // 4 = H264 (webrtc VideoCodecType numbering, kept for analysis/)
  int32_t is_idr;           // 1 for key frames (x264enc emits IDR at every key frame)
  int32_t at_target_quality;// -1: not reported by x264enc (column kept for the webrtc schema)
};
inline constexpr const char* kEncodedFrameHeader =
    "rtp_ts,encode_done_mono_ns,encode_done_wall_ns,bytes,width,height,frame_type,qp,temporal_idx,"
    "spatial_idx,simulcast_idx,capture_time_ms,ntp_time_ms,codec,is_idr,at_target_quality";
inline void FormatEncodedFrameRow(std::FILE* f, const EncodedFrameRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%lld,%d,%d,%d\n", r.rtp_ts,
               (long long)r.encode_done_mono_ns, (long long)r.encode_done_wall_ns, r.bytes, r.width, r.height,
               r.frame_type, r.qp, r.temporal_idx, r.spatial_idx, r.simulcast_idx, (long long)r.capture_time_ms,
               (long long)r.ntp_time_ms, r.codec, r.is_idr, r.at_target_quality);
}
using EncodedFrameTrace = TraceRing<EncodedFrameRow>;

// ---- <stream>-tx-encoder-rates.csv : one row per bitrate the encoder is told to produce ----------
struct EncoderRateRow {
  int64_t set_mono_ns;
  int64_t set_wall_ns;
  int64_t target_bps;               // x264enc bitrate property x 1000
  int64_t allocated_bps;            // same (no allocator in this stack)
  int64_t bandwidth_allocation_bps; // -1 (no network estimate: the profile decides)
  double framerate_fps;
  int32_t num_active_spatial_layers;
};
inline constexpr const char* kEncoderRateHeader =
    "set_mono_ns,set_wall_ns,target_bps,allocated_bps,bandwidth_allocation_bps,framerate_fps,num_active_spatial_layers";
inline void FormatEncoderRateRow(std::FILE* f, const EncoderRateRow& r) {
  std::fprintf(f, "%lld,%lld,%lld,%lld,%lld,%.3f,%d\n", (long long)r.set_mono_ns, (long long)r.set_wall_ns,
               (long long)r.target_bps, (long long)r.allocated_bps, (long long)r.bandwidth_allocation_bps,
               r.framerate_fps, r.num_active_spatial_layers);
}
using EncoderRateTrace = TraceRing<EncoderRateRow>;

// ---- <stream>-tx-cc.csv : one row per bandwidth estimate (only with --cc gcc) -------------------
// Same header as the webrtc tree's GoogCC ledger so analysis/ reads both; rtpgccbwe exposes only the
// estimate itself, every other column is -1.
struct CcUpdateRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  int64_t target_bps;              // rtpgccbwe estimated-bitrate
};
inline constexpr const char* kCcUpdateHeader =
    "log_mono_ns,log_wall_ns,trigger,target_bps,stable_target_bps,est_bandwidth_bps,rtt_us,"
    "loss_rate_ratio,bwe_period_ms,cwnd_reduce_ratio,pacer_rate_bps,pad_rate_bps,cwnd_bytes,num_probe_clusters";
inline void FormatCcUpdateRow(std::FILE* f, const CcUpdateRow& r) {
  std::fprintf(f, "%lld,%lld,-1,%lld,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1\n", (long long)r.log_mono_ns, (long long)r.log_wall_ns,
               (long long)r.target_bps);
}
using CcUpdateTrace = TraceRing<CcUpdateRow>;

// TWCC header extension (draft-holmer-rmcat-transport-wide-cc-extensions-01), id 1, as in
// gst-examples/webrtc/sendrecv/gst/webrtc-sendrecv.c. Both the payloader (add-extension) and the receiver's
// caps (extmap-1) name it.
inline constexpr const char* kTwccUri = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
inline constexpr int kTwccExtId = 1;

// The Rust plugins (rtpgccbwe) live in <tree>/build/gst-plugins-rs, next to the apps' build directory.
// Registering that path here keeps the binaries self-contained (no GST_PLUGIN_PATH needed on the laptops).
// Returns whether the element is available afterwards.
inline bool EnsureRustPlugins(const char* element) {
  if (GstElementFactory* f = gst_element_factory_find(element)) { gst_object_unref(f); return true; }
  char exe[4096]; const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return false;
  exe[n] = 0;
  std::string dir(exe); dir = dir.substr(0, dir.rfind('/'));          // .../build/apps
  const std::string rs = dir.substr(0, dir.rfind('/')) + "/gst-plugins-rs";  // .../build/gst-plugins-rs
  gst_registry_scan_path(gst_registry_get(), rs.c_str());
  if (GstElementFactory* f = gst_element_factory_find(element)) { gst_object_unref(f); return true; }
  return false;
}

// ---- <stream>-{tx,rx}-rtp.csv : one row per RTP packet ------------------------------------------
struct RtpPacketRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  uint32_t rtp_ts;
  uint32_t ssrc;
  int32_t pkt_bytes;        // header + payload + padding
  int32_t hdr_bytes;        // incl. extensions
  int32_t pad_bytes;
  int32_t payload_bytes;
  int32_t probe_cluster_id; // -1 (no BWE probes in this stack)
  uint16_t seq;
  uint8_t pt;
  uint8_t marker;
  uint8_t csrc_count;
  uint8_t has_ext;
  uint8_t dir;              // 0 out, 1 in
};
inline constexpr const char* kRtpPacketHeader =
    "log_mono_ns,log_wall_ns,dir,seq,rtp_ts,ssrc,pt,marker,pkt_bytes,hdr_bytes,pad_bytes,"
    "payload_bytes,csrc_count,has_ext,probe_cluster_id";
inline void FormatRtpPacketRow(std::FILE* f, const RtpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%d\n", (long long)r.log_mono_ns,
               (long long)r.log_wall_ns, r.dir ? "in" : "out", r.seq, r.rtp_ts, r.ssrc, r.pt, r.marker,
               r.pkt_bytes, r.hdr_bytes, r.pad_bytes, r.payload_bytes, r.csrc_count, r.has_ext, r.probe_cluster_id);
}
using RtpPacketTrace = TraceRing<RtpPacketRow, /*kMultiWriter=*/true>;

// Fills a row from an RTP buffer (RFC 3550 §5.1 header via GstRTPBuffer). Returns false if the buffer
// is not a valid RTP packet (then nothing is written).
inline bool FillRtpRow(GstBuffer* b, uint8_t dir, RtpPacketRow* r) {
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) return false;
  r->log_mono_ns = NowMonoNs();
  r->log_wall_ns = NowWallNs();
  r->rtp_ts = gst_rtp_buffer_get_timestamp(&rtp);
  r->ssrc = gst_rtp_buffer_get_ssrc(&rtp);
  r->pkt_bytes = static_cast<int32_t>(gst_buffer_get_size(b));
  r->hdr_bytes = static_cast<int32_t>(gst_rtp_buffer_get_header_len(&rtp));
  r->payload_bytes = static_cast<int32_t>(gst_rtp_buffer_get_payload_len(&rtp));  // excludes padding
  r->pad_bytes = r->pkt_bytes - r->hdr_bytes - r->payload_bytes;                  // padding octets incl. the count byte
  r->probe_cluster_id = -1;
  r->seq = gst_rtp_buffer_get_seq(&rtp);
  r->pt = gst_rtp_buffer_get_payload_type(&rtp);
  r->marker = gst_rtp_buffer_get_marker(&rtp) ? 1 : 0;
  r->csrc_count = gst_rtp_buffer_get_csrc_count(&rtp);
  r->has_ext = gst_rtp_buffer_get_extension(&rtp) ? 1 : 0;
  r->dir = dir;
  gst_rtp_buffer_unmap(&rtp);
  return true;
}

// ---- <stream>-{tx,rx}-rtcp.csv : one row per compound RTCP packet -------------------------------
inline constexpr int kMaxRtcpParts = 8;
struct RtcpPacketRow {
  int64_t log_mono_ns;
  int64_t log_wall_ns;
  uint32_t sender_ssrc;    // SSRC in the first RTCP header (0 if malformed)
  int32_t bytes;
  uint8_t dir;
  uint8_t num_parts;
  uint8_t pt[kMaxRtcpParts];   // 200 SR, 201 RR, 202 SDES, 203 BYE, 205 RTPFB, 206 PSFB (RFC 3550 §6, RFC 4585 §6)
  uint8_t fmt[kMaxRtcpParts];  // RC / FMT field
};
inline constexpr const char* kRtcpPacketHeader = "log_mono_ns,log_wall_ns,dir,bytes,sender_ssrc,num_parts,parts";
inline void FormatRtcpPacketRow(std::FILE* f, const RtcpPacketRow& r) {
  std::fprintf(f, "%lld,%lld,%s,%d,%u,%u,", (long long)r.log_mono_ns, (long long)r.log_wall_ns,
               r.dir ? "in" : "out", r.bytes, r.sender_ssrc, r.num_parts);
  for (int i = 0; i < r.num_parts && i < kMaxRtcpParts; ++i) std::fprintf(f, "%s%u/%u", i ? ";" : "", r.pt[i], r.fmt[i]);
  std::fputc('\n', f);
}
using RtcpPacketTrace = TraceRing<RtcpPacketRow, /*kMultiWriter=*/true>;

// Walks the compound packet by the length field of each RTCP header (RFC 3550 §6.4.1: length in
// 32-bit words minus one). Tolerant: stops at the first inconsistent header.
inline void FillRtcpRow(GstBuffer* b, uint8_t dir, RtcpPacketRow* r) {
  GstMapInfo map;
  r->log_mono_ns = NowMonoNs();
  r->log_wall_ns = NowWallNs();
  r->dir = dir;
  r->bytes = static_cast<int32_t>(gst_buffer_get_size(b));
  r->sender_ssrc = 0;
  r->num_parts = 0;
  if (!gst_buffer_map(b, &map, GST_MAP_READ)) return;
  size_t off = 0;
  while (off + 4 <= map.size && r->num_parts < kMaxRtcpParts) {
    const uint8_t* p = map.data + off;
    if ((p[0] >> 6) != 2) break;  // version
    const size_t len = (static_cast<size_t>((p[2] << 8) | p[3]) + 1) * 4;
    if (off + len > map.size) break;
    if (r->num_parts == 0 && len >= 8) r->sender_ssrc = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    r->pt[r->num_parts] = p[1];
    r->fmt[r->num_parts] = p[0] & 0x1f;
    r->num_parts++;
    off += len;
  }
  gst_buffer_unmap(b, &map);
}

// Strict validation (unlike FillRtcpRow, which is a tolerant trace parser). RFC 3550 §6.1 / §6.4 /
// §6.5 / §6.6 / §6.7 and RFC 4585 §6.1: every packet has version 2; the lengths tile the buffer exactly;
// padding (P bit) only on the last packet with a count byte that fits; every packet's body is at least
// what its type and count field require:
//   SR  200: exactly 28 + 24 x RC (header, sender info, report blocks; profile extensions rejected)
//   RR  201: exactly  8 + 24 x RC
//   SDES 202: >= 4 + 8 x SC (each chunk: SSRC + at least one item + END, padded to 32 bits)
//   BYE 203: >= 4 + 4 x SC (optional reason follows)
//   APP 204: >= 12 (SSRC + 4-byte name);   RTPFB 205 / PSFB 206: >= 12 (sender + media SSRC)
// and the first packet must be an SR or RR whose SSRC is `ssrc`. Used before letting a packet change
// where RTCP reports are sent, so a spoofed or malformed datagram cannot redirect them.
inline size_t RtcpMinBody(uint8_t pt, unsigned count) {
  switch (pt) {
    case 200: return 28 + 24 * count;
    case 201: return 8 + 24 * count;
    case 202: return 4 + 8 * count;
    case 203: return 4 + 4 * count;
    case 204: case 205: case 206: return 12;
    default: return 0;  // unknown type: invalid
  }
}
inline bool IsValidRtcpCompoundFrom(GstBuffer* b, uint32_t ssrc) {
  GstMapInfo map;
  if (!gst_buffer_map(b, &map, GST_MAP_READ)) return false;
  bool ok = false;
  size_t off = 0;
  int idx = 0;
  while (off + 4 <= map.size) {
    const uint8_t* p = map.data + off;
    if ((p[0] >> 6) != 2) { ok = false; break; }                       // version
    const bool padded = (p[0] & 0x20) != 0;
    const unsigned rc = p[0] & 0x1f;                                   // RC / SC / FMT
    const uint8_t pt = p[1];
    const size_t len = (static_cast<size_t>((p[2] << 8) | p[3]) + 1) * 4;
    if (len < 4 || off + len > map.size) { ok = false; break; }         // must tile the buffer
    if (padded) {                                                      // §6.4.1: only the last packet may be padded
      if (off + len != map.size) { ok = false; break; }
      const uint8_t pad = p[len - 1];
      if (pad == 0 || pad > len - 4) { ok = false; break; }
    }
    const size_t body = padded ? len - p[len - 1] : len;                // length without padding octets
    const size_t min_body = RtcpMinBody(pt, rc);
    if (min_body == 0 || body < min_body) { ok = false; break; }        // type/count-specific minimum, every packet
    if ((pt == 200 || pt == 201) && body != min_body) { ok = false; break; }  // SR/RR sized exactly by RC
    if (idx == 0) {
      if (pt != 200 && pt != 201) { ok = false; break; }                // compound must start with SR/RR (§6.1)
      const uint32_t s = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
      if (s != ssrc) { ok = false; break; }
      ok = true;
    }
    off += len;
    ++idx;
  }
  if (off != map.size) ok = false;  // trailing bytes that are not a packet
  gst_buffer_unmap(b, &map);
  return ok;
}

// ---- <stream>-rx-decoded.csv : one row per frame out of the decoder ------------------------------
struct DecodedFrameLedgerRow {
  uint32_t rtp_ts;
  int64_t decode_start_mono_ns;   // decoder sink pad (frame handed to avdec_h264)
  int64_t decode_done_mono_ns;    // decoder src pad
  int64_t decode_done_wall_ns;
  int32_t input_bytes;
  int32_t frame_type;             // 3 key, 4 delta
  int64_t render_time_ms;         // -1 (no renderer; the app consumes frames as they come)
  int32_t decoder_decode_time_ms; // -1
  int32_t qp;                     // -1
  int32_t width, height;
};
inline constexpr const char* kDecodedFrameLedgerHeader =
    "rtp_ts,decode_start_mono_ns,decode_done_mono_ns,decode_done_wall_ns,input_bytes,frame_type,"
    "render_time_ms,decoder_decode_time_ms,qp,width,height";
inline void FormatDecodedFrameLedgerRow(std::FILE* f, const DecodedFrameLedgerRow& r) {
  std::fprintf(f, "%u,%lld,%lld,%lld,%d,%d,%lld,%d,%d,%d,%d\n", r.rtp_ts, (long long)r.decode_start_mono_ns,
               (long long)r.decode_done_mono_ns, (long long)r.decode_done_wall_ns, r.input_bytes, r.frame_type,
               (long long)r.render_time_ms, r.decoder_decode_time_ms, r.qp, r.width, r.height);
}
using DecodedFrameLedgerTrace = TraceRing<DecodedFrameLedgerRow, /*kMultiWriter=*/true>;

// ---- probe plumbing --------------------------------------------------------------------------------
// A probe on a pad that carries RTP may see single buffers or buffer lists (rtph264pay emits a list
// per access unit when it fragments). `fn(GstBuffer*)` is called for every buffer either way.
template <typename Fn>
inline void ForEachProbeBuffer(GstPadProbeInfo* info, Fn fn) {
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    fn(GST_PAD_PROBE_INFO_BUFFER(info));
  } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    const guint n = gst_buffer_list_length(list);
    for (guint i = 0; i < n; ++i) fn(gst_buffer_list_get(list, i));
  }
}
inline constexpr GstPadProbeType kBufferProbes =
    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST);

// Width/height of the caps currently on a pad (0,0 if unknown). Not for the hot path.
inline void PadResolution(GstPad* pad, int32_t* w, int32_t* h) {
  *w = *h = 0;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) return;
  const GstStructure* s = gst_caps_get_structure(caps, 0);
  gint iw = 0, ih = 0;
  gst_structure_get_int(s, "width", &iw);
  gst_structure_get_int(s, "height", &ih);
  *w = iw; *h = ih;
  gst_caps_unref(caps);
}

// ---- provenance -----------------------------------------------------------------------------------
inline std::string PluginVersion(const char* element_factory) {
  GstElementFactory* f = gst_element_factory_find(element_factory);
  if (!f) return "missing";
  GstPlugin* p = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(f));
  std::string v = p ? gst_plugin_get_version(p) : "?";
  if (p) gst_object_unref(p);
  gst_object_unref(f);
  return v;
}
inline std::string GstProvenance() {
  gchar* core = gst_version_string();
  std::string s = std::string("gstreamer=\"") + core + "\" x264enc=" + PluginVersion("x264enc") +
                  " rtph264pay=" + PluginVersion("rtph264pay") + " rtpbin=" + PluginVersion("rtpbin") +
                  " avdec_h264=" + PluginVersion("avdec_h264") + " appsrc=" + PluginVersion("appsrc");
  g_free(core);
  return s;
}

// Bus messages: handled synchronously on the posting thread (gst_bus_set_sync_handler), because the
// apps run no GLib main loop (the lifecycle loop in app_util.h polls). Errors raise the shutdown flag;
// EOS sets *eos_seen (user_data, may be null) so a sender can drain the encoder before going to NULL;
// everything is dropped so the bus queue cannot grow.
inline GstBusSyncReply BusSyncHandler(GstBus*, GstMessage* msg, gpointer eos_seen) {
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      P5G_LOG_ERROR << "gst error from " << GST_OBJECT_NAME(msg->src) << ": " << err->message << " (" << (dbg ? dbg : "") << ")";
      g_error_free(err); g_free(dbg);
      ShutdownFlag() = 1;
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError* err = nullptr; gchar* dbg = nullptr;
      gst_message_parse_warning(msg, &err, &dbg);
      P5G_LOG_WARN << "gst warning from " << GST_OBJECT_NAME(msg->src) << ": " << err->message << " (" << (dbg ? dbg : "") << ")";
      g_error_free(err); g_free(dbg);
      break;
    }
    case GST_MESSAGE_EOS:
      if (eos_seen) static_cast<std::atomic<bool>*>(eos_seen)->store(true);
      else ShutdownFlag() = 1;
      break;
    default: break;
  }
  gst_message_unref(msg);
  return GST_BUS_DROP;
}

}  // namespace p5g

#endif  // P5G_APPS_COMMON_GST_UTIL_H
