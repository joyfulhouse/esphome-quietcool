#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace quietcool::test {

struct Failure final : std::exception {
  explicit Failure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }
 private:
  std::string message_;
};

struct Case final {
  const char* suite;
  const char* name;
  void (*run)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar final {
  Registrar(const char* suite, const char* name, void (*run)()) {
    registry().push_back({suite, name, run});
  }
};

inline void fail(const char* expression, const char* file, int line,
                 const std::string& detail = {}) {
  std::ostringstream out;
  out << file << ':' << line << ": check failed: " << expression;
  if (!detail.empty()) out << " (" << detail << ')';
  throw Failure(out.str());
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected, const char* a_text,
                 const char* b_text, const char* file, int line) {
  if (!(actual == expected)) {
    fail("equality", file, line, std::string(a_text) + " != " + b_text);
  }
}

}  // namespace quietcool::test

#define QC_JOIN_INNER(a, b) a##b
#define QC_JOIN(a, b) QC_JOIN_INNER(a, b)
#define QC_TEST(suite, name)                                                   \
  static void QC_JOIN(qc_test_, __LINE__)();                                   \
  static ::quietcool::test::Registrar QC_JOIN(qc_reg_, __LINE__)(              \
      suite, name, &QC_JOIN(qc_test_, __LINE__));                              \
  static void QC_JOIN(qc_test_, __LINE__)()
#define QC_CHECK(expr)                                                         \
  do {                                                                         \
    if (!(expr)) ::quietcool::test::fail(#expr, __FILE__, __LINE__);           \
  } while (false)
#define QC_CHECK_EQ(actual, expected)                                           \
  ::quietcool::test::check_equal((actual), (expected), #actual, #expected,      \
                                 __FILE__, __LINE__)

