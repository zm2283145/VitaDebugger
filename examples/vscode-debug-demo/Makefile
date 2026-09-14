TARGET := vscode-debug-demo
TITLE_ID := UVDBDEMO1
APP_NAME := VitaDebugger VS Code Demo
GDB_PORT ?= 1234

VITADEBUGGER_DIR := ../..
KUBRIDGE_DIR ?= ../../../kubridge-review
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local
DEBUGSCREEN_DIR := $(VITASDK)/share/gcc-arm-vita-eabi/samples/common
BUILD_DIR := build

ifneq ($(UVDB_TOOL_PATH),)
export PATH := $(UVDB_TOOL_PATH):$(PATH)
endif

UVDB_SOURCE_NAMES := \
	uvdb \
	uvdb_registers \
	uvdb_rsp \
	uvdb_thread_control \
	uvdb_monitor \
	uvdb_breakpoint_patch \
	uvdb_exclusive_step \
	uvdb_console \
	uvdb_console_transport \
	uvdb_debugnet \
	stdio_redirect
UVDB_OBJECTS := $(addprefix $(BUILD_DIR)/uvdb/,$(addsuffix .o,$(UVDB_SOURCE_NAMES)))
UVDB_HEADERS := $(wildcard $(VITADEBUGGER_DIR)/src/*.h) $(VITADEBUGGER_DIR)/uvdb.h

COMMON_FLAGS := \
	-std=gnu11 \
	-O0 \
	-g \
	-fno-omit-frame-pointer \
	-fno-inline-functions \
	-DDEMO_GDB_PORT=$(GDB_PORT) \
	-Wall \
	-Wextra \
	-Werror \
	-I"$(DEBUGSCREEN_DIR)" \
	-I"$(VITADEBUGGER_DIR)" \
	-I"$(VITADEBUGGER_DIR)/src" \
	-I"$(KUBRIDGE_DIR)"

APP_LIBS := \
	$(BUILD_DIR)/libuvdb.a \
	-L"$(KUBRIDGE_LIB_DIR)" \
	-lkubridge_stub \
	-lSceDisplay_stub \
	-lSceCtrl_stub \
	-lSceNet_stub \
	-lSceNetPs_stub \
	-lSceNetCtl_stub \
	-lSceSysmodule_stub \
	-lSceKernelModulemgr_stub \
	-pthread

.PHONY: all package clean

all: package

package: $(BUILD_DIR)/$(TARGET).vpk

$(BUILD_DIR):
	mkdir -p "$@"

$(BUILD_DIR)/uvdb:
	mkdir -p "$@"

$(BUILD_DIR)/main.o: main.c $(UVDB_HEADERS) Makefile | $(BUILD_DIR)
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/debugScreen.o: $(DEBUGSCREEN_DIR)/debugScreen.c Makefile | $(BUILD_DIR)
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/uvdb/%.o: $(VITADEBUGGER_DIR)/src/%.c $(UVDB_HEADERS) Makefile | $(BUILD_DIR)/uvdb
	arm-vita-eabi-gcc $(COMMON_FLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/libuvdb.a: $(UVDB_OBJECTS)
	rm -f "$@"
	arm-vita-eabi-ar rcs "$@" $^

$(BUILD_DIR)/$(TARGET).elf: $(BUILD_DIR)/main.o \
		$(BUILD_DIR)/debugScreen.o $(BUILD_DIR)/libuvdb.a
	arm-vita-eabi-gcc $(BUILD_DIR)/main.o $(BUILD_DIR)/debugScreen.o \
		$(APP_LIBS) -Wl,-q -o "$@"

$(BUILD_DIR)/$(TARGET).velf: $(BUILD_DIR)/$(TARGET).elf
	vita-elf-create "$<" "$@"

$(BUILD_DIR)/eboot.bin: $(BUILD_DIR)/$(TARGET).velf
	vita-make-fself "$<" "$@"

$(BUILD_DIR)/param.sfo: | $(BUILD_DIR)
	vita-mksfoex -s TITLE_ID=$(TITLE_ID) '$(APP_NAME)' "$@"

$(BUILD_DIR)/$(TARGET).vpk: $(BUILD_DIR)/eboot.bin $(BUILD_DIR)/param.sfo
	vita-pack-vpk -s $(BUILD_DIR)/param.sfo -b $(BUILD_DIR)/eboot.bin "$@"

clean:
	rm -rf "$(BUILD_DIR)"
