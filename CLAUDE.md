# CLAUDE.md — project rules

Global rules: `~/.claude/CLAUDE.md`. These take precedence where they overlap.

Private-5G (srsRAN gNB + Open5GS + USRP B210 + Pixel UEs) video-transmission testbed with real-time
cross-layer tracing. Roles: **gNB PC** (`ran/`, `patches/srsran_gnb`, `scripts/run/`), **UE laptop**
(`apps/sender`), **internet host** (`apps/receiver`, `apps/signaling`). Raw logs go to `results/<run>/`.

1. **Third-party stays stock.** `third_party/*` and the libwebrtc checkout are never edited; changes
   are build-time patches under `patches/<component>/` applied to a copy by `scripts/build/`.
   `git -C third_party/<x> status --porcelain` must be empty.
2. **Pure behaviour.** Hooks only observe; they never change scheduler, HARQ, RLC, PDCP or libwebrtc
   decisions, and must not change timing: no allocation / lock / I/O on media or RAN threads
   (`apps/common/trace_ring.h`, `patches/srsran_gnb/p5g_gnb_tracer.h`: fixed circular rings,
   background flusher at nice 19, overflow counted into `.ERROR`). Anything that adds load
   (pcaps, extra logs) is opt-in and off by default.
3. **Real-time logging only.** Every file in a run is written by the running process. No derived
   files; analysis happens later, outside this repo's run path.
4. **Follow the framework examples** (`examples/peerconnection/client/conductor.cc`,
   `test/test_video_capturer.cc`, `srsRAN_Project/configs/*.yml`, srsRAN's `docker/open5gs`); cite
   the upstream file when deviating. Deviations from stock behaviour are explicit CLI flags, off by
   default.
5. **Compact.** Only what the USRP/Pixel scenario needs plus the one code-test path (srsUE over
   ZeroMQ, `make run-local`). No alternative stacks. Add a file only with a reason in `docs/ARCHITECTURE.md`.
6. **Two clocks everywhere** (`*_mono_ns` same host, `*_wall_ns` cross-host with NTP). Descriptive
   names with units in columns. Schema changes update `docs/TRACE_SCHEMA.md` in the same commit.
7. **Verify with real data** and log experiments (incl. negative results) in `docs/NOTES.md`.

```
make deps | submodules | build-gnb | build-libwebrtc | build-apps | build-ue-sim | core-up | core-down | run-local | verify RD=
scripts/run/run_gnb.sh b210_n78_tdd_20mhz <trace_dir>      (P5G_GNB_PCAP=1 adds stock pcaps)
```
