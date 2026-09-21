SHELL := /bin/bash
ROOT  := $(shell pwd)

.PHONY: help deps submodules build-gnb build-libwebrtc build-apps build-ue-sim core-up core-down run-local verify venv clean

help:
	@echo "private-5g-industrial"
	@echo "  make deps             apt packages for building srsRAN natively (Ubuntu 22.04)"
	@echo "  make submodules       fetch third_party/srsRAN_Project (shallow, pinned)"
	@echo "  make build-gnb        srsRAN gNB + tracer patches   -> build/srsran_gnb/apps/gnb/gnb"
	@echo "  make build-libwebrtc  fetch + build stock libwebrtc M120 (hours; or symlink an existing checkout to third_party/libwebrtc)"
	@echo "  make build-apps       video_sender / video_receiver -> build/apps/"
	@echo "  make core-up          Open5GS (docker) + host route to the UE subnet     [gNB PC]"
	@echo "  make core-down"
	@echo "  make verify RD=results/<run>   completeness check of the real-time logs"
	@echo ""
	@echo "code test (no radio): make build-ue-sim ; make run-local [DURATION=30 CODEC=H264 WIDTH=1280 HEIGHT=720 FPS=30 YUV= DIRECTION=ul|dl]"
	@echo ""
	@echo "run gNB:      scripts/run/run_gnb.sh b210_n78_tdd_20mhz results/<run>/gnb      [gNB PC]"
	@echo "run apps:     see docs/SETUP.md (receiver + signaling on the internet host, sender on the UE laptop)"

deps:
	sudo apt-get install -y --no-install-recommends cmake ninja-build build-essential pkg-config \
	    libfftw3-dev libmbedtls-dev libsctp-dev libyaml-cpp-dev libgtest-dev libzmq3-dev libdw-dev \
	    libboost-program-options-dev libfmt-dev \
	    libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxcomposite-dev libxrandr-dev libxtst-dev \
	    python3-venv docker-compose-plugin

submodules:
	git submodule update --init --depth 1

build-gnb:
	$(ROOT)/scripts/build/build_srsran_gnb.sh

build-libwebrtc:
	$(ROOT)/scripts/build/build_libwebrtc.sh

build-apps:
	$(ROOT)/scripts/build/build_apps.sh

# srsUE for the ZeroMQ loopback code test (not part of the measurement testbed)
build-ue-sim:
	$(ROOT)/scripts/build/build_srsran_ue_sim.sh

core-up:
	$(ROOT)/scripts/run/start_core.sh

core-down:
	$(ROOT)/scripts/run/start_core.sh down

DURATION ?= 30
CODEC ?= H264
WIDTH ?= 1280
HEIGHT ?= 720
FPS ?= 30
YUV ?=
LABEL ?= local
DIRECTION ?= ul
run-local:
	$(ROOT)/scripts/run/run_local_e2e.sh --duration $(DURATION) --codec $(CODEC) --width $(WIDTH) --height $(HEIGHT) \
	    --fps $(FPS) --label $(LABEL) --direction $(DIRECTION) $(if $(YUV),--yuv $(YUV),)

verify:
	@test -n "$(RD)" || (echo "usage: make verify RD=results/<run>" && exit 1)
	$(ROOT)/.venv/bin/python $(ROOT)/analysis/verify_run.py $(RD)

venv:
	python3 -m venv $(ROOT)/.venv && $(ROOT)/.venv/bin/pip install -q -r $(ROOT)/analysis/requirements.txt

clean:
	rm -rf $(ROOT)/build/apps $(ROOT)/build/srsran_gnb $(ROOT)/build/srsran_gnb_src $(ROOT)/build/srsran_ue_sim
