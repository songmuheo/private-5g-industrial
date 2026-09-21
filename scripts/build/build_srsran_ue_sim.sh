#!/usr/bin/env bash
# Build srsUE (srsRAN_4G, 5G SA mode) — CODE-TEST TOOL ONLY.
#
# The testbed UEs are Pixel phones over a USRP B210. srsUE exists in this repo so that the gNB tracer
# and the WebRTC apps can be exercised end-to-end on one PC without radios (ZeroMQ baseband loopback,
# scripts/run/run_local_e2e.sh). It is built stock, out-of-tree, from the pinned submodule.
# srsUE 5G SA supports 15 kHz SCS only -> paired with ran/gnb/configs/gnb_zmq_local.yml (band 3 FDD).
# Output: build/srsran_ue_sim/srsue/src/srsue
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/third_party/srsRAN_4G"
BUILD_DIR="$ROOT/build/srsran_ue_sim"
JOBS="${JOBS:-$(nproc)}"

[ -f "$SRC/CMakeLists.txt" ] || { echo "[build_srsran_ue_sim] submodule missing: git submodule update --init --depth 1" >&2; exit 1; }
echo "[build_srsran_ue_sim] stock commit: $(git -C "$SRC" rev-parse --short HEAD) ($(git -C "$SRC" describe --tags --always))"

cmake -S "$SRC" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SRSUE=ON -DENABLE_SRSENB=OFF -DENABLE_SRSEPC=OFF -DENABLE_GUI=OFF \
  -DENABLE_UHD=OFF -DENABLE_BLADERF=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_ZEROMQ=ON -DENABLE_HARDSIM=OFF
make -C "$BUILD_DIR" -j"$JOBS" srsue
echo "[build_srsran_ue_sim] done: $BUILD_DIR/srsue/src/srsue"
