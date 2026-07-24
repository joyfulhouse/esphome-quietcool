#include "test.h"
#include "recursion_probe.h"

#include <map>

namespace {

constexpr std::size_t kMaxInstrumentedDepth = 512;
void* active_functions[kMaxInstrumentedDepth]{};
std::size_t active_depth = 0;
bool recursion_probe_enabled = false;
bool recursion_observed = false;

}  // namespace

extern "C" void __cyg_profile_func_enter(void* function, void*)
    __attribute__((no_instrument_function));
extern "C" void __cyg_profile_func_exit(void*, void*)
    __attribute__((no_instrument_function));

extern "C" void __cyg_profile_func_enter(void* function, void*) {
  if (!recursion_probe_enabled) return;
  for (std::size_t index = 0; index < active_depth; ++index)
    if (active_functions[index] == function) recursion_observed = true;
  if (active_depth == kMaxInstrumentedDepth) {
    recursion_observed = true;
    return;
  }
  active_functions[active_depth++] = function;
}

extern "C" void __cyg_profile_func_exit(void*, void*) {
  if (recursion_probe_enabled && active_depth != 0) --active_depth;
}

namespace quietcool::test {

void start_recursion_probe() __attribute__((no_instrument_function));
bool stop_recursion_probe() __attribute__((no_instrument_function));

void start_recursion_probe() {
  active_depth = 0;
  recursion_observed = false;
  recursion_probe_enabled = true;
}

bool stop_recursion_probe() {
  recursion_probe_enabled = false;
  active_depth = 0;
  return recursion_observed;
}

}  // namespace quietcool::test

int main() {
  std::size_t passed = 0;
  std::map<std::string, std::pair<std::size_t, std::size_t>> suites;
  for (const auto& test : quietcool::test::registry()) {
    auto& tally = suites[test.suite];
    ++tally.second;
    try {
      test.run();
      ++passed;
      ++tally.first;
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << test.suite << " / " << test.name << '\n'
                << "  " << error.what() << '\n';
    }
  }
  for (const auto& suite : suites) {
    std::cout << suite.first << ": " << suite.second.first << '/'
              << suite.second.second << " passed\n";
  }
  std::cout << "TOTAL: " << passed << '/' << quietcool::test::registry().size()
            << " passed\n";
  return passed == quietcool::test::registry().size() ? EXIT_SUCCESS
                                                       : EXIT_FAILURE;
}
