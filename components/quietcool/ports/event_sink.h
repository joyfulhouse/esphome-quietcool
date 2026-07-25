#pragma once

#include "quietcool/core/core_types.h"

namespace quietcool {

class EventSink {
 public:
  virtual ~EventSink();
  virtual void on_core_event(const CoreEvent& event) = 0;
};

}  // namespace quietcool
