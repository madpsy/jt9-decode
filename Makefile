# Makefile for jt9_decode

CXX = g++
TARGET = jt9_decode
SOURCE = jt9_decode.cpp
HEADERS = qt_shm_compat.h wsjtx/commons.h
INCLUDES = -I./wsjtx
CXXFLAGS = -std=c++11 -O2 -Wall -Wextra -fPIC
LDFLAGS = -pthread -lrt

.PHONY: all clean test test-full test-interop test-e2e test-stress install uninstall

all: $(TARGET)

$(TARGET): $(SOURCE) $(HEADERS)
	$(CXX) -o $(TARGET) $(SOURCE) $(INCLUDES) $(CXXFLAGS) $(LDFLAGS)

# ---- Tests -----------------------------------------------------------------
# The interop test links a small helper against Qt5Core to check that our
# shared memory is byte-for-byte compatible with QSharedMemory (what jt9 uses).
# Qt is only needed to run the tests, never to build or run jt9_decode.

TEST_DIR = tests
TEST_BUILD = $(TEST_DIR)/build

$(TEST_BUILD):
	mkdir -p $(TEST_BUILD)

$(TEST_BUILD)/qt_ref_helper: $(TEST_DIR)/qt_ref_helper.cpp | $(TEST_BUILD)
	$(CXX) -std=c++11 -fPIC -o $@ $< $(shell pkg-config --cflags --libs Qt5Core)

$(TEST_BUILD)/test_shm_interop: $(TEST_DIR)/test_shm_interop.cpp qt_shm_compat.h | $(TEST_BUILD)
	$(CXX) -std=c++11 -Wall -Wextra -I. -o $@ $< -pthread

$(TEST_BUILD)/jt9_decode_asan: $(SOURCE) $(HEADERS) | $(TEST_BUILD)
	$(CXX) -o $@ $(SOURCE) $(INCLUDES) -std=c++11 -g -O1 -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread -lrt

$(TEST_BUILD)/jt9_decode_tsan: $(SOURCE) $(HEADERS) | $(TEST_BUILD)
	$(CXX) -o $@ $(SOURCE) $(INCLUDES) -std=c++11 -g -O1 -fsanitize=thread -pthread -lrt

test-interop: $(TEST_BUILD)/qt_ref_helper $(TEST_BUILD)/test_shm_interop
	$(TEST_BUILD)/test_shm_interop $(TEST_BUILD)/qt_ref_helper

# FULL=1 runs every e2e/stress case at full size (~15 min); the default is a
# representative subset of every stage (~5 min). `make test-full` sets it.
test-e2e: $(TARGET)
	$(TEST_DIR)/run_e2e.sh ./$(TARGET)

test-stress: $(TARGET) $(TEST_BUILD)/jt9_decode_asan $(TEST_BUILD)/jt9_decode_tsan
	$(TEST_DIR)/run_stress.sh ./$(TARGET) $(TEST_BUILD)/jt9_decode_asan $(TEST_BUILD)/jt9_decode_tsan

test: test-interop test-e2e test-stress

test-full:
	$(MAKE) FULL=1 test

clean:
	rm -f $(TARGET)
	rm -rf $(TEST_BUILD)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET)
