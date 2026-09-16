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
CONTROL_SOURCES := broker/src/vitadebug_attach_control.c broker/src/vitadebug_attach_control_wire.c
LISTENER_SOURCES := $(CONTROL_SOURCES) broker/src/vitadebug_attach_listener.c
LOADER_SOURCES := $(CONTROL_SOURCES) broker/src/vitadebug_attach_loader.c
BROKER_TEST := test-attach-broker$(HOST_EXEEXT)
CONTROL_TEST := test-attach-control$(HOST_EXEEXT)
LISTENER_TEST := test-attach-listener$(HOST_EXEEXT)
LOADER_TEST := test-attach-loader$(HOST_EXEEXT)

.PHONY: host-test python-test broker-test control-test listener-test loader-test vita-lib clean

host-test: broker-test control-test listener-test loader-test python-test

python-test:
	$(PYTHON) -m unittest discover -s tests -v

broker-test: $(BROKER_TEST)
	./$(BROKER_TEST)

control-test: $(CONTROL_TEST)
	./$(CONTROL_TEST)

listener-test: $(LISTENER_TEST)
	./$(LISTENER_TEST)

loader-test: $(LOADER_TEST)
	./$(LOADER_TEST)

$(BROKER_TEST): $(BROKER_SOURCES) broker/tests/test_broker.c \
		broker/include/vitadebug_attach_broker.h \
		broker/include/vitadebug_attach_vita.h \
		include/vitadebug_attach_protocol.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(BROKER_INCLUDES) $(BROKER_SOURCES) broker/tests/test_broker.c -o $@

$(CONTROL_TEST): $(CONTROL_SOURCES) broker/tests/test_control.c \
		broker/include/vitadebug_attach_control.h \
		broker/include/vitadebug_attach_control_wire.h \
		broker/tests/control_golden_vectors.h \
		broker/include/vitadebug_attach_broker.h \
		include/vitadebug_attach_protocol.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(BROKER_INCLUDES) $(CONTROL_SOURCES) broker/tests/test_control.c -o $@

$(LISTENER_TEST): $(LISTENER_SOURCES) broker/tests/test_listener.c \
		broker/include/vitadebug_attach_listener.h \
		broker/include/vitadebug_attach_control.h \
		broker/include/vitadebug_attach_control_wire.h \
		broker/include/vitadebug_attach_broker.h \
		include/vitadebug_attach_protocol.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(BROKER_INCLUDES) $(LISTENER_SOURCES) broker/tests/test_listener.c -o $@

$(LOADER_TEST): $(LOADER_SOURCES) broker/tests/test_loader.c \
		broker/include/vitadebug_attach_loader.h \
		broker/include/vitadebug_attach_control.h \
		broker/include/vitadebug_attach_control_wire.h \
		broker/include/vitadebug_attach_broker.h \
		include/vitadebug_attach_protocol.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(BROKER_INCLUDES) $(LOADER_SOURCES) broker/tests/test_loader.c -o $@

# This proves the allocation-free read-only broker, authenticated-control and
# framing models, fixed-loader journal adapter, and fail-closed Vita inventory
# adapter compile for ARM. It intentionally does not build or install a
# resident module, socket listener, crypto backend, or Vita lifecycle backend.
vita-lib: libvitadebug_attach_broker.a

broker-vita-core.o: broker/src/vitadebug_attach_broker.c broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-adapter.o: broker/src/vitadebug_attach_vita.c broker/include/vitadebug_attach_vita.h broker/include/vitadebug_attach_broker.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-control.o: broker/src/vitadebug_attach_control.c broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_control_wire.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-control-wire.o: broker/src/vitadebug_attach_control_wire.c broker/include/vitadebug_attach_control_wire.h broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-listener.o: broker/src/vitadebug_attach_listener.c broker/include/vitadebug_attach_listener.h broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-loader.o: broker/src/vitadebug_attach_loader.c broker/include/vitadebug_attach_loader.h broker/include/vitadebug_attach_control_wire.h broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

libvitadebug_attach_broker.a: broker-vita-core.o broker-vita-adapter.o broker-vita-control.o broker-vita-control-wire.o broker-vita-listener.o broker-vita-loader.o
	arm-vita-eabi-ar rcs $@ $^

clean:
	rm -f test-attach-broker test-attach-broker.exe test-attach-control test-attach-control.exe test-attach-listener test-attach-listener.exe test-attach-loader test-attach-loader.exe broker-vita-core.o broker-vita-adapter.o broker-vita-control.o broker-vita-control-wire.o broker-vita-listener.o broker-vita-loader.o listener-check.o loader-check.o control-check.o libvitadebug_attach_broker.a
