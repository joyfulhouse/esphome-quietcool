CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2
TEST_CXXFLAGS := $(CXXFLAGS) -DQUIETCOOL_HOST_TEST -finstrument-functions
ROOT := ../..
INCLUDES := -I$(ROOT)/components -I.
BUILD := build

CORE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp
TEST_SOURCES := $(shell find . -name '*_test.cpp' -print) support/test_main.cpp
TARGET := $(BUILD)/quietcool_core_tests
SANITIZED_TARGET := $(BUILD)/quietcool_core_tests_sanitized
# -finstrument-functions is omitted here: the recursion probe is exercised by
# the instrumented target, and combining it with the sanitizers slows the suite
# without covering anything new.
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
PLATFORM_FREE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.h) \
	$(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.h) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.h \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp

.PHONY: all test test-sanitized platform-include-check production-flags-check clean
all: $(TARGET)

$(TARGET): $(CORE_SOURCES) $(TEST_SOURCES)
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) $(CORE_SOURCES) $(TEST_SOURCES) -o $@

$(SANITIZED_TARGET): $(CORE_SOURCES) $(TEST_SOURCES)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DQUIETCOOL_HOST_TEST $(SANITIZE_FLAGS) $(INCLUDES) \
		$(CORE_SOURCES) $(TEST_SOURCES) -o $@

platform-include-check:
	@if grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(esphome|Arduino|arduino|esp-idf|esp_|freertos|driver/)' $(PLATFORM_FREE_SOURCES); then \
		echo "platform include found in platform-free production code"; exit 1; \
	fi

production-flags-check:
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti $(INCLUDES) -fsyntax-only $(CORE_SOURCES)

test: platform-include-check production-flags-check $(TARGET)
	./$(TARGET)

# Gated in CI. Previously the sanitized binary was built by hand and the
# "ASan/UBSan clean" claim rested on a run no rule reproduced.
# LeakSanitizer is on by default under Linux ASan and unsupported on macOS, so
# it is deliberately not forced here — the target must run on both.
test-sanitized: $(SANITIZED_TARGET)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(SANITIZED_TARGET)

clean:
	rm -rf $(BUILD)
