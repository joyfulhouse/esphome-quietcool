#include "quietcool/ports/millis32_widener.h"
#include "support/test.h"

#include <cstdint>

namespace quietcool {
namespace {

QC_TEST("clock_widener", "first sample preserves the raw millisecond value") {
  Millis32Widener widener;
  QC_CHECK_EQ(widener.widen(1234U), 1234U);
}

QC_TEST("clock_widener", "equal and increasing samples never move backward") {
  Millis32Widener widener;
  QC_CHECK_EQ(widener.widen(100U), 100U);
  QC_CHECK_EQ(widener.widen(100U), 100U);
  QC_CHECK_EQ(widener.widen(101U), 101U);
}

QC_TEST("clock_widener", "uint32 wrap advances the widened epoch") {
  Millis32Widener widener;
  QC_CHECK_EQ(widener.widen(0xFFFFFFFEU), 0xFFFFFFFEULL);
  QC_CHECK_EQ(widener.widen(0xFFFFFFFFU), 0xFFFFFFFFULL);
  QC_CHECK_EQ(widener.widen(0U), MonotonicMs{1} << 32U);
  QC_CHECK_EQ(widener.widen(7U), (MonotonicMs{1} << 32U) + 7U);
}

QC_TEST("clock_widener", "multiple wraps accumulate independent epochs") {
  Millis32Widener widener;
  widener.widen(0xFFFFFFFFU);
  widener.widen(0U);
  widener.widen(0xFFFFFFFFU);
  QC_CHECK_EQ(widener.widen(0U), MonotonicMs{2} << 32U);
}

}  // namespace
}  // namespace quietcool
