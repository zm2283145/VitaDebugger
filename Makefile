all: libuvdb.a

clean:
	rm -f *.o src/*.o tests/*.o tests/vita/*.o *.a *.elf *.velf eboot.bin param.sfo *.vpk *.psp2dmp test-rsp test-rsp.exe test-rsp-hardening test-rsp-hardening.exe test-uvdb-core-integration test-uvdb-core-integration.exe test-uvdb-core-integration-kernel test-uvdb-core-integration-kernel.exe test-memory-transaction test-memory-transaction.exe test-protocol-gate test-protocol-gate.exe test-exception-guard test-exception-guard.exe test-exception-handlers test-exception-handlers.exe test-rsp-console test-rsp-console.exe test-register-bank test-register-bank.exe test-thread-control test-thread-control.exe test-monitor test-monitor.exe test-breakpoint-patch test-breakpoint-patch.exe test-exclusive-step test-exclusive-step.exe test-console-queue test-console-queue.exe test-console-transport test-console-transport.exe test-debugnet-lifecycle test-debugnet-lifecycle.exe test-vfp-policy test-vfp-policy.exe test-kernel-thread-mutation test-kernel-thread-mutation.exe test-kernel-thread-mutation-provider test-kernel-thread-mutation-provider.exe test-kernel-thread-gpr-gate-enabled test-kernel-thread-gpr-gate-enabled.exe test-kernel-pmu-session test-kernel-pmu-session.exe test-kernel-pmu-backend test-kernel-pmu-backend.exe test-kernel-pmu-profiler-bridge test-kernel-pmu-profiler-bridge.exe test-kernel-pmu-profiler-real-events-disabled test-kernel-pmu-profiler-real-events-disabled.exe test-kernel-pmu-profiler-real-events test-kernel-pmu-profiler-real-events.exe test-kernel-pmu-profiler-safe-rearm test-kernel-pmu-profiler-safe-rearm.exe test-pmu-failure-matrix test-pmu-failure-matrix.exe test-pmu-lifecycle-gate-record test-pmu-lifecycle-gate-record.exe test-pmu-thread-exit-gate-record test-pmu-thread-exit-gate-record.exe kernel/dipsw-read-probe/test-record kernel/dipsw-read-probe/test-record.exe kernel/dipsw-set-restore-probe/test-record kernel/dipsw-set-restore-probe/test-record.exe kernel/dipsw-dbgvcr-probe/test-record kernel/dipsw-dbgvcr-probe/test-record.exe kernel/pmu-session-probe/test-record kernel/pmu-session-probe/test-record.exe kernel/thread-setter-resolver-probe/test-record kernel/thread-setter-resolver-probe/test-record.exe $(UVDB_ASLR_FIXTURE_OBJECT) $(UVDB_ASLR_FIXTURE_ELF) $(UVDB_ASLR_FIXTURE_VELF) $(UVDB_ASLR_FIXTURE_SELF)
	$(MAKE) -C profiler clean
	$(MAKE) -C attach clean

package: uvdb-test.vpk

deploy: package
	curl -v -T uvdb-test.vpk ftp://$(VITA_IP):1337/ux0:/uvdb-test.vpk 

fetch_dumps:
	curl ftp://$(VITA_IP):1337/ux0:/data/ | grep -o 'psp2core-.*' | while read line; do curl "ftp://$(VITA_IP):1337/ux0:/data/$$line" > "$$line"; curl -v "ftp://$(VITA_IP):1337/" -Q "DELE ux0:/data/$$line" >/dev/null; done

DEBUGNET_GATE_HOST ?=
DEBUGNET_GATE_PORT ?= 18194

.PHONY: debugnet-exit-gate debugnet-exit-gate-clean

debugnet-exit-gate:
	$(MAKE) -C tests/vita/debugnet-exit-gate package \
		DEBUGNET_GATE_HOST="$(DEBUGNET_GATE_HOST)" \
		DEBUGNET_GATE_PORT="$(DEBUGNET_GATE_PORT)"

debugnet-exit-gate-clean:
	$(MAKE) -C tests/vita/debugnet-exit-gate clean

KUBRIDGE_DIR ?= ../kubridge-review
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local
VITADEBUG_KERNEL_DIR ?= kernel
VITADEBUG_KERNEL_BUILD_DIR ?= $(VITADEBUG_KERNEL_DIR)/build
UVDB_ASLR_FIXTURE_BUILD_DIR ?= build-aslr-fixture
UVDB_ASLR_FIXTURE_OBJECT := $(UVDB_ASLR_FIXTURE_BUILD_DIR)/uvdb_aslr_fixture.o
UVDB_ASLR_FIXTURE_ELF := $(UVDB_ASLR_FIXTURE_BUILD_DIR)/uvdb_aslr_fixture.elf
UVDB_ASLR_FIXTURE_VELF := $(UVDB_ASLR_FIXTURE_BUILD_DIR)/uvdb_aslr_fixture.velf
UVDB_ASLR_FIXTURE_SELF := $(UVDB_ASLR_FIXTURE_BUILD_DIR)/uvdb_aslr_fixture.suprx

EXTRA_CFLAGS := -O0 -g -Wall -Wextra -I $(VITASDK)/share/gcc-arm-vita-eabi/samples/common -I $(KUBRIDGE_DIR)
EXTRA_LDFLAGS := $(CFLAGS) -Wl,-q -L $(KUBRIDGE_LIB_DIR) -lSceDisplay_stub -lSceNet_stub -lSceNetPs_stub -lSceKernelModulemgr_stub -lkubridge_stub -pthread

ifdef UVDB_DEBUGNET_HOST
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_HOST=\"$(UVDB_DEBUGNET_HOST)\"
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_PORT=$(or $(UVDB_DEBUGNET_PORT),18194)
endif

ifeq ($(UVDB_DEBUGNET_LIFECYCLE_TEST),1)
override EXTRA_CFLAGS += -DUVDB_DEBUGNET_LIFECYCLE_TEST
endif

ifeq ($(UVDB_GDB_CONSOLE_TEST),1)
override EXTRA_CFLAGS += -DUVDB_GDB_CONSOLE_TEST
endif

ifeq ($(UVDB_MONITOR_DISPLAY),1)
override CFLAGS += -DUVDB_MONITOR_DISPLAY
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
UVDB_VFP_OBJECTS := src/uvdb_vfp_policy.o
endif

ifeq ($(UVDB_GDB_VFP_FIXTURE),1)
ifneq ($(UVDB_KERNEL_VFP_READS),1)
$(error UVDB_GDB_VFP_FIXTURE=1 requires UVDB_KERNEL_VFP_READS=1)
endif
override EXTRA_CFLAGS += -DUVDB_GDB_VFP_FIXTURE
endif

ifeq ($(UVDB_GDB_ASLR_FIXTURE),1)
ifneq ($(UVDB_KERNEL_THREAD_CONTROL),1)
$(error UVDB_GDB_ASLR_FIXTURE=1 requires UVDB_KERNEL_THREAD_CONTROL=1)
endif
override EXTRA_CFLAGS += -DUVDB_GDB_ASLR_FIXTURE
UVDB_PACKAGE_EXTRA_PREREQUISITES += $(UVDB_ASLR_FIXTURE_SELF)
UVDB_PACKAGE_EXTRA_ARGS += -a $(UVDB_ASLR_FIXTURE_SELF)=module/uvdb_aslr_fixture.suprx
endif

override CFLAGS += -Isrc -I. -I $(KUBRIDGE_DIR) -Wall -Wextra -g

src/%.o: src/%.c src/*.h uvdb.h
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

# This compact example Makefile shares object names across plain, kernel, and
# VFP configurations. Always rebuild the two configuration-sensitive objects so
# changing feature flags cannot silently reuse an incompatible prior object.
.PHONY: force-feature-objects
force-feature-objects:

src/uvdb.o tests/vita/test.o: force-feature-objects

ifeq ($(UVDB_KERNEL_THREAD_CONTROL),1)
src/uvdb.o tests/vita/test.o: $(VITADEBUG_KERNEL_DIR)/include/vitadebug_kernel.h
endif

src/uvdb.o: protocol/arm_vfp_target_xml.inc

src/uvdb_vfp_policy.o: src/uvdb_vfp_policy.c src/uvdb_vfp_policy.h $(VITADEBUG_KERNEL_DIR)/include/vitadebug_kernel.h

tests/vita/test.o: tests/vita/test.c uvdb.h src/*.h
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

psvDebugScreen.o: $(VITASDK)/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

tests/vita/thumb_step_returns.o: tests/vita/thumb_step_returns.S
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

libuvdb.a: src/uvdb.o src/uvdb_registers.o src/uvdb_rsp.o src/uvdb_rsp_frame.o src/uvdb_fileio_flow.o src/uvdb_memory_transaction.o src/uvdb_protocol_gate.o src/uvdb_exception_guard.o src/uvdb_exception_handlers.o src/uvdb_thread_control.o src/uvdb_monitor.o src/uvdb_breakpoint_patch.o src/uvdb_exclusive_step.o $(UVDB_VFP_OBJECTS) src/uvdb_console.o src/uvdb_console_transport.o src/uvdb_debugnet.o src/stdio_redirect.o
	arm-vita-eabi-ar rcs $@ $^

HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Isrc -I.
ifeq ($(OS),Windows_NT)
HOST_THREAD_FLAGS ?= -pthread -static
HOST_EXEEXT ?= .exe
HOST_CC_RUN ?= powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tools/invoke-vita-env.ps1 $(HOST_CC)
HOST_PYTHON ?= py -3
else
HOST_THREAD_FLAGS ?= -pthread
HOST_EXEEXT ?=
HOST_CC_RUN ?= $(HOST_CC)
HOST_PYTHON ?= python3
endif

.PHONY: host-tests host-test-rsp host-test-rsp-hardening host-test-rsp-register-validation host-test-uvdb-core-integration host-test-uvdb-core-integration-kernel host-test-memory-transaction host-test-protocol-gate host-test-exception-guard host-test-exception-handlers host-test-rsp-console host-test-register-bank host-test-thread-control host-test-monitor host-test-breakpoint-patch host-test-exclusive-step host-test-vfp-policy host-test-kernel-thread-mutation host-test-kernel-thread-mutation-provider host-test-kernel-thread-gpr-gate-enabled host-test-kernel-pmu-session host-test-kernel-pmu-backend host-test-kernel-pmu-profiler-bridge host-test-kernel-pmu-profiler-real-events-disabled host-test-kernel-pmu-profiler-real-events host-test-kernel-pmu-profiler-safe-rearm host-test-pmu-failure-matrix host-test-pmu-profiler-gate-record host-test-pmu-lifecycle-gate-record host-test-pmu-thread-exit-gate-record host-test-pmu-session-probe-record host-test-thread-setter-resolver-record host-test-console-queue host-test-console-transport host-test-debugnet-lifecycle host-test-symbols host-test-vfp-lifecycle host-test-aslr-lifecycle host-test-live-gates host-test-target-xml host-test-dipsw-probe-record host-test-dipsw-set-restore-record host-test-dipsw-dbgvcr-record host-test-profiler host-test-attach aslr-fixture

host-tests: host-test-rsp host-test-rsp-hardening host-test-rsp-register-validation host-test-uvdb-core-integration host-test-uvdb-core-integration-kernel host-test-memory-transaction host-test-protocol-gate host-test-exception-guard host-test-exception-handlers host-test-rsp-console host-test-register-bank host-test-thread-control host-test-monitor host-test-breakpoint-patch host-test-exclusive-step host-test-vfp-policy host-test-kernel-thread-mutation host-test-kernel-thread-mutation-provider host-test-kernel-thread-gpr-gate-enabled host-test-kernel-pmu-session host-test-kernel-pmu-backend host-test-kernel-pmu-profiler-bridge host-test-kernel-pmu-profiler-real-events-disabled host-test-kernel-pmu-profiler-real-events host-test-kernel-pmu-profiler-safe-rearm host-test-pmu-failure-matrix host-test-pmu-profiler-gate-record host-test-pmu-lifecycle-gate-record host-test-pmu-thread-exit-gate-record host-test-pmu-session-probe-record host-test-thread-setter-resolver-record host-test-console-queue host-test-console-transport host-test-debugnet-lifecycle host-test-symbols host-test-vfp-lifecycle host-test-aslr-lifecycle host-test-live-gates host-test-target-xml host-test-dipsw-probe-record host-test-dipsw-set-restore-record host-test-dipsw-dbgvcr-record host-test-profiler host-test-attach

host-test-rsp: test-rsp$(HOST_EXEEXT)
	./test-rsp$(HOST_EXEEXT)

host-test-rsp-hardening: test-rsp-hardening$(HOST_EXEEXT)
	./test-rsp-hardening$(HOST_EXEEXT)

host-test-rsp-register-validation:
	$(HOST_PYTHON) -m unittest tests.host.test_rsp_register_validation

host-test-uvdb-core-integration: test-uvdb-core-integration$(HOST_EXEEXT)
	./test-uvdb-core-integration$(HOST_EXEEXT)

host-test-uvdb-core-integration-kernel: test-uvdb-core-integration-kernel$(HOST_EXEEXT)
	./test-uvdb-core-integration-kernel$(HOST_EXEEXT)

host-test-memory-transaction: test-memory-transaction$(HOST_EXEEXT)
	./test-memory-transaction$(HOST_EXEEXT)

host-test-protocol-gate: test-protocol-gate$(HOST_EXEEXT)
	./test-protocol-gate$(HOST_EXEEXT)

host-test-exception-guard: test-exception-guard$(HOST_EXEEXT)
	./test-exception-guard$(HOST_EXEEXT)

host-test-exception-handlers: test-exception-handlers$(HOST_EXEEXT)
	./test-exception-handlers$(HOST_EXEEXT)

host-test-rsp-console: test-rsp-console$(HOST_EXEEXT)
	./test-rsp-console$(HOST_EXEEXT)

host-test-register-bank: test-register-bank$(HOST_EXEEXT)
	./test-register-bank$(HOST_EXEEXT)

host-test-thread-control: test-thread-control$(HOST_EXEEXT)
	./test-thread-control$(HOST_EXEEXT)

host-test-monitor: test-monitor$(HOST_EXEEXT)
	./test-monitor$(HOST_EXEEXT)

host-test-breakpoint-patch: test-breakpoint-patch$(HOST_EXEEXT)
	./test-breakpoint-patch$(HOST_EXEEXT)

host-test-exclusive-step: test-exclusive-step$(HOST_EXEEXT)
	./test-exclusive-step$(HOST_EXEEXT)

host-test-vfp-policy: test-vfp-policy$(HOST_EXEEXT)
	./test-vfp-policy$(HOST_EXEEXT)

host-test-kernel-thread-mutation: test-kernel-thread-mutation$(HOST_EXEEXT)
	./test-kernel-thread-mutation$(HOST_EXEEXT)

host-test-kernel-thread-mutation-provider: test-kernel-thread-mutation-provider$(HOST_EXEEXT)
	./test-kernel-thread-mutation-provider$(HOST_EXEEXT)

host-test-kernel-thread-gpr-gate-enabled: test-kernel-thread-gpr-gate-enabled$(HOST_EXEEXT)
	./test-kernel-thread-gpr-gate-enabled$(HOST_EXEEXT)

host-test-kernel-pmu-session: test-kernel-pmu-session$(HOST_EXEEXT)
	./test-kernel-pmu-session$(HOST_EXEEXT)

host-test-kernel-pmu-backend: test-kernel-pmu-backend$(HOST_EXEEXT)
	./test-kernel-pmu-backend$(HOST_EXEEXT)

host-test-kernel-pmu-profiler-bridge: test-kernel-pmu-profiler-bridge$(HOST_EXEEXT)
	./test-kernel-pmu-profiler-bridge$(HOST_EXEEXT)

host-test-kernel-pmu-profiler-real-events-disabled: test-kernel-pmu-profiler-real-events-disabled$(HOST_EXEEXT)
	./test-kernel-pmu-profiler-real-events-disabled$(HOST_EXEEXT)

host-test-kernel-pmu-profiler-real-events: test-kernel-pmu-profiler-real-events$(HOST_EXEEXT)
	./test-kernel-pmu-profiler-real-events$(HOST_EXEEXT)

host-test-kernel-pmu-profiler-safe-rearm: test-kernel-pmu-profiler-safe-rearm$(HOST_EXEEXT)
	./test-kernel-pmu-profiler-safe-rearm$(HOST_EXEEXT)

host-test-pmu-failure-matrix: test-pmu-failure-matrix$(HOST_EXEEXT)
	./test-pmu-failure-matrix$(HOST_EXEEXT)

host-test-pmu-profiler-gate-record: test-pmu-profiler-gate-record$(HOST_EXEEXT)
	./test-pmu-profiler-gate-record$(HOST_EXEEXT)

host-test-pmu-lifecycle-gate-record: test-pmu-lifecycle-gate-record$(HOST_EXEEXT)
	./test-pmu-lifecycle-gate-record$(HOST_EXEEXT)

host-test-pmu-thread-exit-gate-record: test-pmu-thread-exit-gate-record$(HOST_EXEEXT)
	./test-pmu-thread-exit-gate-record$(HOST_EXEEXT)

host-test-pmu-session-probe-record: kernel/pmu-session-probe/test-record$(HOST_EXEEXT)
	./kernel/pmu-session-probe/test-record$(HOST_EXEEXT)

host-test-thread-setter-resolver-record: kernel/thread-setter-resolver-probe/test-record$(HOST_EXEEXT)
	./kernel/thread-setter-resolver-probe/test-record$(HOST_EXEEXT)
	$(HOST_PYTHON) -m unittest discover -s kernel/thread-setter-resolver-probe -p test_decode_record.py -v

host-test-console-queue: test-console-queue$(HOST_EXEEXT)
	./test-console-queue$(HOST_EXEEXT)

host-test-console-transport: test-console-transport$(HOST_EXEEXT)
	./test-console-transport$(HOST_EXEEXT)

host-test-debugnet-lifecycle: test-debugnet-lifecycle$(HOST_EXEEXT)
	./test-debugnet-lifecycle$(HOST_EXEEXT)
	./test-debugnet-lifecycle$(HOST_EXEEXT) --process-exit-after-stop
	./test-debugnet-lifecycle$(HOST_EXEEXT) --process-exit-full
	./test-debugnet-lifecycle$(HOST_EXEEXT) --process-exit-fallback
	./test-debugnet-lifecycle$(HOST_EXEEXT) --process-exit-contention

host-test-symbols:
	$(HOST_PYTHON) -m unittest \
		tests.host.test_gdb_symbols \
		tests.host.test_gdb_symbols_connect_wait \
		tests.host.test_gdb_build_identity \
		tests.host.test_gdb_symbol_refresh \
		tests.host.test_vscode_demo_session

host-test-vfp-lifecycle:
	$(HOST_PYTHON) -m unittest tests.host.test_gdb_vfp_lifecycle

host-test-aslr-lifecycle:
	$(HOST_PYTHON) -m unittest tests.host.test_gdb_aslr_lifecycle

host-test-live-gates:
	$(HOST_PYTHON) -m unittest tests.host.test_gdb_monitor_smoke tests.host.test_gdb_step_register_gate

host-test-target-xml:
	$(HOST_PYTHON) -m unittest tests.host.test_target_xml

host-test-dipsw-probe-record: kernel/dipsw-read-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-read-probe/test-record$(HOST_EXEEXT)

host-test-dipsw-set-restore-record: kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT)

host-test-dipsw-dbgvcr-record: kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT)
	./kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT)

host-test-profiler:
	$(MAKE) -C profiler host-test HOST_CC=$(HOST_CC) PYTHON="$(HOST_PYTHON)"

host-test-attach:
	$(MAKE) -C attach host-test HOST_CC=$(HOST_CC) PYTHON="$(HOST_PYTHON)"

test-rsp$(HOST_EXEEXT): src/uvdb_rsp.c src/uvdb_rsp.h tests/host/test_rsp_registers.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_rsp.c tests/host/test_rsp_registers.c -o $@

test-rsp-hardening$(HOST_EXEEXT): src/uvdb_rsp.c src/uvdb_rsp.h src/uvdb_rsp_frame.c src/uvdb_rsp_frame.h src/uvdb_fileio_flow.c src/uvdb_fileio_flow.h src/uvdb_thread_control.c src/uvdb_thread_control.h tests/host/test_rsp_hardening.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_rsp.c src/uvdb_rsp_frame.c src/uvdb_fileio_flow.c src/uvdb_thread_control.c tests/host/test_rsp_hardening.c -o $@

test-uvdb-core-integration$(HOST_EXEEXT): src/uvdb.c tests/host/test_uvdb_core_integration.c tests/host/uvdb_host_platform.h src/uvdb_rsp.c src/uvdb_rsp_frame.c src/uvdb_fileio_flow.c src/uvdb_thread_control.c src/uvdb_console_transport.c src/uvdb_console.c src/uvdb_protocol_gate.c src/uvdb_exception_guard.c src/uvdb_exception_handlers.c src/uvdb_breakpoint_patch.c src/uvdb_exclusive_step.c src/uvdb_monitor.c src/uvdb_registers.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -ffunction-sections -fdata-sections -Itests/host tests/host/test_uvdb_core_integration.c src/uvdb_rsp.c src/uvdb_rsp_frame.c src/uvdb_fileio_flow.c src/uvdb_thread_control.c src/uvdb_console_transport.c src/uvdb_console.c src/uvdb_protocol_gate.c src/uvdb_exception_guard.c src/uvdb_exception_handlers.c src/uvdb_breakpoint_patch.c src/uvdb_exclusive_step.c src/uvdb_monitor.c src/uvdb_registers.c $(HOST_THREAD_FLAGS) -Wl,--gc-sections -o $@

test-uvdb-core-integration-kernel$(HOST_EXEEXT): src/uvdb.c tests/host/test_uvdb_core_integration.c tests/host/uvdb_host_platform.h kernel/include/vitadebug_kernel.h src/uvdb_rsp.c src/uvdb_rsp_frame.c src/uvdb_fileio_flow.c src/uvdb_thread_control.c src/uvdb_console_transport.c src/uvdb_console.c src/uvdb_protocol_gate.c src/uvdb_exception_guard.c src/uvdb_exception_handlers.c src/uvdb_breakpoint_patch.c src/uvdb_exclusive_step.c src/uvdb_monitor.c src/uvdb_registers.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DUVDB_KERNEL_THREAD_CONTROL -Ikernel/include -Itests/host/include -ffunction-sections -fdata-sections -Itests/host tests/host/test_uvdb_core_integration.c src/uvdb_rsp.c src/uvdb_rsp_frame.c src/uvdb_fileio_flow.c src/uvdb_thread_control.c src/uvdb_console_transport.c src/uvdb_console.c src/uvdb_protocol_gate.c src/uvdb_exception_guard.c src/uvdb_exception_handlers.c src/uvdb_breakpoint_patch.c src/uvdb_exclusive_step.c src/uvdb_monitor.c src/uvdb_registers.c $(HOST_THREAD_FLAGS) -Wl,--gc-sections -o $@

test-memory-transaction$(HOST_EXEEXT): src/uvdb_memory_transaction.c src/uvdb_memory_transaction.h tests/host/test_memory_transaction.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_memory_transaction.c tests/host/test_memory_transaction.c -o $@

test-protocol-gate$(HOST_EXEEXT): src/uvdb_protocol_gate.c src/uvdb_protocol_gate.h tests/host/test_protocol_gate.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_protocol_gate.c tests/host/test_protocol_gate.c $(HOST_THREAD_FLAGS) -o $@

test-exception-guard$(HOST_EXEEXT): src/uvdb_exception_guard.c src/uvdb_exception_guard.h tests/host/test_exception_guard.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_exception_guard.c tests/host/test_exception_guard.c $(HOST_THREAD_FLAGS) -o $@

test-exception-handlers$(HOST_EXEEXT): src/uvdb_exception_handlers.c src/uvdb_exception_handlers.h tests/host/test_exception_handlers.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_exception_handlers.c tests/host/test_exception_handlers.c -o $@

test-rsp-console$(HOST_EXEEXT): src/uvdb_rsp.c src/uvdb_rsp.h tests/host/test_rsp_console.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_rsp.c tests/host/test_rsp_console.c -o $@

test-register-bank$(HOST_EXEEXT): src/uvdb_registers.c src/uvdb_registers.h tests/host/test_register_bank.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_registers.c tests/host/test_register_bank.c -o $@

test-thread-control$(HOST_EXEEXT): src/uvdb_thread_control.c src/uvdb_thread_control.h tests/host/test_thread_control.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_thread_control.c tests/host/test_thread_control.c -o $@

test-monitor$(HOST_EXEEXT): src/uvdb_monitor.c src/uvdb_monitor.h tests/host/test_monitor.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_monitor.c tests/host/test_monitor.c -o $@

test-breakpoint-patch$(HOST_EXEEXT): src/uvdb_breakpoint_patch.c src/uvdb_breakpoint_patch.h tests/host/test_breakpoint_patch.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_breakpoint_patch.c tests/host/test_breakpoint_patch.c -o $@

test-exclusive-step$(HOST_EXEEXT): src/uvdb_exclusive_step.c src/uvdb_exclusive_step.h tests/host/test_exclusive_step.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) src/uvdb_exclusive_step.c tests/host/test_exclusive_step.c -o $@

test-vfp-policy$(HOST_EXEEXT): src/uvdb_vfp_policy.c src/uvdb_vfp_policy.h kernel/include/vitadebug_kernel.h tests/host/test_vfp_policy.c tests/host/include/psp2/types.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -Itests/host/include -Ikernel/include src/uvdb_vfp_policy.c tests/host/test_vfp_policy.c -o $@

test-kernel-thread-mutation$(HOST_EXEEXT): kernel/src/thread_mutation.c kernel/src/thread_mutation.h kernel/include/vitadebug_kernel.h tests/host/test_kernel_thread_mutation.c tests/host/include/psp2/types.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -Itests/host/include -Ikernel/include -Ikernel/src kernel/src/thread_mutation.c tests/host/test_kernel_thread_mutation.c -o $@

test-kernel-thread-mutation-provider$(HOST_EXEEXT): kernel/src/thread_mutation.c kernel/src/thread_mutation.h kernel/src/thread_mutation_provider.c kernel/src/thread_mutation_provider.h kernel/src/thread_gpr_gate.c kernel/src/thread_gpr_gate.h kernel/include/vitadebug_kernel.h tests/host/test_kernel_thread_mutation_provider.c tests/host/include/psp2/types.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DVD_THREAD_GPR_GATE_COMPILED=0 -DVD_TEST_EXPECT_GPR_GATE_COMPILED=0 -Itests/host/include -Ikernel/include -Ikernel/src kernel/src/thread_mutation.c kernel/src/thread_mutation_provider.c kernel/src/thread_gpr_gate.c tests/host/test_kernel_thread_mutation_provider.c -o $@

test-kernel-thread-gpr-gate-enabled$(HOST_EXEEXT): kernel/src/thread_mutation.c kernel/src/thread_mutation.h kernel/src/thread_mutation_provider.c kernel/src/thread_mutation_provider.h kernel/src/thread_gpr_gate.c kernel/src/thread_gpr_gate.h kernel/include/vitadebug_kernel.h tests/host/test_kernel_thread_mutation_provider.c tests/host/include/psp2/types.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DVD_THREAD_GPR_GATE_COMPILED=1 -DVD_TEST_EXPECT_GPR_GATE_COMPILED=1 -Itests/host/include -Ikernel/include -Ikernel/src kernel/src/thread_mutation.c kernel/src/thread_mutation_provider.c kernel/src/thread_gpr_gate.c tests/host/test_kernel_thread_mutation_provider.c -o $@

test-kernel-pmu-session$(HOST_EXEEXT): kernel/src/pmu_session.c kernel/src/pmu_session.h tests/host/test_kernel_pmu_session.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -Ikernel/src kernel/src/pmu_session.c tests/host/test_kernel_pmu_session.c -o $@

test-kernel-pmu-backend$(HOST_EXEEXT): kernel/src/pmu_backend.c kernel/src/pmu_backend.h kernel/src/pmu_backend_host_test.h kernel/src/pmu_session.c kernel/src/pmu_session.h tests/host/test_kernel_pmu_backend.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_PMU_BACKEND_HOST_TEST=1 -Ikernel/src kernel/src/pmu_backend.c kernel/src/pmu_session.c tests/host/test_kernel_pmu_backend.c -o $@

test-kernel-pmu-profiler-bridge$(HOST_EXEEXT): kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_bridge.h kernel/src/pmu_profiler_transport.c kernel/src/pmu_profiler_transport.h kernel/src/pmu_backend.c kernel/src/pmu_backend.h kernel/src/pmu_backend_host_test.h kernel/src/pmu_session.c kernel/src/pmu_session.h kernel/include/vitadebug_pmu_profiler.h profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c profiler/include/vitaprofiler_pmu.h profiler/include/vitaprofiler.h tests/host/test_kernel_pmu_profiler_bridge.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 -DVD_TEST_EXPECT_REAL_EVENTS_COMPILED=0 -DVD_PMU_BACKEND_HOST_TEST=1 -Iprofiler/include -Ikernel/include -Ikernel/src kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_transport.c kernel/src/pmu_backend.c kernel/src/pmu_session.c profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c tests/host/test_kernel_pmu_profiler_bridge.c -o $@

test-kernel-pmu-profiler-real-events-disabled$(HOST_EXEEXT): kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_bridge.h kernel/src/pmu_profiler_transport.c kernel/src/pmu_profiler_transport.h kernel/src/pmu_backend.c kernel/src/pmu_backend.h kernel/src/pmu_backend_host_test.h kernel/src/pmu_session.c kernel/src/pmu_session.h kernel/include/vitadebug_pmu_profiler.h profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c profiler/include/vitaprofiler_pmu.h profiler/include/vitaprofiler.h tests/host/test_kernel_pmu_profiler_bridge.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=0 -DVD_TEST_EXPECT_REAL_EVENTS_COMPILED=0 -DVD_PMU_BACKEND_HOST_TEST=1 -Iprofiler/include -Ikernel/include -Ikernel/src kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_transport.c kernel/src/pmu_backend.c kernel/src/pmu_session.c profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c tests/host/test_kernel_pmu_profiler_bridge.c -o $@

test-kernel-pmu-profiler-real-events$(HOST_EXEEXT): kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_bridge.h kernel/src/pmu_profiler_transport.c kernel/src/pmu_profiler_transport.h kernel/src/pmu_backend.c kernel/src/pmu_backend.h kernel/src/pmu_backend_host_test.h kernel/src/pmu_session.c kernel/src/pmu_session.h kernel/include/vitadebug_pmu_profiler.h profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c profiler/include/vitaprofiler_pmu.h profiler/include/vitaprofiler.h tests/host/test_kernel_pmu_profiler_bridge.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1 -DVD_TEST_EXPECT_REAL_EVENTS_COMPILED=1 -DVD_PMU_BACKEND_HOST_TEST=1 -Iprofiler/include -Ikernel/include -Ikernel/src kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_transport.c kernel/src/pmu_backend.c kernel/src/pmu_session.c profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c tests/host/test_kernel_pmu_profiler_bridge.c -o $@

test-kernel-pmu-profiler-safe-rearm$(HOST_EXEEXT): kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_bridge.h kernel/src/pmu_profiler_transport.c kernel/src/pmu_profiler_transport.h kernel/src/pmu_backend.c kernel/src/pmu_backend.h kernel/src/pmu_backend_host_test.h kernel/src/pmu_session.c kernel/src/pmu_session.h kernel/include/vitadebug_pmu_profiler.h profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c profiler/include/vitaprofiler_pmu.h profiler/include/vitaprofiler.h tests/host/test_kernel_pmu_profiler_bridge.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=1 -DVD_TEST_EXPECT_REAL_EVENTS_COMPILED=1 -DVD_TEST_EXPECT_SAFE_REARM_COMPILED=1 -DVD_PMU_BACKEND_HOST_TEST=1 -Iprofiler/include -Ikernel/include -Ikernel/src kernel/src/pmu_profiler_bridge.c kernel/src/pmu_profiler_transport.c kernel/src/pmu_backend.c kernel/src/pmu_session.c profiler/src/vitaprofiler.c profiler/src/vitaprofiler_names.c profiler/src/vitaprofiler_pmu.c tests/host/test_kernel_pmu_profiler_bridge.c -o $@

test-pmu-failure-matrix$(HOST_EXEEXT): kernel/src/pmu_profiler_transport.c kernel/src/pmu_profiler_transport.h kernel/include/vitadebug_pmu_profiler.h profiler/src/vitaprofiler_tcp_vita.c profiler/include/vitaprofiler_tcp_vita.h tests/host/test_pmu_failure_matrix.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1 -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=1 -Iprofiler/include -Ikernel/include -Ikernel/src kernel/src/pmu_profiler_transport.c profiler/src/vitaprofiler_tcp_vita.c tests/host/test_pmu_failure_matrix.c -o $@

test-pmu-profiler-gate-record$(HOST_EXEEXT): kernel/pmu-profiler-gate/journal.h tests/host/test_pmu_profiler_gate_record.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -Ikernel/pmu-profiler-gate tests/host/test_pmu_profiler_gate_record.c -o $@

test-pmu-lifecycle-gate-record$(HOST_EXEEXT): kernel/pmu-profiler-lifecycle-gate/journal.h tests/host/test_pmu_lifecycle_gate_record.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -Ikernel/pmu-profiler-lifecycle-gate tests/host/test_pmu_lifecycle_gate_record.c -o $@

test-pmu-thread-exit-gate-record$(HOST_EXEEXT): kernel/pmu-profiler-thread-exit-gate/journal.h kernel/include/vitadebug_pmu_profiler.h tests/host/test_pmu_thread_exit_gate_record.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -pedantic-errors -Ikernel/pmu-profiler-thread-exit-gate -Ikernel/include tests/host/test_pmu_thread_exit_gate_record.c -o $@

kernel/pmu-session-probe/test-record$(HOST_EXEEXT): kernel/pmu-session-probe/test_record.c kernel/pmu-session-probe/include/vitadebug_pmu_probe.h kernel/src/pmu_backend.h kernel/src/pmu_session.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 -Ikernel/pmu-session-probe/include -Ikernel/src kernel/pmu-session-probe/test_record.c -o $@

kernel/thread-setter-resolver-probe/test-record$(HOST_EXEEXT): kernel/thread-setter-resolver-probe/test_record.c kernel/thread-setter-resolver-probe/include/vd_thread_setter_resolver_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) -Ikernel/thread-setter-resolver-probe/include kernel/thread-setter-resolver-probe/test_record.c -o $@

test-console-queue$(HOST_EXEEXT): src/uvdb_console.c src/uvdb_console.h tests/host/test_console_queue.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DUVDB_CONSOLE_TESTING src/uvdb_console.c tests/host/test_console_queue.c $(HOST_THREAD_FLAGS) -o $@

test-console-transport$(HOST_EXEEXT): src/uvdb_console.c src/uvdb_console.h src/uvdb_rsp.c src/uvdb_rsp.h src/uvdb_console_transport.c src/uvdb_console_transport.h tests/host/test_console_transport.c
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DUVDB_CONSOLE_TESTING src/uvdb_console.c src/uvdb_rsp.c src/uvdb_console_transport.c tests/host/test_console_transport.c -o $@

UVDB_DEBUGNET_FAKE_HEADERS := \
	tests/host/debugnet_fake/include/arpa/inet.h \
	tests/host/debugnet_fake/include/netinet/in.h \
	tests/host/debugnet_fake/include/psp2/types.h \
	tests/host/debugnet_fake/include/psp2/kernel/error.h \
	tests/host/debugnet_fake/include/psp2/kernel/processmgr.h \
	tests/host/debugnet_fake/include/psp2/kernel/threadmgr/semaphore.h \
	tests/host/debugnet_fake/include/psp2/kernel/threadmgr/thread.h \
	tests/host/debugnet_fake/include/psp2/net/net_syscalls.h \
	tests/host/debugnet_fake/include/sys/socket.h

test-debugnet-lifecycle$(HOST_EXEEXT): src/uvdb_debugnet.c uvdb.h tests/host/test_debugnet_lifecycle.c $(UVDB_DEBUGNET_FAKE_HEADERS)
	$(HOST_CC_RUN) $(HOST_CFLAGS) -DUVDB_DEBUGNET_TESTING -Itests/host/debugnet_fake/include src/uvdb_debugnet.c tests/host/test_debugnet_lifecycle.c $(HOST_THREAD_FLAGS) -o $@

kernel/dipsw-read-probe/test-record$(HOST_EXEEXT): kernel/dipsw-read-probe/test_record.c kernel/dipsw-read-probe/include/vd_dipsw_probe_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-read-probe/test_record.c -o $@

kernel/dipsw-set-restore-probe/test-record$(HOST_EXEEXT): kernel/dipsw-set-restore-probe/test_record.c kernel/dipsw-set-restore-probe/include/vd_dipsw_set_probe_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-set-restore-probe/test_record.c -o $@

kernel/dipsw-dbgvcr-probe/test-record$(HOST_EXEEXT): kernel/dipsw-dbgvcr-probe/test_record.c kernel/dipsw-dbgvcr-probe/include/vd_dipsw_dbgvcr_record.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) kernel/dipsw-dbgvcr-probe/test_record.c -o $@

aslr-fixture: $(UVDB_ASLR_FIXTURE_SELF)

$(UVDB_ASLR_FIXTURE_BUILD_DIR):
ifeq ($(OS),Windows_NT)
	powershell.exe -NoLogo -NoProfile -NonInteractive -Command "[void][IO.Directory]::CreateDirectory('$@')"
else
	mkdir -p $@
endif

$(UVDB_ASLR_FIXTURE_OBJECT): tests/aslr_fixture/fixture.c tests/aslr_fixture/control.h | $(UVDB_ASLR_FIXTURE_BUILD_DIR)
	arm-vita-eabi-gcc -std=gnu11 -O0 -g -Wall -Wextra -Werror -fno-inline -fno-omit-frame-pointer -fvisibility=hidden -D__PSP2_USER__ -I tests/aslr_fixture -c $< -o $@

$(UVDB_ASLR_FIXTURE_ELF): $(UVDB_ASLR_FIXTURE_OBJECT)
	arm-vita-eabi-gcc -nostdlib -Wl,-q $< -lSceLibKernel_stub_weak -lSceKernelThreadMgr_stub_weak -o $@

$(UVDB_ASLR_FIXTURE_VELF): $(UVDB_ASLR_FIXTURE_ELF) tests/aslr_fixture/exports.yml
	vita-elf-create -e tests/aslr_fixture/exports.yml $< $@

$(UVDB_ASLR_FIXTURE_SELF): $(UVDB_ASLR_FIXTURE_VELF)
	vita-make-fself -c $< $@

test.elf: psvDebugScreen.o tests/vita/test.o tests/vita/thumb_step_returns.o libuvdb.a
	arm-vita-eabi-gcc $^ $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@

param.sfo:
	vita-mksfoex -s TITLE_ID=SLRS00001 'UVDB test' $@

test.velf: test.elf
	vita-elf-create $< $@

eboot.bin: test.velf
	vita-make-fself $< $@

uvdb-test.vpk: eboot.bin param.sfo $(UVDB_PACKAGE_EXTRA_PREREQUISITES)
	vita-pack-vpk -s param.sfo -b eboot.bin $(UVDB_PACKAGE_EXTRA_ARGS) $@
