# SPDX-License-Identifier: MIT
CC ?= cc
PREFIX ?= /usr/local
LIBANE_DIR ?= /usr/include/libane
LIBANE_LIB ?= -lane
BUILD ?= build
CFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Werror -std=c11 -fPIC -Iinclude -I$(LIBANE_DIR)
LIBS = $(LIBANE_LIB) -lcrypto -ldl -lm -pthread
CORE = src/engine.c src/stream.c
.PHONY: all check install clean
all: $(BUILD)/libaurora_capture_ns.so
$(BUILD):
	mkdir -p $@
$(BUILD)/libaurora_capture_ns.so: $(CORE) src/ladspa.c include/capture_ns.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -shared $(CORE) src/ladspa.c $(LDFLAGS) $(LIBS) -o $@
$(BUILD)/test-stream: src/stream.c tests/test_stream.c include/capture_ns.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) src/stream.c tests/test_stream.c -lm -pthread -o $@
$(BUILD)/test-engine: src/engine.c tests/test_engine.c include/capture_ns.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) src/engine.c tests/test_engine.c $(LDFLAGS) $(LIBS) -o $@
$(BUILD)/test-ladspa: tests/test_ladspa.c include/capture_ns.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) tests/test_ladspa.c -ldl -o $@
check: all $(BUILD)/test-stream $(BUILD)/test-engine $(BUILD)/test-ladspa
	$(BUILD)/test-stream
	$(BUILD)/test-engine
	$(BUILD)/test-ladspa $(BUILD)/libaurora_capture_ns.so
	python3 -m unittest discover -s tests -p 'test_*.py' -v
install: all
	install -Dm755 $(BUILD)/libaurora_capture_ns.so $(DESTDIR)$(PREFIX)/lib/ladspa/libaurora_capture_ns.so
	install -Dm755 tools/aurora-capture-ns $(DESTDIR)$(PREFIX)/bin/aurora-capture-ns
clean:
	rm -rf $(BUILD)
