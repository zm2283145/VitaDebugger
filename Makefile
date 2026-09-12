all: libuvdb.a

clean:
	rm -f *.o tests/*.o *.a *.elf *.velf eboot.bin param.sfo *.vpk *.psp2dmp

package: uvdb-test.vpk

deploy: package
	curl -v -T uvdb-test.vpk ftp://$(VITA_IP):1337/ux0:/uvdb-test.vpk 

fetch_dumps:
	curl ftp://$(VITA_IP):1337/ux0:/data/ | grep -o 'psp2core-.*' | while read line; do curl "ftp://$(VITA_IP):1337/ux0:/data/$$line" > "$$line"; curl -v "ftp://$(VITA_IP):1337/" -Q "DELE ux0:/data/$$line" >/dev/null; done

KUBRIDGE_DIR ?= ../kubridge-review
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local
VITADEBUG_KERNEL_DIR ?= kernel
VITADEBUG_KERNEL_BUILD_DIR ?= $(VITADEBUG_KERNEL_DIR)/build-short

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

uvdb.o: protocol/arm_vfp_target_xml.inc

test.o: test.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

psvDebugScreen.o: $(VITASDK)/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

tests/thumb_step_returns.o: tests/thumb_step_returns.S
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

libuvdb.a: uvdb.o uvdb_rsp.o uvdb_debugnet.o stdio_redirect.o
	arm-vita-eabi-ar rcs $@ $^

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
