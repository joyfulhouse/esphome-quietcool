#pragma once

// Host stub. mark_failed() is modelled faithfully because the adapter tests
// assert on it; see esphome_stubs/README.md.

namespace esphome {

namespace setup_priority {
constexpr float BUS = 1000.0f;
constexpr float IO = 900.0f;
constexpr float HARDWARE = 800.0f;
constexpr float DATA = 600.0f;
constexpr float PROCESSOR = 400.0f;
constexpr float LATE = -100.0f;
}  // namespace setup_priority

enum class ComponentState { Constructed, Setup, Loop, Failed };

class Component {
 public:
  virtual ~Component() = default;

  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return setup_priority::DATA; }

  // Real ESPHome sets COMPONENT_STATE_FAILED, calls status_set_error() and
  // removes the component from the loop. Component::call() then does nothing
  // for a failed component and nothing invokes reset_to_construction_state()
  // at runtime, so the failure is permanent until reboot. Tests rely on that
  // being permanent, so this stub has no recovery path either.
  void mark_failed() {
    state_ = ComponentState::Failed;
    status_ = true;
  }

  bool is_failed() const { return state_ == ComponentState::Failed; }
  bool status_has_error() const { return status_; }
  void status_set_error() { status_ = true; }
  void status_clear_error() { status_ = false; }

  // Mirrors the dispatch rule that makes mark_failed() terminal: a failed
  // component's loop() is never called again.
  void call_loop() {
    if (is_failed()) return;
    loop();
  }

 private:
  ComponentState state_{ComponentState::Constructed};
  bool status_{false};
};

}  // namespace esphome
