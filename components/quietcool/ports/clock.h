#pragma once

#include "esphome/components/quietcool/core/core_types.h"

namespace quietcool {

class Clock {
 public:
  virtual ~Clock();
  virtual MonotonicMs now_ms() const = 0;
};

}  // namespace quietcool
