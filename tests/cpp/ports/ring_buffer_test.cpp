// Direct coverage for the shared bounded FIFO. Both the effect queue and the
// core-callback queue now wrap this, so the index arithmetic that used to be
// duplicated is tested once, here, rather than only through its two wrappers.

#include "quietcool/core/ring_buffer.h"
#include "support/test.h"


namespace quietcool {
namespace {

QC_TEST("ring_buffer", "fills to capacity then fails closed") {
  RingBuffer<int, 4> ring;
  QC_CHECK(ring.empty());
  QC_CHECK_EQ(ring.available(), std::size_t(4));

  for (int value = 0; value < 4; ++value) QC_CHECK(ring.push(value));

  QC_CHECK(ring.full());
  QC_CHECK_EQ(ring.size(), std::size_t(4));
  QC_CHECK_EQ(ring.available(), std::size_t(0));

  // Fail closed: refuse the newest rather than overwrite an accepted entry.
  QC_CHECK(!ring.push(99));
  QC_CHECK_EQ(ring.size(), std::size_t(4));

  // The rejected value must not have displaced anything.
  for (int value = 0; value < 4; ++value) QC_CHECK_EQ(ring.pop().value(), value);
  QC_CHECK(ring.empty());
  QC_CHECK(!ring.pop().has_value());
}

QC_TEST("ring_buffer", "preserves order across wraparound") {
  RingBuffer<int, 4> ring;
  for (int value = 0; value < 3; ++value) QC_CHECK(ring.push(value));
  QC_CHECK_EQ(ring.pop().value(), 0);
  QC_CHECK_EQ(ring.pop().value(), 1);

  // head_ and tail_ are now mid-array; push past the end to force the modulo
  // path that was previously implemented twice.
  for (int value = 10; value < 13; ++value) QC_CHECK(ring.push(value));
  QC_CHECK_EQ(ring.size(), std::size_t(4));

  QC_CHECK_EQ(ring.pop().value(), 2);
  QC_CHECK_EQ(ring.pop().value(), 10);
  QC_CHECK_EQ(ring.pop().value(), 11);
  QC_CHECK_EQ(ring.pop().value(), 12);
  QC_CHECK(ring.empty());
}

QC_TEST("ring_buffer", "reuses slots indefinitely without drift") {
  RingBuffer<int, 3> ring;
  // Many more cycles than capacity: an off-by-one in the modulo would desync
  // head_ from tail_ and surface as a wrong value or a stuck size.
  for (int cycle = 0; cycle < 50; ++cycle) {
    QC_CHECK(ring.push(cycle));
    QC_CHECK_EQ(ring.pop().value(), cycle);
    QC_CHECK(ring.empty());
  }
}

QC_TEST("ring_buffer", "capacity of one behaves as a single slot") {
  RingBuffer<int, 1> ring;
  QC_CHECK(ring.push(7));
  QC_CHECK(ring.full());
  QC_CHECK(!ring.push(8));
  QC_CHECK_EQ(ring.pop().value(), 7);
  QC_CHECK(ring.empty());
  QC_CHECK(ring.push(8));
  QC_CHECK_EQ(ring.pop().value(), 8);
}

}  // namespace
}  // namespace quietcool
