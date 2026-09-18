HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
HOST_EXEEXT ?=

ifeq ($(OS),Windows_NT)
HOST_EXEEXT := .exe
HOST_CC_RUN ?= powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ../tools/invoke-vita-env.ps1 $(HOST_CC)
PYTHON ?= py -3
else
HOST_CC_RUN ?= $(HOST_CC)
PYTHON ?= python3
endif

BROKER_INCLUDES := -Iinclude -Ibroker/include
MONOCYPHER_DIR := ../deploy/agent/third_party/monocypher
AUTH_INCLUDES := $(BROKER_INCLUDES) -I$(MONOCYPHER_DIR)
BROKER_SOURCES := broker/src/vitadebug_attach_broker.c broker/src/vitadebug_attach_vita.c
CONTROL_SOURCES := broker/src/vitadebug_attach_control.c broker/src/vitadebug_attach_control_wire.c
AUTH_SOURCES := broker/src/vitadebug_attach_auth.c broker/src/vitadebug_attach_auth_vita.c \
	$(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher-ed25519.c
AUTH_LISTENER_SOURCES := broker/src/vitadebug_attach_auth.c \
	broker/src/vitadebug_attach_auth_listener.c \
	$(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher-ed25519.c
AUTH_STORE_SOURCES := broker/src/vitadebug_attach_auth.c \
	broker/src/vitadebug_attach_auth_store.c \
	broker/src/vitadebug_attach_auth_store_vita.c \
	$(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher-ed25519.c
BROKER_TEST := test-attach-broker$(HOST_EXEEXT)
CONTROL_TEST := test-attach-control$(HOST_EXEEXT)
AUTH_TEST := test-attach-auth$(HOST_EXEEXT)
AUTH_LISTENER_TEST := test-attach-auth-listener$(HOST_EXEEXT)
AUTH_STORE_TEST := test-attach-auth-store$(HOST_EXEEXT)

.PHONY: host-test python-test broker-test control-test auth-test \
	auth-listener-test auth-store-test vita-lib clean

host-test: broker-test control-test auth-test auth-listener-test \
	auth-store-test python-test

python-test:
	$(PYTHON) -m unittest discover -s tests -v

broker-test: $(BROKER_TEST)
	./$(BROKER_TEST)

control-test: $(CONTROL_TEST)
	./$(CONTROL_TEST)

auth-test: $(AUTH_TEST)
	./$(AUTH_TEST)

auth-listener-test: $(AUTH_LISTENER_TEST)
	./$(AUTH_LISTENER_TEST)

auth-store-test: $(AUTH_STORE_TEST)
	./$(AUTH_STORE_TEST)

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

$(AUTH_TEST): $(AUTH_SOURCES) broker/tests/test_auth.c \
		broker/include/vitadebug_attach_auth.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(AUTH_INCLUDES) $(AUTH_SOURCES) broker/tests/test_auth.c -o $@

$(AUTH_LISTENER_TEST): $(AUTH_LISTENER_SOURCES) broker/tests/test_auth_listener.c \
		broker/include/vitadebug_attach_auth_listener.h \
		broker/include/vitadebug_attach_auth.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(AUTH_INCLUDES) $(AUTH_LISTENER_SOURCES) broker/tests/test_auth_listener.c -o $@

$(AUTH_STORE_TEST): $(AUTH_STORE_SOURCES) broker/tests/test_auth_store.c \
		broker/include/vitadebug_attach_auth_store.h \
		broker/include/vitadebug_attach_auth.h
	$(HOST_CC_RUN) $(HOST_CFLAGS) $(AUTH_INCLUDES) $(AUTH_STORE_SOURCES) broker/tests/test_auth_store.c -o $@

# This proves the read-only broker, authenticated-control model, serialized
# authentication listener, persistent-store core, and Vita adapters compile
# for ARM. It does not install a service or add a target-process operation.
vita-lib: libvitadebug_attach_broker.a

broker-vita-core.o: broker/src/vitadebug_attach_broker.c broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-adapter.o: broker/src/vitadebug_attach_vita.c broker/include/vitadebug_attach_vita.h broker/include/vitadebug_attach_broker.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-control.o: broker/src/vitadebug_attach_control.c broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_control_wire.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-control-wire.o: broker/src/vitadebug_attach_control_wire.c broker/include/vitadebug_attach_control_wire.h broker/include/vitadebug_attach_control.h broker/include/vitadebug_attach_broker.h include/vitadebug_attach_protocol.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(BROKER_INCLUDES) -c $< -o $@

broker-vita-auth.o: broker/src/vitadebug_attach_auth.c broker/include/vitadebug_attach_auth.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-auth-adapter.o: broker/src/vitadebug_attach_auth_vita.c broker/include/vitadebug_attach_auth.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-auth-listener.o: broker/src/vitadebug_attach_auth_listener.c broker/include/vitadebug_attach_auth_listener.h broker/include/vitadebug_attach_auth.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-auth-listener-adapter.o: broker/src/vitadebug_attach_auth_listener_vita.c broker/include/vitadebug_attach_auth_listener_vita.h broker/include/vitadebug_attach_auth_listener.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-auth-store.o: broker/src/vitadebug_attach_auth_store.c broker/include/vitadebug_attach_auth_store.h broker/include/vitadebug_attach_auth.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-auth-store-adapter.o: broker/src/vitadebug_attach_auth_store_vita.c broker/include/vitadebug_attach_auth_store.h broker/include/vitadebug_attach_auth.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror $(AUTH_INCLUDES) -c $< -o $@

broker-vita-monocypher.o: $(MONOCYPHER_DIR)/monocypher.c $(MONOCYPHER_DIR)/monocypher.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror -I$(MONOCYPHER_DIR) -c $< -o $@

broker-vita-monocypher-ed25519.o: $(MONOCYPHER_DIR)/monocypher-ed25519.c $(MONOCYPHER_DIR)/monocypher-ed25519.h
	arm-vita-eabi-gcc -std=c11 -O2 -Wall -Wextra -Werror -I$(MONOCYPHER_DIR) -c $< -o $@

libvitadebug_attach_broker.a: broker-vita-core.o broker-vita-adapter.o broker-vita-control.o broker-vita-control-wire.o broker-vita-auth.o broker-vita-auth-adapter.o broker-vita-auth-listener.o broker-vita-auth-listener-adapter.o broker-vita-auth-store.o broker-vita-auth-store-adapter.o broker-vita-monocypher.o broker-vita-monocypher-ed25519.o
	arm-vita-eabi-ar rcs $@ $^

clean:
	rm -f test-attach-broker test-attach-broker.exe test-attach-control test-attach-control.exe test-attach-auth test-attach-auth.exe test-attach-auth-listener test-attach-auth-listener.exe test-attach-auth-store test-attach-auth-store.exe broker-vita-core.o broker-vita-adapter.o broker-vita-control.o broker-vita-control-wire.o broker-vita-auth.o broker-vita-auth-adapter.o broker-vita-auth-listener.o broker-vita-auth-listener-adapter.o broker-vita-auth-store.o broker-vita-auth-store-adapter.o broker-vita-monocypher.o broker-vita-monocypher-ed25519.o libvitadebug_attach_broker.a
