# Host-side unit tests for FreeSWITCH-independent logic.
#
# These do NOT build any FreeSWITCH module. They compile the pure, header-only
# units (base64.hpp, SimpleBuffer) on the host with coverage instrumentation so
# the logic that does not need the FreeSWITCH runtime can actually be tested.
#
#   make            # build + run tests
#   make coverage   # build + run + gcov; prints HOST_COVERAGE=<pct>%
#   make sanitize   # build + run under AddressSanitizer + UndefinedBehaviorSanitizer
#   make clean
#
# NOTE: `sanitize` exercises only the FreeSWITCH-independent units. True ASan/TSan
# of the modules' threading needs a sanitizer build of the module + a live call
# (see SANITIZERS.md) — it cannot be done from this host harness.
#
# The repo's quality gate parses the HOST_COVERAGE line. This coverage figure is
# for the host-testable units ONLY — it is NOT repo-wide or module coverage.

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -g -O0 -Wall -Wextra
COVFLAGS  = --coverage
BUILD     = build

# simple_buffer.h is shared by the four callBroadcast-utilized transcribe modules.
# base64.hpp is not used by those four; it lives in mod_audio_fork (tested here as
# general repo coverage, with a note that it is outside the utilized set).
DEEPGRAM  = ../modules/mod_deepgram_transcribe
AUDIOFORK = ../modules/mod_audio_fork

TESTS     = test_base64 test_simple_buffer
BINS      = $(addprefix $(BUILD)/,$(TESTS))
COV_SRCS  = base64.hpp simple_buffer.h

.PHONY: test coverage sanitize clean

test: $(BINS)
	@set -e; for b in $(BINS); do echo "== $$b =="; ./$$b; done

# AddressSanitizer + UndefinedBehaviorSanitizer (+ leak detection) on the
# host-testable units. Built separately (no coverage flags) so the sanitizer
# runtime isn't perturbed by gcov instrumentation.
SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: | $(BUILD)
	@set -e; \
	$(CXX) -std=c++11 -g -O1 $(SANFLAGS) -I$(AUDIOFORK) -o $(BUILD)/test_base64_san test_base64.cpp; \
	$(CXX) -std=c++11 -g -O1 $(SANFLAGS) -I$(DEEPGRAM) -o $(BUILD)/test_simple_buffer_san test_simple_buffer.cpp; \
	echo "== ASan/UBSan: test_base64 =="; \
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $(BUILD)/test_base64_san; \
	echo "== ASan/UBSan: test_simple_buffer =="; \
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 $(BUILD)/test_simple_buffer_san; \
	echo "ASan/UBSan: clean (no sanitizer errors)"

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_base64.o: test_base64.cpp test_util.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(COVFLAGS) -I$(AUDIOFORK) -c -o $@ test_base64.cpp

$(BUILD)/test_simple_buffer.o: test_simple_buffer.cpp test_util.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(COVFLAGS) -I$(DEEPGRAM) -c -o $@ test_simple_buffer.cpp

$(BUILD)/test_base64: $(BUILD)/test_base64.o
	$(CXX) $(COVFLAGS) -o $@ $^

$(BUILD)/test_simple_buffer: $(BUILD)/test_simple_buffer.o
	$(CXX) $(COVFLAGS) -o $@ $^

coverage: test
	@gcov -r -o $(BUILD) test_base64.cpp test_simple_buffer.cpp >/dev/null 2>&1 || true
	@echo "--- host coverage (FreeSWITCH-independent units) ---"
	@total=0; covered=0; \
	for h in $(COV_SRCS); do \
	  g=$$(ls $$h.gcov 2>/dev/null | head -n1); \
	  if [ -n "$$g" ]; then \
	    c=$$(grep -cE '^[[:space:]]*[0-9]+:' "$$g"); \
	    n=$$(grep -cE '^[[:space:]]*#####:' "$$g"); \
	    tot=$$((c + n)); \
	    if [ $$tot -gt 0 ]; then pct=$$(awk -v c=$$c -v t=$$tot 'BEGIN{printf "%.1f", 100*c/t}'); \
	      else pct="n/a"; fi; \
	    printf '  %-18s %s%% (%s/%s lines)\n' "$$h" "$$pct" "$$c" "$$tot"; \
	    covered=$$((covered + c)); total=$$((total + tot)); \
	  else printf '  %-18s no gcov data\n' "$$h"; fi; \
	done; \
	if [ $$total -gt 0 ]; then \
	  awk -v c=$$covered -v t=$$total 'BEGIN{printf "HOST_COVERAGE=%.1f%%\n", 100*c/t}'; \
	else echo "HOST_COVERAGE=n/a"; fi

clean:
	@rm -rf $(BUILD) *.gcov
