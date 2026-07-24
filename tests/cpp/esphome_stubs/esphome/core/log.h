#pragma once

// Host stub. Logging is deliberately inert: the adapter tests assert on
// observable state (failed flag, core state, published entity values), never on
// log text. The arguments are still consumed so format/argument mismatches stay
// compile errors here as they are in a firmware build.

namespace esphome::host_test {

template <typename... Args>
inline void consume_log_args(const Args&...) {}

}  // namespace esphome::host_test

#define ESP_LOGE(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
#define ESP_LOGW(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
#define ESP_LOGI(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
#define ESP_LOGD(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
#define ESP_LOGV(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
#define ESP_LOGCONFIG(...) ::esphome::host_test::consume_log_args(__VA_ARGS__)
