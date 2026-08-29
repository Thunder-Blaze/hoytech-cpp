W        = -Wall -Wextra
OPT      = -O2 -g
STD      = -std=c++17
INC      = -I..
ifeq ($(OS),Windows_NT)
    WS_LIBS = -lws2_32
    TEST_BINS = hex_test zerocopy_test file_change_monitor_test
else
    WS_LIBS =
    TEST_BINS = hex_test stream_test zerocopy_test file_change_monitor_test
endif

CXXFLAGS = $(STD) $(OPT) $(W) $(INC) -fPIC $(XCXXFLAGS)
LDFLAGS  = -lpthread $(WS_LIBS) $(XLDFLAGS)


all: $(TEST_BINS) timer_test

# ---------------------------------------------------------------------------
# Individual targets
# ---------------------------------------------------------------------------

timer_test: timer_test.cpp ../hoytech/timer.h
	$(CXX) $(CXXFLAGS) timer_test.cpp $(LDFLAGS) -o $@

zerocopy_test: zerocopy_test.cpp ../hoytech/assert_zerocopy.h
	$(CXX) $(CXXFLAGS) zerocopy_test.cpp $(LDFLAGS) -o $@

hex_test: hex_test.cpp ../hoytech/hex.h
	$(CXX) $(CXXFLAGS) hex_test.cpp $(LDFLAGS) -o $@

file_change_monitor_test: file_change_monitor_test.cpp \
    ../hoytech/file_change_monitor.h \
    ../hoytech/detail/inotify_watcher.h \
    ../hoytech/detail/kqueue_watcher.h \
    ../hoytech/detail/polling_watcher.h \
    ../hoytech/detail/watch_result.h
	$(CXX) $(CXXFLAGS) file_change_monitor_test.cpp $(LDFLAGS) -o $@

stream_test: stream_test.cpp ../hoytech/stream.h
	$(CXX) $(CXXFLAGS) stream_test.cpp $(LDFLAGS) -o $@

# ---------------------------------------------------------------------------
# Convenience targets
# ---------------------------------------------------------------------------

# Build and run every test binary; fail fast if any returns non-zero.
.PHONY: test
test: $(TEST_BINS)
	@echo "=== Running hex_test ==="
	./hex_test
	@echo "=== Running zerocopy_test ==="
	./zerocopy_test
ifneq ($(OS),Windows_NT)
	@echo "=== Running stream_test ==="
	./stream_test
endif
	@echo "=== Running file_change_monitor_test ==="
	./file_change_monitor_test
	@echo ""
	@echo "All tests passed."

.PHONY: clean
clean:
	$(RM) $(TEST_BINS) timer_test zerocopy_test
