CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2
TEST_CXXFLAGS := $(CXXFLAGS) -DQUIETCOOL_HOST_TEST -finstrument-functions
ROOT := ../..
INCLUDES := -I$(ROOT)/components -I.
BUILD := build

CORE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp
# The platform-free suite deliberately excludes tests/cpp/adapter: those drive
# the ESPHome adapter and must not be linked into the build that enforces the
# platform-include gate.
TEST_SOURCES := $(shell find . -name '*_test.cpp' -not -path './adapter/*' -print) \
	support/test_main.cpp
# Headers MUST be prerequisites. Without them, editing a core header — where
# the guard predicates, CoreEffects and the effect drain all live — left the
# previous binary in place and the suite "passed" against stale object code.
HEADERS := $(wildcard $(ROOT)/components/quietcool/core/*.h) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.h) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.h \
	$(shell find . -name '*.h' -print)
TARGET := $(BUILD)/quietcool_core_tests
SANITIZED_TARGET := $(BUILD)/quietcool_core_tests_sanitized
# -finstrument-functions is omitted here: the recursion probe is exercised by
# the instrumented target, and combining it with the sanitizers slows the suite
# without covering anything new.
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g

# Adapter suite. Compiles the real components/quietcool/esphome sources against
# minimal ESPHome stubs (tests/cpp/esphome_stubs) so the wiring that defers core
# re-entry — the adapter half of the loop-stack crash fix — is covered on the
# host instead of only by `esphome compile` and live uptime.
# quietcool_fan.cpp and quietcool_button.cpp are excluded: they derive from
# esphome::fan::Fan and esphome::button::Button, whose surface (traits, FanCall,
# state restore) is large enough that stubbing it would be a reimplementation
# rather than a stub — and a stub big enough to be wrong is worse than no test.
# They stay covered by `esphome compile` in CI. The publication gate they rely
# on is separately unit-tested in ports/authority_publication_gate_test.cpp.
ADAPTER_SOURCES := $(filter-out %/quietcool_fan.cpp %/quietcool_button.cpp, \
	$(wildcard $(ROOT)/components/quietcool/esphome/*.cpp))
ADAPTER_TEST_SOURCES := $(shell find ./adapter -name '*_test.cpp' -print) \
	esphome_stubs/esphome_stubs.cpp support/test_main.cpp
ADAPTER_TARGET := $(BUILD)/quietcool_adapter_tests
ADAPTER_INCLUDES := $(INCLUDES) -Iesphome_stubs
ADAPTER_HEADERS := $(wildcard $(ROOT)/components/quietcool/esphome/*.h) \
	$(shell find esphome_stubs -name '*.h' -print)
PLATFORM_FREE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.h) \
	$(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.h) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.h \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp

.PHONY: all test test-adapter test-sanitized platform-include-check \
	production-flags-check clean
all: $(TARGET)

$(TARGET): $(CORE_SOURCES) $(TEST_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) $(CORE_SOURCES) $(TEST_SOURCES) -o $@

$(SANITIZED_TARGET): $(CORE_SOURCES) $(TEST_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DQUIETCOOL_HOST_TEST $(SANITIZE_FLAGS) $(INCLUDES) \
		$(CORE_SOURCES) $(TEST_SOURCES) -o $@

$(ADAPTER_TARGET): $(CORE_SOURCES) $(ADAPTER_SOURCES) $(ADAPTER_TEST_SOURCES) \
		$(HEADERS) $(ADAPTER_HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DQUIETCOOL_HOST_TEST $(ADAPTER_INCLUDES) \
		$(CORE_SOURCES) $(ADAPTER_SOURCES) $(ADAPTER_TEST_SOURCES) -o $@

platform-include-check:
	@if grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(esphome|Arduino|arduino|esp-idf|esp_|freertos|driver/)' $(PLATFORM_FREE_SOURCES); then \
		echo "platform include found in platform-free production code"; exit 1; \
	fi

production-flags-check:
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti $(INCLUDES) -fsyntax-only $(CORE_SOURCES)

test: platform-include-check production-flags-check $(TARGET) test-adapter
	./$(TARGET)

test-adapter: $(ADAPTER_TARGET)
	./$(ADAPTER_TARGET)

# Gated in CI. Previously the sanitized binary was built by hand and the
# "ASan/UBSan clean" claim rested on a run no rule reproduced.
# LeakSanitizer is on by default under Linux ASan and unsupported on macOS, so
# it is deliberately not forced here — the target must run on both.
test-sanitized: $(SANITIZED_TARGET)
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./$(SANITIZED_TARGET)

clean:
	rm -rf $(BUILD)
