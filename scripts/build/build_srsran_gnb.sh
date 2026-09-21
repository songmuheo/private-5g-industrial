#!/usr/bin/env bash
# Build the srsRAN_Project gNB (native, host toolchain) with the gNB tracer patches.
#
# Design:
#   * third_party/srsRAN_Project is a pinned, STOCK submodule and is never edited.
#   * The source is mirrored into build/srsran_gnb_src/ (rsync, mtime-preserving),
#     the patches in patches/srsran_gnb/ are applied to the mirror, and the mirror
#     is compiled out-of-tree into build/srsran_gnb/.
#   * Re-running is incremental: rsync only touches changed files, so ninja only
#     recompiles what the patches changed.
#
# Radio front-end: UHD (USRP B210). ZeroMQ support is left enabled in the build (stock default,
# no runtime effect unless a profile selects it).
#
# Output: build/srsran_gnb/apps/gnb/gnb
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_STOCK="$ROOT/third_party/srsRAN_Project"
SRC_PATCHED="$ROOT/build/srsran_gnb_src"
BUILD_DIR="$ROOT/build/srsran_gnb"
PATCH_DIR="$ROOT/patches/srsran_gnb"
JOBS="${JOBS:-$(nproc)}"

if [ ! -f "$SRC_STOCK/CMakeLists.txt" ]; then
  echo "[build_srsran_gnb] submodule missing: run 'git submodule update --init --depth 1'" >&2
  exit 1
fi
if [ -n "$(git -C "$SRC_STOCK" status --porcelain 2>/dev/null)" ]; then
  echo "[build_srsran_gnb] ERROR: third_party/srsRAN_Project is not clean. It must stay stock;" >&2
  echo "                   put changes into patches/srsran_gnb/ instead." >&2
  exit 1
fi

echo "[build_srsran_gnb] stock commit: $(git -C "$SRC_STOCK" rev-parse --short HEAD) ($(git -C "$SRC_STOCK" describe --tags --always))"

# 1) Mirror stock source (mtimes preserved -> incremental ninja).
mkdir -p "$SRC_PATCHED"
rsync -a --delete --exclude '.git' "$SRC_STOCK/" "$SRC_PATCHED/"

# 2) Drop the tracer header into the mirror and apply patches in lexical order.
#    The header is added (not patched in) so it can be versioned as a plain file.
if [ -f "$PATCH_DIR/p5g_gnb_tracer.h" ]; then
  install -m 0644 "$PATCH_DIR/p5g_gnb_tracer.h" "$SRC_PATCHED/include/srsran/support/p5g_gnb_tracer.h"
fi
shopt -s nullglob
for p in "$PATCH_DIR"/*.patch; do
  echo "[build_srsran_gnb] applying $(basename "$p")"
  # The mirror was just re-synced from stock, so every patch must apply cleanly
  # in the forward direction. Any failure is fatal (no silent partial patching).
  patch -p1 --forward --silent -d "$SRC_PATCHED" < "$p"
done
shopt -u nullglob

# 3) Configure + build.
cmake -S "$SRC_PATCHED" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_UHD=ON \
  -DENABLE_ZEROMQ=ON \
  -DENABLE_DPDK=OFF \
  -DBUILD_TESTS=OFF \
  -DENABLE_WERROR=OFF \
  -DENABLE_EXPORT=OFF
ninja -C "$BUILD_DIR" -j"$JOBS" gnb

echo "[build_srsran_gnb] done: $BUILD_DIR/apps/gnb/gnb"
"$BUILD_DIR/apps/gnb/gnb" --version 2>/dev/null | head -2 || true
