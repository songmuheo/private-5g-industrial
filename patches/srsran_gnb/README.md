# srsRAN gNB tracer patches

Observe-only hooks that make the srsRAN_Project gNB (`release_25_10`, submodule
`third_party/srsRAN_Project`) write per-event CSV traces. Applied at build time by
`scripts/build/build_srsran_gnb.sh` onto a mirror of the stock source; the submodule is never
modified. Tracing is enabled at run time by `P5G_GNB_TRACE_DIR=<dir>`; without it the hooks cost one
relaxed atomic load each and the gNB behaves exactly like stock.

| file | hook point (release_25_10) | trace |
|---|---|---|
| `p5g_gnb_tracer.h` | installed as `include/srsran/support/p5g_gnb_tracer.h` | rows, lock-free rings, background CSV writer |
| `0001-gnb-main-tracer-init.patch` | `apps/gnb/gnb.cpp` `main()` | open traces from env, close via `make_scope_exit` on every exit path |
| `0002-scheduler-grant-trace.patch` | `lib/scheduler/cell_scheduler.cpp` `run_slot()` after `result_logger.on_scheduler_result()` | `gnb_sched_dl.csv`, `gnb_sched_ul.csv` (one row per PDSCH/PUSCH grant in the slot result) |
| `0003-mac-crc-bsr-ulpdu-trace.patch` | `lib/mac/mac_sched/srsran_scheduler_adapter.cpp` `handle_crc()`; `lib/mac/mac_ul/pdu_rx_handler.cpp` `handle_rx_pdu()` + BSR CE case | `gnb_ul_crc.csv`, `gnb_mac_ul_pdu.csv`, `gnb_bsr.csv` |
| `0004-uci-sr-csi-harqack-trace.patch` | `lib/scheduler/ue_scheduling/ue_event_manager.cpp` SR branches (PUCCH F0/F1, F2/F3/F4), `handle_harq_ind()`, `handle_csi()` | `gnb_sr.csv`, `gnb_dl_harq_ack.csv`, `gnb_csi.csv` |
| `0005-rlc-rx-am-trace.patch` | `lib/rlc/rlc_rx_am_entity.{h,cpp}` `handle_pdu()`, SDU delivery in `handle_data_pdu()`, `on_expired_reassembly_timer()` | `gnb_rlc_ul.csv` |
| `0006-pdcp-sdu-trace.patch` | `lib/pdcp/pdcp_entity_rx.{h,cpp}` all three `upper_dn.on_new_sdu()` sites; `lib/pdcp/pdcp_entity_tx.{h,cpp}` `handle_sdu()` | `gnb_pdcp_ul.csv`, `gnb_pdcp_dl.csv` (with IPv4/UDP/RTP header parse) |

Column definitions: `docs/TRACE_SCHEMA.md`.

## Design rules

* **No allocation, lock or I/O on gNB threads.** A hook copies one POD row into a pre-allocated
  ring (`fetch_add` to claim + release-store to publish); one `nice 19` thread formats CSV every
  `P5G_GNB_TRACE_FLUSH_MS` (default 500 ms).
* **Overflow is visible.** Rings are sized for >30 min at one cell; if one overflows the footer says
  so and a `<file>.ERROR` sidecar is written (the run is then rejected by `analysis/verify_run.py`).
* **Identity columns are added, decisions are not.** RLC/PDCP entities only keep `ue_index`/`rb_id`
  inside their log prefix, so the patches add three `p5g_*` members set in the constructor. Nothing
  else in the entities changes.
* **Two clocks per row** (`mono_ns`, `wall_ns`) so gNB rows can be joined with app traces on the
  same host (monotonic) or across hosts (wall, NTP/PTP-synced).

## Upgrading srsRAN

Bump the submodule, run `scripts/build/build_srsran_gnb.sh`; a patch that no longer applies fails
the build (`patch --forward` without fuzz). Re-locate the hook with the function names in the table,
regenerate the patch (the patches are plain `diff -u` output against `a/<path>`, `b/<path>`), and
re-run `make run-local` — `verify_run.py` checks that every trace has rows.
