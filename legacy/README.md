# Legacy YAML track (frozen)

The two configs in this directory are the original all-YAML firmware: the RF
confirmation state machine implemented in YAML lambdas on the Arduino
framework, using the still-supported
[`components/quietcool_legacy_yaml`](../components/quietcool_legacy_yaml)
fan-entity platform.

**The YAML state machine was frozen on 2026-07-21 in favor of the C++ core**
(`components/quietcool`, configured by the `quietcool-cpp-*.yaml` files at the
repository root). These files remain for reference and for units not yet cut
over — do not develop new behavior here.

| File | Board | Radio | Status |
| --- | --- | --- | --- |
| `quietcool-lora32.yaml` | LilyGO TTGO LoRa32 V2.1 | SX1278 | Frozen; superseded by `quietcool-cpp-lora32.yaml` |
| `quietcool-lora-v3.yaml` | Heltec / HiLetgo ESP32 LoRa V3 | SX1262 | Frozen; still the only deployable config for this board |

Both configs stay buildable: CI validates and compiles them, and the Python
regression suite (`tests/test_quietcool_esphome_config.py`) continues to assert
their safety invariants. To build one, provide a `secrets.yaml` in this
directory (`cp ../secrets.yaml.example secrets.yaml`, then edit) — ESPHome
resolves `!secret` and the `../components` / `../fonts` / `../images` paths
relative to the config file.

For everything else — features, learn mode, safety guidance — see the
[root README](../README.md).
