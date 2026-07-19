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
`Fan Speed Capability`. The fan entity still statically exposes all three speed
choices (ESPHome entities are fixed at compile time); on a two-speed
installation, `Fan Speed Capability` will report `2-speed` after the first
confirmed command — use only the speeds your OEM control supports rather than
interpreting the entity's Medium option as detected hardware capability. When
deploying to multiple fans, confirmation state and capability remain local to
each device's learned sender ID.

## Home Assistant display sources

The OLED's indoor/outdoor/attic values come from three HA template-sensor helpers
(`sensor.quietcool_display_{indoor,outdoor,attic}`), editable in
Settings → Devices & Services → Helpers. Re-point any source with a template
edit — no reflash. The device imports them over the native API and never drives
the fan from them.
