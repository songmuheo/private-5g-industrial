// Unit test of IsValidRtcpCompoundFrom (gst_util.h): crafted RTCP compounds, each with the expected verdict.
// Built and run by scripts/build_apps.sh (a wrong verdict fails the build). The validator gates where the
// receiver sends its RTCP reports, so it must reject anything a spoofed datagram could carry.
#include <gst/gst.h>
#include <cstdio>
#include <vector>
#include "gst_util.h"
static GstBuffer* mk(const std::vector<uint8_t>& v){ GstBuffer* b=gst_buffer_new_allocate(nullptr,v.size(),nullptr); gst_buffer_fill(b,0,v.data(),v.size()); return b; }
static std::vector<uint8_t> hdr(uint8_t rc,uint8_t pt,size_t bytes,bool pad=false){ uint16_t w=bytes/4-1; return {uint8_t(0x80|(pad?0x20:0)|rc),pt,uint8_t(w>>8),uint8_t(w&0xff)}; }
static void ssrc(std::vector<uint8_t>& v,uint32_t s){ v.push_back(s>>24); v.push_back(s>>16); v.push_back(s>>8); v.push_back(s); }
int main(){ gst_init(nullptr,nullptr); const uint32_t S=0x11223344; struct C{const char* n; std::vector<uint8_t> v; bool want;}; std::vector<C> cs;
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); cs.push_back({"SR rc=0 28B",v,true}); }
  { auto v=hdr(1,201,32); ssrc(v,S); v.resize(32,0); cs.push_back({"RR rc=1 32B",v,true}); }
  { auto v=hdr(1,201,8); ssrc(v,S); cs.push_back({"RR rc=1 but 8B (block missing)",v,false}); }
  { auto v=hdr(0,200,8); ssrc(v,S); cs.push_back({"SR 8B no sender info",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S^1); v.resize(28,0); cs.push_back({"SR wrong ssrc",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); v.push_back(0); cs.push_back({"SR + 1 trailing byte",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(1,202,12); ssrc(t,S); t.resize(12,0); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + SDES compound",v,true}); }
  { auto v=hdr(0,200,32,true); ssrc(v,S); v.resize(32,0); v[31]=4; cs.push_back({"SR padded (4) last",v,true}); }
  { auto v=hdr(0,200,32,true); ssrc(v,S); v.resize(32,0); v[31]=4; auto t=hdr(0,202,4); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"padded SR not last",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(0,96,4); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + pt 96 (not RTCP)",v,false}); }
  { auto v=hdr(0,201,8); ssrc(v,S); v[0]=0x40; cs.push_back({"RR version 1",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(0,200,4); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + bare 4B SR header",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(1,202,4); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + SDES sc=1 but 4B",v,false}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(1,203,8); ssrc(t,S); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + BYE sc=1 8B",v,true}); }
  { auto v=hdr(0,200,28); ssrc(v,S); v.resize(28,0); auto t=hdr(1,205,8); ssrc(t,S); v.insert(v.end(),t.begin(),t.end()); cs.push_back({"SR + RTPFB 8B (needs 12)",v,false}); }
  { auto v=hdr(1,202,12); ssrc(v,S); v.resize(12,0); cs.push_back({"SDES first (not SR/RR)",v,false}); }
  { auto v=hdr(1,200,52); ssrc(v,S); v.resize(52,0); cs.push_back({"SR rc=1 52B",v,true}); }
  { auto v=hdr(1,200,56); ssrc(v,S); v.resize(56,0); cs.push_back({"SR rc=1 56B (extension)",v,false}); }
  int bad=0; for(auto& c:cs){ GstBuffer* b=mk(c.v); bool got=p5g::IsValidRtcpCompoundFrom(b,S); gst_buffer_unref(b); std::printf("%-34s -> %s%s\n",c.n,got?"valid  ":"invalid",got==c.want?"":"  <-- UNEXPECTED"); bad+=got!=c.want; }
  std::printf("%d unexpected\n",bad); return bad; }
