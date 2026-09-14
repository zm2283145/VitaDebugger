HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PYTHON ?= python3
HOST_EXEEXT ?=

ifeq ($(OS),Windows_NT)
HOST_EXEEXT := .exe
HOST_CC_RUN ?= powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ../tools/invoke-vita-env.ps1 $(HOST_CC)
else
HOST_CC_RUN ?= $(HOST_CC)
endif

BROKER_INCLUDES := -Iinclude -Ibroker/include
BROKER_SOURCES := broker/src/vitadebug_attach_broker.c broker/src/vitadebug_attach_vita.c
BROKER_TEST := test-attach-broker$(HOST_EXEEXT)

.PHONY: host-test python-test broker-test vita-lib clean

host-test: broker-test python-test

python-test:
	$(PYTHON) -m unittest discover -s tests -v

broker-test: $(BROKER_TEST)
	./$(BROKER_TEST)

$(BROKER_TEST): $(BROKER_SOURCES) broker/tests/test_broker.c \
		broker/include/vitadebug_attach_broker.h \
		broker/include/vitadebug_attach_vita.h \
		include/vitadebug_attach_protocol.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(BROKER_INCLUDES) $(BROKER_SOURCES) broker/tests/test_broker.c -o $@

# This proves the allocation-free broker core and fail-closed Vita adapter
# compile for ARM. It intentionally does not build or install a resident module
# or network listener.
vita-lib: libvitadebug_attach_broker.a

broker-vita-core.o: broker/src/vitadebug_attach_broker.c broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-adapter.o: broker/src/vitadebug_attach_vita.c broker/include/vitadebug_attach_vita.h broker/include/vitadebug_attach_broker.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

libvitadebug_attach_broker.a: broker-vita-core.o broker-vita-adapter.o
	arm-vita-eabi-ar rcs $@ $^

clean:
	rm -f test-attach-broker test-attach-broker.exe broker-vita-core.o broker-vita-adapter.o libvitadebug_attach_broker.a
