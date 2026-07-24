CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2
TEST_CXXFLAGS := $(CXXFLAGS) -DQUIETCOOL_HOST_TEST
ROOT := ../..
INCLUDES := -I$(ROOT)/components -I.
BUILD := build

CORE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp
TEST_SOURCES := $(shell find . -name '*_test.cpp' -print) support/test_main.cpp
TARGET := $(BUILD)/quietcool_core_tests
PLATFORM_FREE_SOURCES := $(wildcard $(ROOT)/components/quietcool/core/*.h) \
	$(wildcard $(ROOT)/components/quietcool/core/*.cpp) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.h) \
	$(wildcard $(ROOT)/components/quietcool/ports/*.cpp) \
	$(ROOT)/components/quietcool/radio/burst_transmitter.h \
	$(ROOT)/components/quietcool/radio/burst_transmitter.cpp

.PHONY: all test platform-include-check production-flags-check clean
all: $(TARGET)

$(TARGET): $(CORE_SOURCES) $(TEST_SOURCES)
	@mkdir -p $(BUILD)
	$(CXX) $(TEST_CXXFLAGS) $(INCLUDES) $(CORE_SOURCES) $(TEST_SOURCES) -o $@

platform-include-check:
	@if grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*(esphome|Arduino|arduino|esp-idf|esp_|freertos|driver/)' $(PLATFORM_FREE_SOURCES); then \
		echo "platform include found in platform-free production code"; exit 1; \
	fi

production-flags-check:
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti $(INCLUDES) -fsyntax-only $(CORE_SOURCES)

test: platform-include-check production-flags-check $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)
