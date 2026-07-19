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

## Home Assistant display sources

The OLED's indoor/outdoor/attic values come from three HA template-sensor helpers
(`sensor.quietcool_display_{indoor,outdoor,attic}`), editable in
Settings → Devices & Services → Helpers. Re-point any source with a template
edit — no reflash. The device imports them over the native API and never drives
the fan from them.
