VITA_CC ?= arm-vita-eabi-gcc
VITA_AR ?= arm-vita-eabi-ar
VITA_ELF_CREATE ?= vita-elf-create
VITA_MAKE_FSELF ?= vita-make-fself
VITA_MKSFOEX ?= vita-mksfoex
VITA_PACK_VPK ?= vita-pack-vpk
HOST_CC ?= cc

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
VITA_COMMON_DIR ?= $(VITASDK)/share/gcc-arm-vita-eabi/samples/common
VITA_PROBE_TITLE_ID ?= VDPR00001
VITA_PROBE_OBJECTS := \
	$(VITA_PROBE_BUILD)/vita_probe.o \
	$(VITA_PROBE_BUILD)/debugScreen.o
VITA_PROBE_LDLIBS := \
	-lSceDisplay_stub \
	-lSceKernelThreadMgr_stub \
	-lSceProcessmgr_stub \
	-lSceSysmem_stub \
	-lSceLibKernel_stub

.PHONY: all vita vita-check vita-probe host-test check clean

all: vita

vita: $(VITA_BUILD)/libvitaprofiler.a

vita-check: vita

vita-probe: $(VITA_PROBE_BUILD)/vitaprofiler-probe.vpk

$(VITA_BUILD)/vitaprofiler.o: src/vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_vita.o: src/vitaprofiler_vita.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_names.o: src/vitaprofiler_names.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/libvitaprofiler.a: $(VITA_BUILD)/vitaprofiler.o $(VITA_BUILD)/vitaprofiler_vita.o $(VITA_BUILD)/vitaprofiler_names.o
	$(VITA_AR) rcs $@ $^

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

$(HOST_BUILD)/test_vitaprofiler: src/vitaprofiler.c tests/test_vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c tests/test_vitaprofiler.c -o $@

$(HOST_BUILD)/test_vitaprofiler_names: src/vitaprofiler.c src/vitaprofiler_names.c tests/test_vitaprofiler_names.c include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c src/vitaprofiler_names.c tests/test_vitaprofiler_names.c -o $@

host-test: $(HOST_BUILD)/test_vitaprofiler $(HOST_BUILD)/test_vitaprofiler_names
	$(HOST_BUILD)/test_vitaprofiler
	$(HOST_BUILD)/test_vitaprofiler_names

check: host-test vita-check

clean:
	rm -rf build
