all: libuvdb.a

clean:
	rm -f *.o tests/*.o *.a *.elf *.velf eboot.bin param.sfo *.vpk *.psp2dmp test-rsp test-rsp.exe test-rsp-console test-rsp-console.exe test-register-bank test-register-bank.exe test-thread-control test-thread-control.exe test-console-queue test-console-queue.exe kernel/dipsw-read-probe/test-record kernel/dipsw-read-probe/test-record.exe kernel/dipsw-set-restore-probe/test-record kernel/dipsw-set-restore-probe/test-record.exe kernel/dipsw-dbgvcr-probe/test-record kernel/dipsw-dbgvcr-probe/test-record.exe

package: uvdb-test.vpk

deploy: package
	curl -v -T uvdb-test.vpk ftp://$(VITA_IP):1337/ux0:/uvdb-test.vpk 

fetch_dumps:
	curl ftp://$(VITA_IP):1337/ux0:/data/ | grep -o 'psp2core-.*' | while read line; do curl "ftp://$(VITA_IP):1337/ux0:/data/$$line" > "$$line"; curl -v "ftp://$(VITA_IP):1337/" -Q "DELE ux0:/data/$$line" >/dev/null; done

KUBRIDGE_DIR ?= ../kubridge-review
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local
VITADEBUG_KERNEL_DIR ?= kernel
VITADEBUG_KERNEL_BUILD_DIR ?= $(VITADEBUG_KERNEL_DIR)/build

EXTRA_CFLAGS := -O0 -g -Wall -Wextra -I $(VITASDK)/share/gcc-arm-vita-eabi/samples/common -I $(KUBRIDGE_DIR)
EXTRA_LDFLAGS := $(CFLAGS) -Wl,-q -L $(KUBRIDGE_LIB_DIR) -lSceDisplay_stub -lSceNetPs_stub -lSceKernelModulemgr_stub -lkubridge_stub -pthread

ifdef UVDB_DEBUGNET_HOST
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_HOST=\"$(UVDB_DEBUGNET_HOST)\"
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_PORT=$(or $(UVDB_DEBUGNET_PORT),18194)
endif

ifeq ($(UVDB_DEBUGNET_LIFECYCLE_TEST),1)
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_LIFECYCLE_TEST
endif

ifeq ($(UVDB_KERNEL_THREAD_CONTROL),1)
override CFLAGS += -DUVDB_KERNEL_THREAD_CONTROL -I $(VITADEBUG_KERNEL_DIR)/include
EXTRA_LDFLAGS += $(VITADEBUG_KERNEL_BUILD_DIR)/vitadebug_stubs/libvitadebug_kernel_stub.a
endif

ifeq ($(UVDB_KERNEL_VFP_READS),1)
ifneq ($(UVDB_KERNEL_THREAD_CONTROL),1)
$(error UVDB_KERNEL_VFP_READS=1 requires UVDB_KERNEL_THREAD_CONTROL=1)
endif
override CFLAGS += -DUVDB_KERNEL_VFP_READS
endif

ifeq ($(UVDB_GDB_VFP_FIXTURE),1)
ifneq ($(UVDB_KERNEL_VFP_READS),1)
$(error UVDB_GDB_VFP_FIXTURE=1 requires UVDB_KERNEL_VFP_READS=1)
endif
override EXTRA_CFLAGS += -DUVDB_GDB_VFP_FIXTURE
endif

override CFLAGS += -I $(KUBRIDGE_DIR) -Wall -Wextra -g

%.o: %.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

# This compact example Makefile shares object names across plain, kernel, and
# VFP configurations. Always rebuild the two configuration-sensitive objects so
# changing feature flags cannot silently reuse an incompatible prior object.
.PHONY: force-feature-objects
force-feature-objects:

uvdb.o test.o: force-feature-objects

ifeq ($(UVDB_KERNEL_THREAD_CONTROL),1)
uvdb.o test.o: $(VITADEBUG_KERNEL_DIR)/include/vitadebug_kernel.h
endif

uvdb.o: protocol/arm_vfp_target_xml.inc

test.o: test.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

psvDebugScreen.o: $(VITASDK)/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

tests/thumb_step_returns.o: tests/thumb_step_returns.S
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

libuvdb.a: uvdb.o uvdb_registers.o uvdb_rsp.o uvdb_thread_control.o uvdb_console.o uvdb_debugnet.o stdio_redirect.o
	arm-vita-eabi-ar rcs $@ $^

HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -I.
ifeq ($(OS),Windows_NT)
HOST_THREAD_FLAGS ?= -pthread -static
HOST_EXEEXT ?= .exe
HOST_CC_RUN ?= powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/invoke-vita-env.ps1 $(HOST_CC)
else
HOST_THREAD_FLAGS ?= -pthread
HOST_EXEEXT ?=
HOST_CC_RUN ?= $(HOST_CC)
endif

.PHONY: host-tests host-test-rsp host-test-rsp-console host-test-register-bank host-test-thread-control host-test-console-queue host-test-dipsw-probe-record host-test-dipsw-set-restore-record host-test-dipsw-dbgvcr-record

host-tests: host-test-rsp host-test-rsp-console host-test-register-bank host-test-thread-control host-test-console-queue host-test-dipsw-probe-record host-test-dipsw-set-restore-record host-test-dipsw-dbgvcr-record

host-test-rsp: test-rsp$(HOST_EXEEXT)
	./test-rsp$(HOST_EXEEXT)

host-test-rsp-console: test-rsp-console$(HOST_EXEEXT)
	./test-rsp-console$(HOST_EXEEXT)

host-test-register-bank: test-register-bank$(HOST_EXEEXT)
	./test-register-bank$(HOST_EXEEXT)

host-test-thread-control: test-thread-control$(HOST_EXEEXT)
	./test-thread-control$(HOST_EXEEXT)

host-test-console-queue: test-console-queue$(HOST_EXEEXT)
	./test-console-queue$(HOST_EXEEXT)

host-test-dipsw-probe-record: kernel/dipsw-read-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-read-probe/test-record$(HOST_EXEEXT)

host-test-dipsw-set-restore-record: kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT)

host-test-dipsw-dbgvcr-record: kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT)

test-rsp$(HOST_EXEEXT): uvdb_rsp.c uvdb_rsp.h tests/host/test_rsp_registers.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) uvdb_rsp.c tests/host/test_rsp_registers.c -o $@

test-rsp-console$(HOST_EXEEXT): uvdb_rsp.c uvdb_rsp.h tests/host/test_rsp_console.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) uvdb_rsp.c tests/host/test_rsp_console.c -o $@

test-register-bank$(HOST_EXEEXT): uvdb_registers.c uvdb_registers.h tests/host/test_register_bank.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) uvdb_registers.c tests/host/test_register_bank.c -o $@

test-thread-control$(HOST_EXEEXT): uvdb_thread_control.c uvdb_thread_control.h tests/host/test_thread_control.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) uvdb_thread_control.c tests/host/test_thread_control.c -o $@

test-console-queue$(HOST_EXEEXT): uvdb_console.c uvdb_console.h tests/host/test_console_queue.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DUVDB_CONSOLE_TESTING uvdb_console.c tests/host/test_console_queue.c $(HOST_THREAD_FLAGS) -o $@

kernel/dipsw-read-probe/test-record$(HOST_EXEEXT): kernel/dipsw-read-probe/test_record.c kernel/dipsw-read-probe/include/vd_dipsw_probe_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-read-probe/test_record.c -o $@

kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT): kernel/dipsw-set-restore-probe/test_record.c kernel/dipsw-set-restore-probe/include/vd_dipsw_set_probe_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-set-restore-probe/test_record.c -o $@

kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT): kernel/dipsw-dbgvcr-probe/test_record.c kernel/dipsw-dbgvcr-probe/include/vd_dipsw_dbgvcr_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-dbgvcr-probe/test_record.c -o $@

test.elf: psvDebugScreen.o test.o tests/thumb_step_returns.o libuvdb.a
	arm-vita-eabi-gcc $^ $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@

param.sfo:
	vita-mksfoex -s TITLE_ID=SLRS00001 'UVDB test' $@

test.velf: test.elf
	vita-elf-create $< $@

eboot.bin: test.velf
	vita-make-fself $< $@

uvdb-test.vpk: eboot.bin param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@
