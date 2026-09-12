VITA_CC ?= arm-vita-eabi-gcc
VITA_AR ?= arm-vita-eabi-ar
HOST_CC ?= cc

CPPFLAGS := -Iinclude
COMMON_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
HOST_CFLAGS ?= $(COMMON_CFLAGS)
VITA_CFLAGS ?= $(COMMON_CFLAGS)

HOST_BUILD := build/host
VITA_BUILD := build/vita

.PHONY: all vita vita-check host-test check clean

all: vita

vita: $(VITA_BUILD)/libvitaprofiler.a

vita-check: vita

$(VITA_BUILD)/vitaprofiler.o: src/vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitaprofiler_vita.o: src/vitaprofiler_vita.c include/vitaprofiler.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/libvitaprofiler.a: $(VITA_BUILD)/vitaprofiler.o $(VITA_BUILD)/vitaprofiler_vita.o
	$(VITA_AR) rcs $@ $^

$(HOST_BUILD)/test_vitaprofiler: src/vitaprofiler.c tests/test_vitaprofiler.c include/vitaprofiler.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) -pthread src/vitaprofiler.c tests/test_vitaprofiler.c -o $@

host-test: $(HOST_BUILD)/test_vitaprofiler
	$(HOST_BUILD)/test_vitaprofiler

check: host-test vita-check

clean:
	rm -rf build
