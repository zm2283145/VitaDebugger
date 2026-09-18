VITA_CC ?= arm-vita-eabi-gcc
VITA_AR ?= arm-vita-eabi-ar
VITA_ELF_CREATE ?= vita-elf-create
VITA_MAKE_FSELF ?= vita-make-fself
VITA_MKSFOEX ?= vita-mksfoex
VITA_PACK_VPK ?= vita-pack-vpk
HOST_CC ?= cc
PYTHON ?= python

CPPFLAGS := -Iinclude
COMMON_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
HOST_CFLAGS ?= $(COMMON_CFLAGS)
VITA_CFLAGS ?= $(COMMON_CFLAGS)
VITA_PROBE_CFLAGS ?= $(COMMON_CFLAGS)
# Reserve a full page boundary before writable data. Without this headroom,
# some optimized layouts leave too little room for vita-elf-create to append
# SCE metadata and the current Windows host tool can corrupt its heap.
VITA_PROBE_LDFLAGS ?= -Wl,--defsym=__sce_headroom=0x1000

HOST_BUILD := build/host
VITA_BUILD := build/vita
VITA_PROBE_BUILD := build/vita-probe
VITA_PMU_DISCOVERY_BUILD := build/vita-pmu-discovery
VITA_TCP_GATE_BUILD := build/vita-tcp-gate
VITA_COMMON_DIR ?= $(VITASDK)/share/gcc-arm-vita-eabi/samples/common
VITA_PROBE_TITLE_ID ?= VDPR00001
VITA_PMU_DISCOVERY_TITLE_ID ?= VDPD00001
VITA_TCP_GATE_TITLE_ID := VDPT00001
-include local/vita-tcp-gate.mk
VITA_TCP_GATE_HOST_A ?= 127
VITA_TCP_GATE_HOST_B ?= 0
VITA_TCP_GATE_HOST_C ?= 0
VITA_TCP_GATE_HOST_D ?= 1
VITA_TCP_GATE_PORT ?= 18195
VITA_TCP_GATE_CONFIG := $(wildcard local/vita-tcp-gate.mk)
VITA_PROBE_OBJECTS := \
	$(VITA_PROBE_BUILD)/vita_probe.o \
	$(VITA_PROBE_BUILD)/debugScreen.o
VITA_PROBE_LDLIBS := \
	-lSceDisplay_stub \
	-lSceKernelThreadMgr_stub \
	-lSceProcessmgr_stub \
	-lSceSysmem_stub \
	-lSceLibKernel_stub
VITA_PMU_DISCOVERY_OBJECTS := \
	$(VITA_PMU_DISCOVERY_BUILD)/vita_pmu_discovery.o \
	$(VITA_PMU_DISCOVERY_BUILD)/debugScreen.o
VITA_PMU_DISCOVERY_LDLIBS := \
	-lScePerf_stub \
	-lSceSysmodule_stub \
	-lSceKernelModulemgr_stub \
	-lSceIofilemgr_stub \
	-lSceDisplay_stub \
	-lSceCtrl_stub \
	-lSceKernelThreadMgr_stub \
	-lSceProcessmgr_stub \
	-lSceSysmem_stub \
	-ltaihen_stub \
	-lSceLibKernel_stub
VITA_TCP_GATE_OBJECTS := \
	$(VITA_TCP_GATE_BUILD)/main.o \
	$(VITA_TCP_GATE_BUILD)/debugScreen.o
VITA_TCP_GATE_CPPFLAGS := \
	-DVP_TCP_GATE_HOST_A=$(VITA_TCP_GATE_HOST_A) \
	-DVP_TCP_GATE_HOST_B=$(VITA_TCP_GATE_HOST_B) \
	-DVP_TCP_GATE_HOST_C=$(VITA_TCP_GATE_HOST_C) \
	-DVP_TCP_GATE_HOST_D=$(VITA_TCP_GATE_HOST_D) \
	-DVP_TCP_GATE_PORT=$(VITA_TCP_GATE_PORT)
VITA_TCP_GATE_LDLIBS := \
	-lSceDisplay_stub \
	-lSceCtrl_stub \
	-lSceNet_stub \
	-lSceNetCtl_stub \
	-lSceSysmodule_stub \
	-lSceKernelThreadMgr_stub \
	-lSceProcessmgr_stub \
	-lSceSysmem_stub \
	-lSceLibKernel_stub

.PHONY: all vita vita-check vita-probe vita-pmu-discovery vita-tcp-gate desktop host-test check clean

all: vita

vita: $(VITA_BUILD)/libvitaprofiler.a

vita-check: vita $(VITA_BUILD)/vitaprofiler-pmu-link-check.elf $(VITA_BUILD)/vitaprofiler-tcp-link-check.elf

vita-probe: $(VITA_PROBE_BUILD)/vitaprofiler-probe.vpk

vita-pmu-discovery: $(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.vpk

vita-tcp-gate: $(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.vpk

desktop:
	$(PYTHON) tools/vitaprofiler_gui.py

$(VITA_BUILD)/vitaprofiler.o: src/vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_vita.o: src/vitaprofiler_vita.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_names.o: src/vitaprofiler_names.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_stream.o: src/vitaprofiler_stream.c include/vitaprofiler_stream.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_tcp_vita.o: src/vitaprofiler_tcp_vita.c include/vitaprofiler_tcp_vita.h include/vitaprofiler_stream.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_pmu.o: src/vitaprofiler_pmu.c include/vitaprofiler_pmu.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_pmu_vita.o: src/vitaprofiler_pmu_vita.c include/vitaprofiler_pmu_vita.h include/vitaprofiler_pmu.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_gpu.o: src/vitaprofiler_gpu.c include/vitaprofiler_gpu.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_sampling.o: src/vitaprofiler_sampling.c include/vitaprofiler_sampling.h include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/libvitaprofiler.a: $(VITA_BUILD)/vitaprofiler.o $(VITA_BUILD)/vitaprofiler_vita.o $(VITA_BUILD)/vitaprofiler_names.o $(VITA_BUILD)/vitaprofiler_stream.o $(VITA_BUILD)/vitaprofiler_tcp_vita.o $(VITA_BUILD)/vitaprofiler_pmu.o $(VITA_BUILD)/vitaprofiler_pmu_vita.o $(VITA_BUILD)/vitaprofiler_gpu.o $(VITA_BUILD)/vitaprofiler_sampling.o
	$(VITA_AR) rcs $@ $^

$(VITA_BUILD)/vitaprofiler-pmu-link-check.elf: tests/vita_pmu_link_check.c $(VITA_BUILD)/libvitaprofiler.a include/vitaprofiler_pmu_vita.h
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) $< $(VITA_BUILD)/libvitaprofiler.a -lSceKernelThreadMgr_stub -lSceLibKernel_stub -o $@

$(VITA_BUILD)/vitaprofiler-tcp-link-check.elf: tests/vita_tcp_link_check.c $(VITA_BUILD)/libvitaprofiler.a include/vitaprofiler_tcp_vita.h
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) $< $(VITA_BUILD)/libvitaprofiler.a -lSceNet_stub -lSceProcessmgr_stub -lSceLibKernel_stub -o $@

$(VITA_PROBE_BUILD)/vita_probe.o: tests/vita_probe.c include/vitaprofiler.h
	mkdir -p $(VITA_PROBE_BUILD)
	$(VITA_CC) $(CPPFLAGS) -I$(VITA_COMMON_DIR) $(VITA_PROBE_CFLAGS) -c $< -o $@

$(VITA_PROBE_BUILD)/debugScreen.o: $(VITA_COMMON_DIR)/debugScreen.c
	mkdir -p $(VITA_PROBE_BUILD)
	$(VITA_CC) -I$(VITA_COMMON_DIR) $(VITA_CFLAGS) -c $< -o $@

$(VITA_PROBE_BUILD)/vitaprofiler-probe.elf: $(VITA_PROBE_OBJECTS) $(VITA_BUILD)/libvitaprofiler.a
	$(VITA_CC) -Wl,-q -Wl,-z,nocopyreloc $(VITA_PROBE_LDFLAGS) $^ $(VITA_PROBE_LDLIBS) -o $@

$(VITA_PROBE_BUILD)/vitaprofiler-probe.velf: $(VITA_PROBE_BUILD)/vitaprofiler-probe.elf
	$(VITA_ELF_CREATE) $< $@

$(VITA_PROBE_BUILD)/eboot.bin: $(VITA_PROBE_BUILD)/vitaprofiler-probe.velf
	$(VITA_MAKE_FSELF) $< $@

$(VITA_PROBE_BUILD)/param.sfo:
	mkdir -p $(VITA_PROBE_BUILD)
	$(VITA_MKSFOEX) -s TITLE_ID=$(VITA_PROBE_TITLE_ID) 'VitaProfiler self-test' $@

$(VITA_PROBE_BUILD)/vitaprofiler-probe.vpk: $(VITA_PROBE_BUILD)/eboot.bin $(VITA_PROBE_BUILD)/param.sfo
	$(VITA_PACK_VPK) -s $(VITA_PROBE_BUILD)/param.sfo -b $(VITA_PROBE_BUILD)/eboot.bin $@

$(VITA_PMU_DISCOVERY_BUILD)/vita_pmu_discovery.o: tests/vita_pmu_discovery.c
	mkdir -p $(VITA_PMU_DISCOVERY_BUILD)
	$(VITA_CC) $(CPPFLAGS) -I$(VITA_COMMON_DIR) $(VITA_PROBE_CFLAGS) -c $< -o $@

$(VITA_PMU_DISCOVERY_BUILD)/debugScreen.o: $(VITA_COMMON_DIR)/debugScreen.c
	mkdir -p $(VITA_PMU_DISCOVERY_BUILD)
	$(VITA_CC) -I$(VITA_COMMON_DIR) $(VITA_CFLAGS) -c $< -o $@

$(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.elf: $(VITA_PMU_DISCOVERY_OBJECTS)
	$(VITA_CC) -Wl,-q -Wl,-z,nocopyreloc $(VITA_PROBE_LDFLAGS) $^ $(VITA_PMU_DISCOVERY_LDLIBS) -o $@

$(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.velf: $(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.elf
	$(VITA_ELF_CREATE) $< $@

$(VITA_PMU_DISCOVERY_BUILD)/eboot.bin: $(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.velf
	$(VITA_MAKE_FSELF) $< $@

$(VITA_PMU_DISCOVERY_BUILD)/param.sfo:
	mkdir -p $(VITA_PMU_DISCOVERY_BUILD)
	$(VITA_MKSFOEX) -s TITLE_ID=$(VITA_PMU_DISCOVERY_TITLE_ID) 'VitaProfiler PMU discovery' $@

$(VITA_PMU_DISCOVERY_BUILD)/vitaprofiler-pmu-discovery.vpk: $(VITA_PMU_DISCOVERY_BUILD)/eboot.bin $(VITA_PMU_DISCOVERY_BUILD)/param.sfo
	$(VITA_PACK_VPK) -s $(VITA_PMU_DISCOVERY_BUILD)/param.sfo -b $(VITA_PMU_DISCOVERY_BUILD)/eboot.bin $@

$(VITA_TCP_GATE_BUILD)/main.o: vita-tcp-gate/main.c include/vitaprofiler.h include/vitaprofiler_stream.h include/vitaprofiler_tcp_vita.h Makefile $(VITA_TCP_GATE_CONFIG)
	mkdir -p $(VITA_TCP_GATE_BUILD)
	$(VITA_CC) $(CPPFLAGS) -I$(VITA_COMMON_DIR) $(VITA_TCP_GATE_CPPFLAGS) $(VITA_PROBE_CFLAGS) -c $< -o $@

$(VITA_TCP_GATE_BUILD)/debugScreen.o: $(VITA_COMMON_DIR)/debugScreen.c
	mkdir -p $(VITA_TCP_GATE_BUILD)
	$(VITA_CC) -I$(VITA_COMMON_DIR) $(VITA_CFLAGS) -c $< -o $@

$(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.elf: $(VITA_TCP_GATE_OBJECTS) $(VITA_BUILD)/libvitaprofiler.a
	$(VITA_CC) -Wl,-q -Wl,-z,nocopyreloc $(VITA_PROBE_LDFLAGS) $^ $(VITA_TCP_GATE_LDLIBS) -o $@

$(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.velf: $(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.elf
	$(VITA_ELF_CREATE) $< $@

$(VITA_TCP_GATE_BUILD)/eboot.bin: $(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.velf
	$(VITA_MAKE_FSELF) $< $@

$(VITA_TCP_GATE_BUILD)/param.sfo:
	mkdir -p $(VITA_TCP_GATE_BUILD)
	$(VITA_MKSFOEX) -s TITLE_ID=$(VITA_TCP_GATE_TITLE_ID) 'VitaProfiler TCP gate' $@

$(VITA_TCP_GATE_BUILD)/vitaprofiler-tcp-gate.vpk: $(VITA_TCP_GATE_BUILD)/eboot.bin $(VITA_TCP_GATE_BUILD)/param.sfo
	$(VITA_PACK_VPK) -s $(VITA_TCP_GATE_BUILD)/param.sfo -b $(VITA_TCP_GATE_BUILD)/eboot.bin $@

$(HOST_BUILD)/test_vitaprofiler: src/vitaprofiler.c tests/test_vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c tests/test_vitaprofiler.c -o $@

$(HOST_BUILD)/test_vitaprofiler_names: src/vitaprofiler.c src/vitaprofiler_names.c tests/test_vitaprofiler_names.c include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c src/vitaprofiler_names.c tests/test_vitaprofiler_names.c -o $@

$(HOST_BUILD)/test_vitaprofiler_stream: src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c tests/test_vitaprofiler_stream.c include/vitaprofiler.h include/vitaprofiler_stream.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c tests/test_vitaprofiler_stream.c -o $@

$(HOST_BUILD)/test_vitaprofiler_tcp_vita: src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c src/vitaprofiler_tcp_vita.c tests/vita_tcp_sce_net_shim.c tests/vita_tcp_sce_net_shim.h tests/test_vitaprofiler_tcp_vita.c include/vitaprofiler.h include/vitaprofiler_stream.h include/vitaprofiler_tcp_vita.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -DVP_VITA_TCP_HOST_TEST -DVP_VITA_TCP_SCE_NET_HOST_TEST -pthread src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c src/vitaprofiler_tcp_vita.c tests/vita_tcp_sce_net_shim.c tests/test_vitaprofiler_tcp_vita.c -o $@

$(HOST_BUILD)/test_vitaprofiler_integrations: src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_pmu.c src/vitaprofiler_gpu.c tests/test_vitaprofiler_integrations.c include/vitaprofiler.h include/vitaprofiler_pmu.h include/vitaprofiler_gpu.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -DVITAPROFILER_GRAPHICS_ENABLED -pthread src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_pmu.c src/vitaprofiler_gpu.c tests/test_vitaprofiler_integrations.c -o $@

$(HOST_BUILD)/test_vitaprofiler_graphics_disabled: tests/test_vitaprofiler_graphics_disabled.c include/vitaprofiler.h include/vitaprofiler_gpu.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) tests/test_vitaprofiler_graphics_disabled.c -o $@

$(HOST_BUILD)/test_vitaprofiler_pmu_vita: src/vitaprofiler.c src/vitaprofiler_pmu.c src/vitaprofiler_pmu_vita.c tests/test_vitaprofiler_pmu_vita.c include/vitaprofiler.h include/vitaprofiler_pmu.h include/vitaprofiler_pmu_vita.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) src/vitaprofiler.c src/vitaprofiler_pmu.c src/vitaprofiler_pmu_vita.c tests/test_vitaprofiler_pmu_vita.c -o $@

$(HOST_BUILD)/test_vitaprofiler_sampling: src/vitaprofiler_sampling.c tests/test_vitaprofiler_sampling.c include/vitaprofiler_sampling.h include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) src/vitaprofiler_sampling.c tests/test_vitaprofiler_sampling.c -o $@

$(HOST_BUILD)/generate_vitaprofiler_fixture: src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c src/vitaprofiler_gpu.c tests/generate_vitaprofiler_fixture.c include/vitaprofiler.h include/vitaprofiler_stream.h include/vitaprofiler_gpu.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c src/vitaprofiler_names.c src/vitaprofiler_stream.c src/vitaprofiler_gpu.c tests/generate_vitaprofiler_fixture.c -o $@

$(HOST_BUILD)/capture-from-c.vptrace: $(HOST_BUILD)/generate_vitaprofiler_fixture
	$(HOST_BUILD)/generate_vitaprofiler_fixture $@

host-test: $(HOST_BUILD)/test_vitaprofiler $(HOST_BUILD)/test_vitaprofiler_names $(HOST_BUILD)/test_vitaprofiler_stream $(HOST_BUILD)/test_vitaprofiler_tcp_vita $(HOST_BUILD)/test_vitaprofiler_integrations $(HOST_BUILD)/test_vitaprofiler_graphics_disabled $(HOST_BUILD)/test_vitaprofiler_pmu_vita $(HOST_BUILD)/test_vitaprofiler_sampling $(HOST_BUILD)/capture-from-c.vptrace
	$(HOST_BUILD)/test_vitaprofiler
	$(HOST_BUILD)/test_vitaprofiler_names
	$(HOST_BUILD)/test_vitaprofiler_stream
	$(HOST_BUILD)/test_vitaprofiler_tcp_vita
	$(HOST_BUILD)/test_vitaprofiler_integrations
	$(HOST_BUILD)/test_vitaprofiler_graphics_disabled
	$(HOST_BUILD)/test_vitaprofiler_pmu_vita
	$(HOST_BUILD)/test_vitaprofiler_sampling
	$(PYTHON) tests/test_vitaprofiler_trace.py
	$(PYTHON) tests/test_vitaprofiler_desktop.py

check: host-test vita-check

clean:
	rm -rf build
