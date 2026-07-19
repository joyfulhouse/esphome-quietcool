# Deployment: multiple fans

The firmware doesn't hard-code anything device-specific. A second (or third) fan
reuses the whole base config through a thin wrapper that overrides only
substitutions — ESPHome gives the including file precedence over the package.

A second-device wrapper looks like this:

```yaml
substitutions:
  device_name: quietcool-fan-2
  device_friendly_name: "QuietCool RF Controller 2"
  fallback_ap_ssid: "QuietCool-2"
  quietcool_sender_id: "0x00000000"        # learn this unit's own remote
  display_indoor_entity: sensor.quietcool_fan_2_indoor
  display_outdoor_entity: sensor.quietcool_fan_2_outdoor
  display_attic_entity: sensor.quietcool_fan_2_attic
packages:
  base: !include quietcool-lora32.yaml
```

Each device needs its own API/OTA/fallback-AP secrets (the `<device>_` naming
convention in `secrets.yaml`); Wi-Fi is shared. Every fan learns its own remote,
so the four-byte sender IDs differ per unit (see
[firmware-analysis.md](firmware-analysis.md)) — no config change needed beyond
letting each one run learn mode.

Keep the checked-in `quietcool_sender_id: "0x00000000"` seed in every reusable
wrapper. Each device must learn its own ID; copying an ID from another fan would
also make any later query/reply correlation target the wrong receiver.

## State confirmation and speed capability

The corrected protocol research shows that a fan reply is a fixed six-byte
payload carrying the receiver's real state and, when supplied, speed capability.
A live-validated SX1278 controller uses that response after an explicit command
to perform bounded query/compare/continuation handling and correctly recognized
a two-speed receiver, whose OEM remote skips Medium.

Both public base files now incorporate that closed-loop controller and publish
`Last Confirmed Fan State`, `Command Confirmation Status`, and
`Fan Speed Capability`, plus the `Fan State Known`, `Fan Confirmed Off`, and
`Timer State Known` binary diagnostics. The fan entity still statically exposes all three speed
choices (ESPHome entities are fixed at compile time); on a two-speed
installation, `Fan Speed Capability` will report `2-speed` after the first
confirmed command — use only the speeds your OEM control supports rather than
interpreting the entity's Medium option as detected hardware capability. When
deploying to multiple fans, confirmation state and capability remain local to
each device's learned sender ID.

The custom fan control path is confirmation-driven: an HA command does not
publish its requested state before the receiver answers. The native ESPHome Fan
API, however, has no missing/unknown value and exposes its raw Off/Low defaults
when HA first subscribes. `Fan State Known` is therefore required to distinguish
that bootstrap value from physical evidence. Equivalent duration-zero Off
requests (`80`/`90`/`A0`/`B0`) join an already active Off transaction without
another TX or retry reset. The remembered-speed byte remains an OEM-faithful
compatibility choice, not a proven acceptance requirement; `90` has succeeded
from an apparent High state.

Every new command and every actual non-query command burst, including an
automatic spaced re-fire, sets both known diagnostics false and clears `Fan
Confirmed Off`. Correlated response consensus from this controller's own query
may publish physical state. A passively heard OEM command is retained in RF
diagnostics and may cancel conflicting local work, but it never mutates the
safety fan entity because hearing a command is not proof that the receiver
acted on it.

Timer state follows the same rule. The select does not boot with a guessed
`None`; outgoing timer and ordinary state commands do not optimistically arm or
clear confirmed timer metadata. A failed/no-response transaction leaves
`Timer State Known` false, suppresses select/countdown synchronization, and
prevents expiry from publishing guessed Off. Because a fan report contains the
programmed duration rather than the timer's age, only confirmation of this
controller's own timer command can anchor a trusted countdown. A manual Refresh
that finds an active timer leaves fan/timer authority unknown. Estimated expiry
clears authority and never publishes guessed Off.

## Home Assistant interlock requirement

On 2026-07-19, an optimistic TemplateFan Off update caused a window interlock
to re-enter whenever RF confirmation restored High. Production recorded 107
fan transitions in 73.34 seconds, 54 interlock runs, 53 mismatches that all
still showed five attempts remaining, and 118 three-frame bursts (354
application frames). No ESP-originated On command was present.

Confirmation-driven local control removes the false requested Off/On edge.
Semantic active-Off coalescing independently prevents repeated HA calls from
extending the fixed transaction budget, while preserving the normal spaced
re-fire schedule. Safety automations should consume the atomic `Fan Confirmed
Off` diagnostic. The native fan entity alone is insufficient at boot and after
an unresolved command, and separately joining Fan state with `Fan State Known`
can observe API updates from different batches:

```jinja2
{{ is_state('binary_sensor.YOUR_DEVICE_fan_confirmed_off', 'on') }}
```

Timer automations must likewise require `Timer State Known`. Passive OEM
traffic cannot satisfy either safety condition. `Fan Confirmed Off` is an
atomic record of the last authoritative consensus, not an eternal motor sensor;
if every later OEM frame is missed, it can become stale. Use explicit Refresh
or independent airflow/motor sensing when freshness is safety-critical.

The TX serializer is a bounded `mode: queued` script with `max_runs: 5`.
ESPHome may reject an execution beyond capacity; deployment documentation must
not promise one on-air burst per rapid press. The latest transaction and re-fire
command are armed before enqueue, and the spaced driver waits for the queue to
become idle, so a rejected initial enqueue still has a bounded path to retry the
latest desired command. Obsolete queued commands are rejected at actual
execution. Successive query windows place the next acceptance floor one
millisecond after the preceding inclusive 2.5 s response tail without blocking
a due one-second command re-fire, and a heard OEM query blocks all local airtime
for its two-second physical exchange.

Each deployed controller owns its transaction state, so coalescing never joins
requests across fans. Boot, OTA, and API reconnect perform no query and no TX;
use each device's `Refresh Fan State` button when an explicit resync is needed.

On 2026-07-19, final config validation and compilation succeeded under ESPHome
2026.7.0 for both public targets and both downstream SX1278 wrappers. The public
compile hashes were `0x80f65068` (SX1278) and `0x0be208d7` (SX1262). The live
downstairs wrapper built as `0xef85b7d8` at 14:25:44 PDT and was OTA-flashed
exactly once to `10.100.8.46` at about 14:26 PDT; the flashed binary SHA-256 was
`714c455a1673c3c3255132df84f68d020afbbcfd989594c34c997a940cafc59d`.

Post-OTA, 62 idle seconds left `TX Count` at zero, all three authority flags
false, the last state unknown, and confirmation status idle. Manual Refresh
used one query and reached exact `90 90` consensus from replies at about +396
and +520 ms. Three rapid HA Off calls then formed one transaction: the two
equivalent repeats joined instead of resetting the budget, and only one Off
burst plus one query was sent. It confirmed `90` after one command and one
query, taking total TX Count to three for the complete Refresh-plus-Off test.
Late exact reports at about +1,422 and +1,520 ms remained passive. HA history
showed no fan or interlock transitions; the final HA fan state was Off and the
interlock remained enabled. These are RF/entity observations, not independent
motor or airflow proof.

This rollout used the downstream wrapper, not a public named artifact. The
upstairs controller was offline and was not flashed. The public V3 target was
compiled but has not been tested on SX1262 hardware.

## Home Assistant display sources

The OLED's indoor/outdoor/attic values come from three HA template-sensor helpers
(`sensor.quietcool_display_{indoor,outdoor,attic}`), editable in
Settings → Devices & Services → Helpers. Re-point any source with a template
edit — no reflash. The device imports them over the native API and never drives
the fan from them.
