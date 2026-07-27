# ESPHome stubs (host tests only)

These headers exist so `components/quietcool/esphome/*.cpp` — the adapter layer,
which holds the adapter half of the loop-stack crash fix — can be compiled and
driven on the host. They are **not** a reimplementation of ESPHome and are never
part of a firmware build.

## Why this is safe

The logic under test is real: `ConfirmationCore`, `BurstTransmitter`,
`CoreEffectDrain` and `CoreCallbackQueue` are the production classes, already
host-compiled by the rest of the suite. Only ESPHome's entity, logging,
preferences and timing surface is stubbed, and none of that surface carries
coordinator logic.

A stub that drifts from real ESPHome cannot hide a defect, because the real
adapter is still compiled against real ESPHome by the `esphome compile` jobs in
CI. If a stub diverges enough to matter, that job fails.

## The places semantics matter

`Component::mark_failed()` is modelled deliberately, not incidentally. In real
ESPHome (`esphome/core/component.cpp`) it sets `COMPONENT_STATE_FAILED`, calls
`status_set_error()` and removes the component from the loop; `Component::call()`
then does nothing for a failed component, and nothing invokes
`reset_to_construction_state()` at runtime. The failure is therefore permanent
until reboot. `is_failed()` here exposes exactly that, and the adapter tests
assert against it.

`ESPPreferences` models the **RAM stage** the same way. In real ESPHome on
ESP32, `ESPPreferenceObject::save()` does not touch flash — it stages the bytes
in memory, and only `sync()` performs the `nvs_commit()`. The preferences
component's interval syncer calls `sync()` on a default 60 s
`flash_write_interval`, so a value that is merely staged is lost to an
ungraceful power cut. The stub therefore keeps a staged copy and a committed
copy, and `simulate_power_loss()` drops the difference. Without that, every
save round-trips through RAM and no test could distinguish a durable write from
a staged one — which is precisely the distinction the persisted speed
capability (issue #31) depends on.

Keep these files mechanical. If one starts needing logic, that is a signal the
adapter is no longer thin.
