/*
 * p5g gNB tracer — per-event CSV traces for the srsRAN_Project gNB.
 *
 * Installed by scripts/build/build_srsran_gnb.sh into
 *   include/srsran/support/p5g_gnb_tracer.h
 * of the patched source mirror. Call sites are added by the numbered .patch files
 * in patches/srsran_gnb/. Everything here is OBSERVE-ONLY: no scheduler,
 * HARQ, RLC or PDCP decision is changed.
 *
 * Design goals
 *   1. No allocation, lock or file I/O on the gNB real-time threads. A hot-path
 *      call copies one POD row into a fixed circular ring (one CAS to claim a
 *      slot + one release store to publish it) and returns. A single low-priority
 *      background thread formats rows to CSV every P5G_GNB_TRACE_FLUSH_MS
 *      (default 500 ms) and frees the slots, so memory is constant (~4 MB for all traces).
 *   2. Multiple producers per trace. Scheduler cell thread, MAC UL PDU handler,
 *      per-UE RLC/PDCP executors all write concurrently -> claim/publish ring.
 *   3. Overflow is counted, never silently dropped: a footer and a `.ERROR`
 *      sidecar are written if a ring ever overflowed.
 *   4. Two clocks per row: CLOCK_MONOTONIC ns (same clock the WebRTC apps use
 *      when they run on this host -> directly subtractable) and CLOCK_REALTIME
 *      ns (comparable across hosts once they are NTP/PTP-synchronised).
 *
 * Activation: set P5G_GNB_TRACE_DIR=<dir>. If unset, every trace call is a
 * single relaxed atomic load and returns (stock behaviour).
 *
 * Files written (<dir>/gnb_*.csv) — see docs/TRACE_SCHEMA.md
 *   gnb_sched_dl.csv     one row per PDSCH UE grant per slot     (DL resource allocation)
 *   gnb_sched_ul.csv     one row per PUSCH grant per slot        (UL resource allocation)
 *   gnb_ul_crc.csv       one row per PUSCH CRC indication        (UL HARQ outcome + SINR/TA)
 *   gnb_dl_harq_ack.csv  one row per DL HARQ-ACK bit received    (DL HARQ outcome)
 *   gnb_bsr.csv          one row per LCG in each BSR MAC CE      (UE buffer status, bytes)
 *   gnb_sr.csv           one row per detected Scheduling Request
 *   gnb_csi.csv          one row per CSI report (CQI/RI)
 *   gnb_mac_ul_pdu.csv   one row per received MAC UL PDU per UE
 *   gnb_rlc_ul.csv       RLC UL: PDU rx / SDU delivered / t-Reassembly expiry
 *   gnb_pdcp_ul.csv      PDCP UL SDU delivered to core, with IPv4/UDP/RTP parse
 *   gnb_pdcp_dl.csv      PDCP DL SDU received from core, with IPv4/UDP/RTP parse
 */
#ifndef SRSRAN_SUPPORT_P5G_GNB_TRACER_H
#define SRSRAN_SUPPORT_P5G_GNB_TRACER_H

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace p5g {

// ---------------------------------------------------------------------------
// Row types (POD; copied by value into the ring)
// ---------------------------------------------------------------------------
struct trace_clock_stamp {
  int64_t mono_ns;
  int64_t wall_ns;
};

inline trace_clock_stamp trace_now()
{
  timespec m{}, w{};
  ::clock_gettime(CLOCK_MONOTONIC, &m);
  ::clock_gettime(CLOCK_REALTIME, &w);
  return {static_cast<int64_t>(m.tv_sec) * 1000000000LL + m.tv_nsec,
          static_cast<int64_t>(w.tv_sec) * 1000000000LL + w.tv_nsec};
}

// gnb_sched_dl.csv / gnb_sched_ul.csv
struct sched_grant_row {
  trace_clock_stamp t;
  uint32_t sfn;        // slot in which the data channel (PDSCH / PUSCH) is transmitted
  uint32_t slot;       //   "
  uint32_t k_offset;   // DL: k1 (PDSCH -> HARQ-ACK delay); UL: k2 (DCI -> PUSCH delay, DCI slot = slot - k2)
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t harq_id;
  uint32_t new_data;   // 1 = new transmission, 0 = HARQ retransmission
  uint32_t nof_retxs;  // retransmission count so far for this HARQ process
  uint32_t rv;
  uint32_t mcs;
  uint32_t mcs_table;  // 0=qam64, 1=qam256, 2=qam64LowSe (see sch_mcs_table)
  uint32_t rb_start;   // VRB start
  uint32_t rb_count;
  uint32_t sym_start;
  uint32_t sym_count;
  uint32_t nof_layers;
  uint32_t tbs_bytes;
  uint32_t buffer_occupancy;  // DL: pending bytes seen by scheduler; UL: 0
  float    olla_offset;       // outer-loop link adaptation offset (NaN if none)
  uint32_t nof_grants_in_slot; // total UE grants of this direction in the slot (shows contention)
};

// gnb_ul_crc.csv
struct ul_crc_row {
  trace_clock_stamp t;
  uint32_t sfn;
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t harq_id;
  uint32_t crc_ok;
  float    ul_sinr_db;   // NaN if not reported by PHY
  float    ul_rsrp_dbfs; // NaN if not reported
  float    ta_us;        // timing advance offset in microseconds (NaN if not reported)
};

// gnb_dl_harq_ack.csv
struct dl_harq_ack_row {
  trace_clock_stamp t;
  uint32_t sfn;   // UCI slot
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t harq_id;
  int32_t  ack;          // 0=nack 1=ack 2=dtx
  uint32_t tbs_bytes;
  int32_t  status;       // 0=acked 1=nacked 2=no_update 3=error (process-level outcome)
  float    pucch_sinr_db;
};

// gnb_bsr.csv
struct bsr_row {
  trace_clock_stamp t;
  uint32_t sfn;
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t bsr_format;    // 0=short 1=long 2=short_trunc 3=long_trunc
  uint32_t lcg_id;
  uint32_t buffer_size_index; // raw 3GPP TS 38.321 §6.1.3.1 index
  uint32_t buffer_bytes;      // decoded upper bound in bytes (buff_size_field_to_bytes)
};

// gnb_sr.csv
struct sr_row {
  trace_clock_stamp t;
  uint32_t sfn;
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t pucch_format; // 1 = F0/F1, 2 = F2/F3/F4
};

// gnb_csi.csv
struct csi_row {
  trace_clock_stamp t;
  uint32_t sfn;
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  int32_t  cqi;  // -1 if absent
  int32_t  ri;   // -1 if absent
  uint32_t valid;
};

// gnb_mac_ul_pdu.csv
struct mac_ul_pdu_row {
  trace_clock_stamp t;
  uint32_t sfn;
  uint32_t slot;
  uint32_t ue_index;
  uint32_t rnti;
  uint32_t harq_id;
  uint32_t pdu_bytes;
};

// gnb_rlc_ul.csv
enum class rlc_event : uint32_t { pdu_rx = 0, sdu_delivered = 1, reassembly_expired = 2, status_pdu_rx = 3 };
struct rlc_ul_row {
  trace_clock_stamp t;
  uint32_t ue_index;
  uint32_t rb_is_drb;
  uint32_t rb_id;
  uint32_t event;   // rlc_event
  int32_t  sn;      // RLC SN (-1 if n/a)
  uint32_t bytes;   // PDU or SDU length
  uint32_t rx_next;
  uint32_t rx_highest_status;
};

// gnb_pdcp_ul.csv / gnb_pdcp_dl.csv
struct pdcp_sdu_row {
  trace_clock_stamp t;
  uint32_t ue_index;
  uint32_t rb_is_drb;
  uint32_t rb_id;
  uint32_t count;      // PDCP COUNT (UL: delivered count; DL: assigned tx count)
  uint32_t sdu_bytes;
  // Best-effort IPv4/UDP/RTP parse of the SDU (0 when not IPv4/UDP or too short).
  uint32_t ip_proto;
  uint32_t src_ip;
  uint32_t dst_ip;
  uint32_t src_port;
  uint32_t dst_port;
  uint32_t rtp_like;   // 1 if UDP payload starts with RTP version 2
  uint32_t rtp_pt;
  uint32_t rtp_seq;
  uint32_t rtp_ts;
  uint32_t rtp_ssrc;
  uint32_t rtp_marker;
};

// ---------------------------------------------------------------------------
// Lock-free multi-producer circular ring + background CSV writer
// Memory stays at capacity * sizeof(Row) for the whole run; slots are freed as
// soon as the flusher has written them. Producers never block: if the ring is
// full the row is dropped and counted (footer + .ERROR sidecar).
// ---------------------------------------------------------------------------
template <typename Row>
class trace_ring
{
public:
  using formatter_fn = void (*)(std::FILE*, const Row&);

  // capacity is rounded up to a power of two: slot selection is a mask, not a 64-bit division.
  trace_ring(std::string path_, const char* header, formatter_fn fmt_, size_t capacity_) :
    path(std::move(path_)), fmt(fmt_), capacity(round_up_pow2(capacity_)), mask(capacity - 1), slots(new slot[capacity])
  {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "[p5g_gnb_tracer] cannot open %s (%s)\n", path.c_str(), std::strerror(errno));
      std::ofstream(path + ".ERROR") << "open-failed\n";
      return;
    }
    std::fprintf(f, "%s\n", header);
    std::fclose(f);
    enabled = true;
  }

  ~trace_ring() { close(); }

  // Hot path: one CAS + POD copy + release store. No allocation, lock or I/O. Row and ready flag
  // share a slot, so a write touches one cache line (two if the row straddles a boundary).
  void write(const Row& r)
  {
    if (!enabled || closed.load(std::memory_order_acquire)) {
      return;
    }
    size_t i = claimed.load(std::memory_order_relaxed);
    for (;;) {
      if (i - consumed.load(std::memory_order_acquire) >= capacity) { // slot still unflushed
        overflow.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (claimed.compare_exchange_weak(i, i + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        break;
      }
    }
    slot& s = slots[i & mask];
    s.row   = r;
    s.ready.store(1, std::memory_order_release);
  }

  // Background thread only.
  void flush(bool final)
  {
    if (!enabled) {
      return;
    }
    size_t       m   = consumed.load(std::memory_order_relaxed);
    const size_t end = claimed.load(std::memory_order_acquire);
    if (m == end && !final) {
      return;
    }
    std::FILE* f = std::fopen(path.c_str(), "a");
    if (f == nullptr) {
      return;
    }
    for (; m != end && slots[m & mask].ready.load(std::memory_order_acquire); ++m) {
      fmt(f, slots[m & mask].row);
      slots[m & mask].ready.store(0, std::memory_order_release);
      ++written;
    }
    consumed.store(m, std::memory_order_release);
    if (final) {
      const size_t ov = overflow.load(std::memory_order_relaxed);
      std::fprintf(f, "# rows=%zu overflow=%zu clock_domain=%s\n", written, ov, boot_id().c_str());
      if (ov != 0) {
        std::ofstream(path + ".ERROR") << "overflow=" << ov << "\n";
      }
    }
    std::fclose(f);
  }

  void close()
  {
    if (!enabled || closed.exchange(true)) {
      return;
    }
    flush(true);
  }

private:
  struct slot {
    Row                  row;
    std::atomic<uint8_t> ready{0};
  };

  static size_t round_up_pow2(size_t n)
  {
    size_t c = 1;
    while (c < n) {
      c <<= 1;
    }
    return c;
  }

  static std::string boot_id()
  {
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string   v;
    if (f && std::getline(f, v) && !v.empty()) {
      return v;
    }
    return "unknown";
  }

  static constexpr size_t cache_line = 64;
  const std::string       path;
  const formatter_fn      fmt;
  const size_t            capacity;
  const size_t            mask;
  std::unique_ptr<slot[]> slots;
  // Producer counter and flusher counter on separate cache lines so the flusher's periodic
  // consumed store never invalidates the line the real-time threads CAS on.
  alignas(cache_line) std::atomic<size_t> claimed{0};
  alignas(cache_line) std::atomic<size_t> consumed{0};
  alignas(cache_line) std::atomic<size_t> overflow{0};
  std::atomic<bool>                       closed{false};
  size_t                                  written = 0; // flusher-thread private
  bool                                    enabled = false;
};

// ---------------------------------------------------------------------------
// Formatters (background thread only)
// ---------------------------------------------------------------------------
inline void fmt_sched_grant(std::FILE* f, const sched_grant_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%g,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.k_offset,
               r.ue_index,
               r.rnti,
               r.harq_id,
               r.new_data,
               r.nof_retxs,
               r.rv,
               r.mcs,
               r.mcs_table,
               r.rb_start,
               r.rb_count,
               r.sym_start,
               r.sym_count,
               r.nof_layers,
               r.tbs_bytes,
               r.buffer_occupancy,
               r.olla_offset,
               r.nof_grants_in_slot);
}
inline constexpr const char* hdr_sched_grant =
    "mono_ns,wall_ns,sfn,slot,k_offset,ue_index,rnti,harq_id,new_data,nof_retxs,rv,mcs,mcs_table,"
    "rb_start,rb_count,sym_start,sym_count,nof_layers,tbs_bytes,buffer_occupancy,olla_offset,nof_grants_in_slot";

inline void fmt_ul_crc(std::FILE* f, const ul_crc_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%u,%g,%g,%g\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.harq_id,
               r.crc_ok,
               r.ul_sinr_db,
               r.ul_rsrp_dbfs,
               r.ta_us);
}
inline constexpr const char* hdr_ul_crc =
    "mono_ns,wall_ns,sfn,slot,ue_index,rnti,harq_id,crc_ok,ul_sinr_db,ul_rsrp_dbfs,ta_us";

inline void fmt_dl_harq_ack(std::FILE* f, const dl_harq_ack_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%d,%u,%d,%g\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.harq_id,
               r.ack,
               r.tbs_bytes,
               r.status,
               r.pucch_sinr_db);
}
inline constexpr const char* hdr_dl_harq_ack =
    "mono_ns,wall_ns,sfn,slot,ue_index,rnti,harq_id,ack,tbs_bytes,status,pucch_sinr_db";

inline void fmt_bsr(std::FILE* f, const bsr_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%u,%u,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.bsr_format,
               r.lcg_id,
               r.buffer_size_index,
               r.buffer_bytes);
}
inline constexpr const char* hdr_bsr =
    "mono_ns,wall_ns,sfn,slot,ue_index,rnti,bsr_format,lcg_id,buffer_size_index,buffer_bytes";

inline void fmt_sr(std::FILE* f, const sr_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.pucch_format);
}
inline constexpr const char* hdr_sr = "mono_ns,wall_ns,sfn,slot,ue_index,rnti,pucch_format";

inline void fmt_csi(std::FILE* f, const csi_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%d,%d,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.cqi,
               r.ri,
               r.valid);
}
inline constexpr const char* hdr_csi = "mono_ns,wall_ns,sfn,slot,ue_index,rnti,cqi,ri,valid";

inline void fmt_mac_ul_pdu(std::FILE* f, const mac_ul_pdu_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.sfn,
               r.slot,
               r.ue_index,
               r.rnti,
               r.harq_id,
               r.pdu_bytes);
}
inline constexpr const char* hdr_mac_ul_pdu = "mono_ns,wall_ns,sfn,slot,ue_index,rnti,harq_id,pdu_bytes";

inline void fmt_rlc_ul(std::FILE* f, const rlc_ul_row& r)
{
  static const char* const names[] = {"pdu_rx", "sdu_delivered", "reassembly_expired", "status_pdu_rx"};
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%s,%d,%u,%u,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.ue_index,
               r.rb_is_drb,
               r.rb_id,
               r.event < 4 ? names[r.event] : "unknown",
               r.sn,
               r.bytes,
               r.rx_next,
               r.rx_highest_status);
}
inline constexpr const char* hdr_rlc_ul =
    "mono_ns,wall_ns,ue_index,rb_is_drb,rb_id,event,sn,bytes,rx_next,rx_highest_status";

inline void fmt_pdcp_sdu(std::FILE* f, const pdcp_sdu_row& r)
{
  std::fprintf(f,
               "%lld,%lld,%u,%u,%u,%u,%u,%u,%u.%u.%u.%u,%u.%u.%u.%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               (long long)r.t.mono_ns,
               (long long)r.t.wall_ns,
               r.ue_index,
               r.rb_is_drb,
               r.rb_id,
               r.count,
               r.sdu_bytes,
               r.ip_proto,
               (r.src_ip >> 24) & 0xFF,
               (r.src_ip >> 16) & 0xFF,
               (r.src_ip >> 8) & 0xFF,
               r.src_ip & 0xFF,
               (r.dst_ip >> 24) & 0xFF,
               (r.dst_ip >> 16) & 0xFF,
               (r.dst_ip >> 8) & 0xFF,
               r.dst_ip & 0xFF,
               r.src_port,
               r.dst_port,
               r.rtp_like,
               r.rtp_pt,
               r.rtp_seq,
               r.rtp_ts,
               r.rtp_ssrc,
               r.rtp_marker);
}
inline constexpr const char* hdr_pdcp_sdu =
    "mono_ns,wall_ns,ue_index,rb_is_drb,rb_id,count,sdu_bytes,ip_proto,src_ip,dst_ip,src_port,dst_port,"
    "rtp_like,rtp_pt,rtp_seq,rtp_ts,rtp_ssrc,rtp_marker";

// Parse the first bytes of a PDCP SDU (= IP packet) into the RTP fields of the row.
// `head` must contain at least `len` bytes (copy of the SDU start, up to 64 bytes).
inline void parse_ip_udp_rtp(const uint8_t* head, size_t len, pdcp_sdu_row& r)
{
  if (len < 20 || (head[0] >> 4) != 4) {
    return;
  }
  const size_t ihl = static_cast<size_t>(head[0] & 0x0F) * 4;
  if (ihl < 20 || len < ihl) {
    return;
  }
  r.ip_proto = head[9];
  r.src_ip   = (uint32_t(head[12]) << 24) | (uint32_t(head[13]) << 16) | (uint32_t(head[14]) << 8) | head[15];
  r.dst_ip   = (uint32_t(head[16]) << 24) | (uint32_t(head[17]) << 16) | (uint32_t(head[18]) << 8) | head[19];
  if (r.ip_proto != 17 /*UDP*/ || len < ihl + 8) {
    return;
  }
  const uint8_t* udp = head + ihl;
  r.src_port         = (uint32_t(udp[0]) << 8) | udp[1];
  r.dst_port         = (uint32_t(udp[2]) << 8) | udp[3];
  const size_t rtp_off = ihl + 8;
  if (len < rtp_off + 12) {
    return;
  }
  const uint8_t* rtp = head + rtp_off;
  // RFC 3550 §5.1: V=2. Note that SRTP keeps the header in clear, so seq/ts/ssrc
  // are readable even for encrypted WebRTC media. RTCP (PT 200..207) is filtered
  // out because RTCP shares the same UDP port under rtcp-mux (RFC 5761).
  if ((rtp[0] >> 6) != 2) {
    return;
  }
  const uint32_t pt = rtp[1] & 0x7F;
  if (pt >= 64 && pt <= 95) { // RTCP packet types 192..223 collide with this PT range (RFC 5761 §4)
    return;
  }
  r.rtp_like   = 1;
  r.rtp_marker = (rtp[1] >> 7) & 0x01;
  r.rtp_pt     = pt;
  r.rtp_seq    = (uint32_t(rtp[2]) << 8) | rtp[3];
  r.rtp_ts     = (uint32_t(rtp[4]) << 24) | (uint32_t(rtp[5]) << 16) | (uint32_t(rtp[6]) << 8) | rtp[7];
  r.rtp_ssrc   = (uint32_t(rtp[8]) << 24) | (uint32_t(rtp[9]) << 16) | (uint32_t(rtp[10]) << 8) | rtp[11];
}

// ---------------------------------------------------------------------------
// Global tracer singleton
// ---------------------------------------------------------------------------
class gnb_tracer
{
public:
  static gnb_tracer& get()
  {
    static gnb_tracer t;
    return t;
  }

  // Called once from main() before the gNB starts. Reads P5G_GNB_TRACE_DIR.
  void init_from_env()
  {
    const char* dir = std::getenv("P5G_GNB_TRACE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      std::fprintf(stderr, "[p5g_gnb_tracer] P5G_GNB_TRACE_DIR not set — tracing disabled\n");
      return;
    }
    const std::string d = std::string(dir) + "/";
    // Ring depth = rows that can accumulate within one flush period (default 500 ms) with >10x slack
    // for a delayed flusher, sized per event rate so the working set stays small (cache-resident):
    //   packet-rate traces (PDCP/RLC/MAC PDU, ~3k rows/s at 25 Mbps of 1200 B packets)  -> 16384
    //   slot-rate traces   (grants, CRC, HARQ-ACK: <= a few per slot, 2000 slots/s)      -> 16384
    //   report-rate traces (BSR, SR, CSI: tens to hundreds per second)                   ->  4096
    // Total ~4 MB for all eleven rings instead of ~70 MB with a uniform 65536.
    constexpr size_t kDepthPacket = 16384;
    constexpr size_t kDepthReport = 4096;
    sched_dl    = std::make_unique<trace_ring<sched_grant_row>>(d + "gnb_sched_dl.csv", hdr_sched_grant, fmt_sched_grant, kDepthPacket);
    sched_ul    = std::make_unique<trace_ring<sched_grant_row>>(d + "gnb_sched_ul.csv", hdr_sched_grant, fmt_sched_grant, kDepthPacket);
    ul_crc      = std::make_unique<trace_ring<ul_crc_row>>(d + "gnb_ul_crc.csv", hdr_ul_crc, fmt_ul_crc, kDepthPacket);
    dl_harq_ack = std::make_unique<trace_ring<dl_harq_ack_row>>(d + "gnb_dl_harq_ack.csv", hdr_dl_harq_ack, fmt_dl_harq_ack, kDepthPacket);
    bsr         = std::make_unique<trace_ring<bsr_row>>(d + "gnb_bsr.csv", hdr_bsr, fmt_bsr, kDepthReport);
    sr          = std::make_unique<trace_ring<sr_row>>(d + "gnb_sr.csv", hdr_sr, fmt_sr, kDepthReport);
    csi         = std::make_unique<trace_ring<csi_row>>(d + "gnb_csi.csv", hdr_csi, fmt_csi, kDepthReport);
    mac_ul_pdu  = std::make_unique<trace_ring<mac_ul_pdu_row>>(d + "gnb_mac_ul_pdu.csv", hdr_mac_ul_pdu, fmt_mac_ul_pdu, kDepthPacket);
    rlc_ul      = std::make_unique<trace_ring<rlc_ul_row>>(d + "gnb_rlc_ul.csv", hdr_rlc_ul, fmt_rlc_ul, kDepthPacket);
    pdcp_ul     = std::make_unique<trace_ring<pdcp_sdu_row>>(d + "gnb_pdcp_ul.csv", hdr_pdcp_sdu, fmt_pdcp_sdu, kDepthPacket);
    pdcp_dl     = std::make_unique<trace_ring<pdcp_sdu_row>>(d + "gnb_pdcp_dl.csv", hdr_pdcp_sdu, fmt_pdcp_sdu, kDepthPacket);

    int flush_ms = 500;
    if (const char* fm = std::getenv("P5G_GNB_TRACE_FLUSH_MS")) {
      flush_ms = std::max(50, std::atoi(fm));
    }
    running.store(true, std::memory_order_release);
    flusher = std::thread([this, flush_ms] { flush_loop(flush_ms); });
    active.store(true, std::memory_order_release);
    std::fprintf(stderr, "[p5g_gnb_tracer] tracing to %s (flush every %d ms)\n", dir, flush_ms);
  }

  // Called once at shutdown (main() exit path). Idempotent.
  void shutdown()
  {
    if (!active.exchange(false)) {
      return;
    }
    running.store(false, std::memory_order_release);
    if (flusher.joinable()) {
      flusher.join();
    }
    for_each_ring([](auto& r) { r.close(); });
  }

  bool enabled() const { return active.load(std::memory_order_relaxed); }

  // Hot-path emitters. Each checks `enabled()` first (one relaxed load when off).
  void on_sched_dl(const sched_grant_row& r) { if (enabled()) sched_dl->write(r); }
  void on_sched_ul(const sched_grant_row& r) { if (enabled()) sched_ul->write(r); }
  void on_ul_crc(const ul_crc_row& r) { if (enabled()) ul_crc->write(r); }
  void on_dl_harq_ack(const dl_harq_ack_row& r) { if (enabled()) dl_harq_ack->write(r); }
  void on_bsr(const bsr_row& r) { if (enabled()) bsr->write(r); }
  void on_sr(const sr_row& r) { if (enabled()) sr->write(r); }
  void on_csi(const csi_row& r) { if (enabled()) csi->write(r); }
  void on_mac_ul_pdu(const mac_ul_pdu_row& r) { if (enabled()) mac_ul_pdu->write(r); }
  void on_rlc_ul(const rlc_ul_row& r) { if (enabled()) rlc_ul->write(r); }
  void on_pdcp_ul(const pdcp_sdu_row& r) { if (enabled()) pdcp_ul->write(r); }
  void on_pdcp_dl(const pdcp_sdu_row& r) { if (enabled()) pdcp_dl->write(r); }

private:
  gnb_tracer() = default;
  ~gnb_tracer() { shutdown(); }

  template <typename F>
  void for_each_ring(F&& f)
  {
    if (sched_dl) f(*sched_dl);
    if (sched_ul) f(*sched_ul);
    if (ul_crc) f(*ul_crc);
    if (dl_harq_ack) f(*dl_harq_ack);
    if (bsr) f(*bsr);
    if (sr) f(*sr);
    if (csi) f(*csi);
    if (mac_ul_pdu) f(*mac_ul_pdu);
    if (rlc_ul) f(*rlc_ul);
    if (pdcp_ul) f(*pdcp_ul);
    if (pdcp_dl) f(*pdcp_dl);
  }

  void flush_loop(int flush_ms)
  {
    // Lowest nice so the writer never competes with gNB real-time threads.
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
    while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(flush_ms));
      for_each_ring([](auto& r) { r.flush(false); });
    }
  }

  std::unique_ptr<trace_ring<sched_grant_row>> sched_dl, sched_ul;
  std::unique_ptr<trace_ring<ul_crc_row>>      ul_crc;
  std::unique_ptr<trace_ring<dl_harq_ack_row>> dl_harq_ack;
  std::unique_ptr<trace_ring<bsr_row>>         bsr;
  std::unique_ptr<trace_ring<sr_row>>          sr;
  std::unique_ptr<trace_ring<csi_row>>         csi;
  std::unique_ptr<trace_ring<mac_ul_pdu_row>>  mac_ul_pdu;
  std::unique_ptr<trace_ring<rlc_ul_row>>      rlc_ul;
  std::unique_ptr<trace_ring<pdcp_sdu_row>>    pdcp_ul, pdcp_dl;
  std::thread                                  flusher;
  std::atomic<bool>                            running{false};
  std::atomic<bool>                            active{false};
};

} // namespace p5g

#endif // SRSRAN_SUPPORT_P5G_GNB_TRACER_H
