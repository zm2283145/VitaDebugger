TARGET := debugnet-exit-gate
TITLE_ID := VDLG00001
APP_NAME := VitaDebugNet Exit Gate

UVDB_ROOT ?= ../../..
BUILD_DIR ?= build
DEBUGNET_GATE_HOST ?=
DEBUGNET_GATE_PORT ?= 18194
DEBUGSCREEN_DIR := $(VITASDK)/share/gcc-arm-vita-eabi/samples/common

ifeq ($(strip $(DEBUGNET_GATE_HOST)),)
$(error DEBUGNET_GATE_HOST must be set to the PC receiver's LAN IPv4 address)
endif

COMMON_FLAGS := \
	-std=gnu11 \
	-O2 \
	-g \
	-Wall \
	-Wextra \
	-Werror \
	-DDEBUGNET_GATE_HOST=\"$(DEBUGNET_GATE_HOST)\" \
	-DDEBUGNET_GATE_PORT=$(DEBUGNET_GATE_PORT) \
	-I"$(DEBUGSCREEN_DIR)" \
	-I"$(UVDB_ROOT)" \
	-I"$(UVDB_ROOT)/src"

APP_LIBS := \
	-lSceDisplay_stub \
	-lSceCtrl_stub \
	-lSceNet_stub \
	-lSceNetPs_stub \
	-lSceNetCtl_stub \
	-lSceSysmodule_stub \
	-pthread

.PHONY: all package clean

all: package

package: $(BUILD_DIR)/$(TARGET).vpk

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/main.o: main.c $(UVDB_ROOT)/uvdb.h Makefile | $(BUILD_DIR)
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/uvdb_debugnet.o: $(UVDB_ROOT)/src/uvdb_debugnet.c \
		$(UVDB_ROOT)/uvdb.h Makefile | $(BUILD_DIR)
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/debugScreen.o: $(DEBUGSCREEN_DIR)/debugScreen.c Makefile | $(BUILD_DIR)
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/$(TARGET).elf: $(BUILD_DIR)/main.o \
		$(BUILD_DIR)/uvdb_debugnet.o $(BUILD_DIR)/debugScreen.o
	arm-vita-eabi-gcc $^ -Wl,-q $(APP_LIBS) -o "$@"

$(BUILD_DIR)/$(TARGET).velf: $(BUILD_DIR)/$(TARGET).elf
	vita-elf-create "$<" "$@"

$(BUILD_DIR)/eboot.bin: $(BUILD_DIR)/$(TARGET).velf
	vita-make-fself "$<" "$@"

$(BUILD_DIR)/param.sfo: Makefile | $(BUILD_DIR)
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) '$(APP_NAME)' "$@"

$(BUILD_DIR)/$(TARGET).vpk: $(BUILD_DIR)/eboot.bin $(BUILD_DIR)/param.sfo
	vita-pack-vpk -s $(BUILD_DIR)/param.sfo -b $(BUILD_DIR)/eboot.bin "$@"

clean:
	rm -rf "$(BUILD_DIR)"
