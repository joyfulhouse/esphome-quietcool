#pragma once

// Host stub. Logging goes nowhere, but the formatted lines are CAPTURED so
// tests can pin the few log lines that are themselves documented user-visible
// surface — README/INSTALL promise `reason=already_provisioned` on a refused
// Learn (issue #16), and a mutant dropping the reason en route to the sink
// survived every state-based assertion until that line was pinned. Adapter
// tests should still prefer observable state (failed flag, core state,
// published entity values); assert on captured_logs() only when the log text
// IS the promised surface. Formatting through snprintf keeps format/argument
// mismatches visible here as they are in a firmware build.

#include <cstdio>
#include <string>
#include <vector>

namespace esphome::host_test {

inline std::vector<std::string>& captured_logs() {
  static std::vector<std::string> logs;
  return logs;
}

// No-argument form: the message is stored verbatim. (snprintf with a
// non-literal format and no arguments trips -Wformat-security under -Werror.)
inline void capture_log(const char* /*tag*/, const char* message) {
  captured_logs().emplace_back(message);
}

template <typename First, typename... Rest>
inline void capture_log(const char* /*tag*/, const char* format, First first,
                        Rest... rest) {
  char line[512];
  std::snprintf(line, sizeof(line), format, first, rest...);
  captured_logs().push_back(line);
}

}  // namespace esphome::host_test

#define ESP_LOGE(...) ::esphome::host_test::capture_log(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::host_test::capture_log(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::host_test::capture_log(__VA_ARGS__)
#define ESP_LOGD(...) ::esphome::host_test::capture_log(__VA_ARGS__)
#define ESP_LOGV(...) ::esphome::host_test::capture_log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) ::esphome::host_test::capture_log(__VA_ARGS__)
