VITA_CC ?= arm-vita-eabi-gcc
VITA_AR ?= arm-vita-eabi-ar
HOST_CC ?= cc
PYTHON ?= python

CPPFLAGS := -Iinclude
MONOCYPHER_DIR := ../deploy/agent/third_party/monocypher
CRYPTO_CPPFLAGS := -I$(MONOCYPHER_DIR)
COMMON_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
HOST_CFLAGS ?= $(COMMON_CFLAGS)
VITA_CFLAGS ?= $(COMMON_CFLAGS)
HOST_BUILD := build/host
VITA_BUILD := build/vita

.PHONY: all host-test vita vita-check check clean

all: vita

$(HOST_BUILD)/test_vitadebug_screen: src/vitadebug_screen.c tests/test_vitadebug_screen.c include/vitadebug_screen.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) src/vitadebug_screen.c tests/test_vitadebug_screen.c -o $@

$(HOST_BUILD)/test_vitadebug_companion: src/vitadebug_companion.c src/vitadebug_input_trace.c src/vitadebug_screen.c tests/test_vitadebug_companion.c include/vitadebug_companion.h include/vitadebug_input_trace.h include/vitadebug_screen.h $(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(CRYPTO_CPPFLAGS) $(HOST_CFLAGS) src/vitadebug_companion.c src/vitadebug_input_trace.c src/vitadebug_screen.c $(MONOCYPHER_DIR)/monocypher.c tests/test_vitadebug_companion.c -o $@

$(HOST_BUILD)/test_vitadebug_input_trace: src/vitadebug_input_trace.c tests/test_vitadebug_input_trace.c include/vitadebug_input_trace.h
	mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) src/vitadebug_input_trace.c tests/test_vitadebug_input_trace.c -o $@

host-test: $(HOST_BUILD)/test_vitadebug_screen $(HOST_BUILD)/test_vitadebug_input_trace $(HOST_BUILD)/test_vitadebug_companion
	$(HOST_BUILD)/test_vitadebug_screen
	$(HOST_BUILD)/test_vitadebug_input_trace
	$(HOST_BUILD)/test_vitadebug_companion
	PYTHONPATH=host $(PYTHON) -m unittest discover -s tests -p "test_*.py" -v

$(VITA_BUILD)/vitadebug_screen.o: src/vitadebug_screen.c include/vitadebug_screen.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitadebug_companion.o: src/vitadebug_companion.c include/vitadebug_companion.h include/vitadebug_input_trace.h include/vitadebug_screen.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(CRYPTO_CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/vitadebug_input_trace.o: src/vitadebug_input_trace.c include/vitadebug_input_trace.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/monocypher.o: $(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher.h
	mkdir -p $(VITA_BUILD)
	$(VITA_CC) $(CRYPTO_CPPFLAGS) $(VITA_CFLAGS) -c $< -o $@

$(VITA_BUILD)/libvitadebug_screen.a: $(VITA_BUILD)/vitadebug_screen.o
	$(VITA_AR) rcs $@ $^

$(VITA_BUILD)/libvitadebug_companion.a: $(VITA_BUILD)/vitadebug_companion.o $(VITA_BUILD)/vitadebug_input_trace.o $(VITA_BUILD)/vitadebug_screen.o $(VITA_BUILD)/monocypher.o
	$(VITA_AR) rcs $@ $^

$(VITA_BUILD)/vita-link-check.elf: tests/vita_link_check.c $(VITA_BUILD)/libvitadebug_companion.a include/vitadebug_companion.h include/vitadebug_screen.h
	$(VITA_CC) $(CPPFLAGS) $(VITA_CFLAGS) $< $(VITA_BUILD)/libvitadebug_companion.a -lSceLibKernel_stub -o $@

vita: $(VITA_BUILD)/libvitadebug_companion.a $(VITA_BUILD)/libvitadebug_screen.a

vita-check: vita $(VITA_BUILD)/vita-link-check.elf

check: host-test vita-check

clean:
	rm -rf build
