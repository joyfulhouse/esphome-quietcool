#include "test.h"

#include <map>

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

