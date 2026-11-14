# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- A 3-speed fan could drop to two listed speeds (LOW/HIGH, Medium missing in
  Home Assistant and on the OLED) until the controller was restarted, while
  `Fan Speed Capability` read 3 (#46). The paired wall remote's command
  frames carry marker bits `10`, which is also the encoding of a 2-speed
  capability, and they are sent under the sender ID the controller is bound
  to. Only the controller's *own* echo was down-ranked (#31), so remote
  bursts overheard in passive observation, or in the recovery-query window
  the remote's activity arms, were taken as full-weight proof of a 2-speed
  fan. Every command-shaped frame is now weak capability evidence in every
  receive context: it can teach a fan whose band is still unknown, but never
  lower one already known. Swapping in a 2-speed fan under the same remote ID
  therefore needs Forget + Learn.

### Changed

- The listed speed band is no longer latched until reboot. It still never
  widens while a Home Assistant API client is connected (HA caches the speed
  count per connection), but it is re-derived from the confirmed capability
  whenever no API client is connected, so any narrowing is corrected at the
  next reconnect instead of needing a manual restart (#46). Builds with MQTT
  keep the until-reboot latch. Band changes are logged at INFO under
  `quietcool.fan`.

## [0.3.1] - 2026-11-01

### Fixed

- ESPHome Device Builder / `github://` installs failed to compile with
  `quietcool/esphome/quietcool_component.h: No such file or directory`
  (#42). ESPHome copies only the top-level C++ files of an external
  component, and all of this component's C++ lives in subdirectories; the
  tree reached the build only through an `esphome: includes:` bridge, which
  accepts local paths only. The component now copies its
  `core/`/`ports/`/`radio/`/`esphome/` tree into the build itself (pruning
  stale files), so it is self-contained however it is sourced. The bridge is
  removed from the repo's configs, so CI's `esphome compile` now exercises the
  same path as a Builder install. Local wrapper configs that still carry the
  bridge keep working (it copies identical files), but can drop it.

## [0.3.0] - 2026-08-17

### Added

- Passive remote-state synchronization: OEM remote key-presses overheard on
  air now update the confirmed fan state, with verification queries and
  passive-evidence consensus guarding against misclassification (#37).
- Periodic heartbeat queries with deterministic jitter: the controller
  re-confirms the fan's true state every few minutes, so any missed RF
  evidence self-corrects without operator action (#37).
- SX127x RX watchdog in `quietcool-cpp-lora32.yaml`: the FSK packet engine
  can wedge in variable-length mode when a truncated burst tail or corrupted
  length byte starts a phantom packet, leaving the radio deaf until the next
  transmit. A guarded 5-second interval now re-arms RX (standby → rx)
  whenever the radio has neither delivered nor transmitted recently,
  bounding any wedge to seconds. Diagnosed on a production unit that
  reliably heard the first burst after each transmit and then nothing.

### Fixed

- Passive-epoch response handling: incomplete or abandoned passive responses
  now recover instead of stalling the confirmation state machine; stale
  passive ambiguity and expired responses are cleaned up at epoch
  boundaries.
- Passive evidence and response consensus are isolated from each other, so
  abandoned observations publish their authority loss instead of leaking
  into later epochs.
- Terminal OEM recovery re-arms after passive activity.
- Heartbeat scheduling survives degradation and passive-sync interleavings.

## [0.2.0] - 2026-08-02

### Added

- Fan timer select entity: every option transmits; a duration starts the fan
  (at LOW if stopped), `None` runs continuously and restarts a fan whose
  timer expired.
- Eight diagnostic entities (TX/RX counters, last frames, confirmed state,
  speed capability, remote sender id, and a controller fault latch).
- Programmed-duration deadline: the timer's expiry is tracked and persisted
  so a scheduled stop survives reboots.

### Fixed

- Fault sensor publishes its healthy state at boot, so an unfaulted
  controller reads "no fault" instead of Unknown (#35).
- `setup()` bails out on mid-drain degradation instead of re-entering a core
  already declared broken (#35).
- Fifteen adversarial-review hardening rounds covering delivery ordering,
  press supersession, expiry-bound rebuilds, publish cadence, and
  persistence gates.

## [0.1.0] - 2026-07-27

### Added

- Initial release of the C++ confirmation-core ESPHome external component:
  RF frame codec, command transactions with confirmation, learn/forget
  provisioning with NVS persistence, burst transmitter, and the TTGO LoRa32
  reference configuration with OLED status display.
- Replaces the legacy YAML-only build (retained under `legacy/`).

[Unreleased]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/joyfulhouse/esphome-quietcool/releases/tag/v0.1.0
