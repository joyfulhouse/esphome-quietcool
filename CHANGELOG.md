# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/joyfulhouse/esphome-quietcool/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/joyfulhouse/esphome-quietcool/releases/tag/v0.1.0
