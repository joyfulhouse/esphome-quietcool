# Deployment: multiple fans

The firmware doesn't hard-code anything device-specific. A second (or third) fan
reuses the whole base config through a thin wrapper that overrides only
substitutions — ESPHome gives the including file precedence over the package.

`quietcool-lora32-upstairs.yaml`:

```yaml
substitutions:
  device_name: quietcool-lora32-upstairs
  device_friendly_name: "QuietCool Upstairs RF Controller"
  fallback_ap_ssid: "QuietCool-Upstairs"
  quietcool_sender_id: "0x00000000"        # learn this unit's own remote
  display_indoor_entity: sensor.quietcool_display_upstairs_indoor
  display_outdoor_entity: sensor.quietcool_display_upstairs_outdoor
  display_attic_entity: sensor.quietcool_display_upstairs_attic
packages:
  base: !include quietcool-lora32.yaml
```

Each device needs its own API/OTA/fallback-AP secrets (the `<device>_` naming
convention in `secrets.yaml`); Wi-Fi is shared.

## Reference install (maintainers')

Two whole-house fans, both on a TTGO LoRa32 V2.1, adopted into Home Assistant:

| | Downstairs | Upstairs |
| --- | --- | --- |
| ESPHome name | `quietcool-lora32` | `quietcool-lora32-upstairs` |
| Learned sender ID | `CB 00 47 39` | `CB 03 D7 D3` |
| Config | `quietcool-lora32.yaml` | `quietcool-lora32-upstairs.yaml` |

The two remotes were confirmed (by firmware dump) to run byte-identical code
differing only in the 3-byte per-unit ID — see
[firmware-analysis.md](firmware-analysis.md). Sender IDs are transmitted in the
clear over RF and are not secret.

## Home Assistant display sources

The OLED's indoor/outdoor/attic values come from three HA template-sensor helpers
(`sensor.quietcool_display_{indoor,outdoor,attic}`), editable in
Settings → Devices & Services → Helpers. Re-point any source with a template
edit — no reflash. The device imports them over the native API and never drives
the fan from them.
