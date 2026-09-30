SHELL := /bin/bash
ROOT  := $(shell pwd)

.PHONY: help deps submodules build-gnb build-apps build-gst-rs webrtc-build-libwebrtc webrtc-build-apps build-ue-sim core-up core-down ota ota-stop run-local verify venv clean

# Transport trees: gstreamer/ (live x264, --cc profile|gcc), ffmpeg/ (SMEC-style pre-encoded RTP) and webrtc/ (frozen
# libwebrtc stack). They share no code, only the run-directory / trace-file contract (docs/TRACE_SCHEMA.md).
# TREE selects which one the shared targets use.
TREE ?= gstreamer

help:
	@echo "private-5g-industrial"
	@echo "  make deps             apt packages: srsRAN build deps + GStreamer 1.20 dev/plugins (Ubuntu 22.04)"
	@echo "  make submodules       fetch third_party/srsRAN_Project (shallow, pinned)"
	@echo "  make build-gnb        srsRAN gNB + tracer patches   -> build/srsran_gnb/apps/gnb/gnb"
	@echo "  make build-apps       [TREE=gstreamer|ffmpeg|webrtc] sender / receiver of the transport tree -> <tree>/build/apps/"
	@echo "  make build-gst-rs     rtpgccbwe (GCC bandwidth estimator, gst-plugins-rs 0.13.7, Rust) -> gstreamer/build/gst-plugins-rs/  [for --cc gcc]"
	@echo "  make webrtc-build-libwebrtc   frozen webrtc tree: fetch + build stock libwebrtc M120 (hours; or symlink a checkout to webrtc/libwebrtc)"
	@echo "  make webrtc-build-apps        frozen webrtc tree: build its sender / receiver"
	@echo "  make core-up          Open5GS (docker) + host route to the UE subnet     [gNB PC]"
	@echo "  make core-down"
	@echo "  make ota [OTA_LABEL=ota] [TREE=..]  gNB PC one-shot: core + gNB + metrics client + <tree> receivers -> results/<ts>-<LABEL>"
	@echo "  make ota-stop         stop all of the above"
	@echo "  make verify RD=results/<run>   completeness check of the real-time logs"
	@echo ""
	@echo "code test (no radio): make build-ue-sim ; make run-local [TREE=gstreamer|ffmpeg|webrtc DURATION=30 CODEC=H264 WIDTH=1280 HEIGHT=720 FPS=30 YUV= DIRECTION=ul|dl]"
	@echo ""
	@echo "run gNB:      scripts/run/run_gnb.sh b210_n78_tdd_20mhz results/<run>/gnb      [gNB PC]"
	@echo "run apps:     <tree>/run_receiver.sh on the receiver host, <tree>/run_sender.sh on the UE laptop, or <tree>/run_experiment.sh (docs/SETUP.md)"

deps:
	sudo apt-get install -y --no-install-recommends cmake ninja-build build-essential pkg-config \
	    libfftw3-dev libmbedtls-dev libsctp-dev libyaml-cpp-dev libgtest-dev libzmq3-dev libdw-dev \
	    libboost-program-options-dev libfmt-dev \
	    libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxcomposite-dev libxrandr-dev libxtst-dev \
	    python3-venv docker-compose-plugin \
	    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-tools gstreamer1.0-plugins-base \
	    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
	    rustc cargo \
	    libavformat-dev libavcodec-dev libavutil-dev ffmpeg unzip

submodules:
	git submodule update --init --depth 1

build-gnb:
	$(ROOT)/scripts/build/build_srsran_gnb.sh

build-apps:
	$(ROOT)/$(TREE)/scripts/build_apps.sh

build-gst-rs:
	$(ROOT)/gstreamer/scripts/build_gst_rs.sh

webrtc-build-libwebrtc:
	$(ROOT)/webrtc/scripts/build_libwebrtc.sh

webrtc-build-apps:
	$(ROOT)/webrtc/scripts/build_apps.sh

# srsUE for the ZeroMQ loopback code test (not part of the measurement testbed)
build-ue-sim:
	$(ROOT)/scripts/build/build_srsran_ue_sim.sh

core-up:
	$(ROOT)/scripts/run/start_core.sh

core-down:
	$(ROOT)/scripts/run/start_core.sh down

# gNB PC over-the-air session (see scripts/run/ota_restart.sh)
OTA_LABEL ?= ota
ota:
	P5G_TREE=$(TREE) $(ROOT)/scripts/run/ota_restart.sh $(OTA_LABEL)

ota-stop:
	$(ROOT)/scripts/run/ota_restart.sh stop

DURATION ?= 30
CODEC ?= H264
WIDTH ?= 1280
HEIGHT ?= 720
FPS ?= 30
YUV ?=
LABEL ?= local
DIRECTION ?= ul
run-local:
	$(ROOT)/scripts/run/run_local_e2e.sh --tree $(TREE) --duration $(DURATION) --codec $(CODEC) --width $(WIDTH) --height $(HEIGHT) \
	    --fps $(FPS) --label $(LABEL) --direction $(DIRECTION) $(if $(YUV),--yuv $(YUV),)

verify:
	@test -n "$(RD)" || (echo "usage: make verify RD=results/<run>" && exit 1)
	$(ROOT)/.venv/bin/python $(ROOT)/analysis/verify_run.py $(RD)

venv:
	python3 -m venv $(ROOT)/.venv && $(ROOT)/.venv/bin/pip install -q -r $(ROOT)/analysis/requirements.txt

clean:
	rm -rf $(ROOT)/gstreamer/build $(ROOT)/ffmpeg/build $(ROOT)/webrtc/build $(ROOT)/build/srsran_gnb $(ROOT)/build/srsran_gnb_src $(ROOT)/build/srsran_ue_sim
