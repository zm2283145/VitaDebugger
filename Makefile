all: libuvdb.a

clean:
	rm -f *.o *.a *.elf *.velf eboot.bin param.sfo *.vpk *.psp2dmp

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
EXTRA_LDFLAGS := $(CFLAGS) -Wl,-q -L $(KUBRIDGE_LIB_DIR) -lSceDisplay_stub -lSceNetPs_stub -lkubridge_stub -pthread

ifeq ($(UVDB_KERNEL_THREAD_CONTROL),1)
override CFLAGS += -DUVDB_KERNEL_THREAD_CONTROL -I $(VITADEBUG_KERNEL_DIR)/include
EXTRA_LDFLAGS += $(VITADEBUG_KERNEL_BUILD_DIR)/vitadebug_stubs/libvitadebug_kernel_stub.a
endif

override CFLAGS += -I $(KUBRIDGE_DIR) -Wall -Wextra -g

%.o: %.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) -c -o $@

test.o: test.c *.h
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

psvDebugScreen.o: $(VITASDK)/share/gcc-arm-vita-eabi/samples/common/debugScreen.c
	arm-vita-eabi-gcc $< $(CFLAGS) $(EXTRA_CFLAGS) -c -o $@

libuvdb.a: uvdb.o stdio_redirect.o
	arm-vita-eabi-ar rcs $@ $^

test.elf: psvDebugScreen.o test.o libuvdb.a
	arm-vita-eabi-gcc $^ $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@

param.sfo:
	vita-mksfoex -s TITLE_ID=SLRS00001 'UVDB test' $@

test.velf: test.elf
	vita-elf-create $< $@

eboot.bin: test.velf
	vita-make-fself $< $@

uvdb-test.vpk: eboot.bin param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin $@
