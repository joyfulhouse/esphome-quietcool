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

Both configs stay buildable: `.github/workflows/ci.yml` runs `esphome config`
and `esphome compile` on both, and the Python regression suite
(`tests/test_quietcool_esphome_config.py`) continues to assert their safety
invariants. To build one, provide a `secrets.yaml` in this directory
(`cp ../secrets.yaml.example secrets.yaml`, then edit) — ESPHome resolves
`!secret` and the `../components` / `../fonts` / `../images` paths against the
directory of the **top-level** config it is given, which for a direct
`esphome … legacy/quietcool-lora32.yaml` is this one.

## If you have your own wrapper for a second unit

Per-device wrapper configs are untracked, so this move did not touch yours. A
wrapper that packaged the old root-level base —

```yaml
packages:
  base: !include quietcool-lora32.yaml     # was at the repository root
```

— now points at nothing. Fixing the `!include` path alone is not enough:
**move the wrapper into this directory** and keep the include relative to it
(`!include quietcool-lora32.yaml`). Left at the repository root with an
`!include legacy/…`, the wrapper becomes the top-level config, so the base's
`../components`, `../fonts`, and `../images` resolve one level *above* the
repository and the build fails. Those paths are hard-coded here, not
substitution-indirected, so `legacy/` is the only place a legacy wrapper works.
Give the relocated wrapper a `secrets.yaml` in this directory too.

(The C++ base is different: `quietcool-cpp-lora32.yaml` parameterizes both
paths, so a wrapper for it may live anywhere that overrides
`cpp_components_root` and `assets_root`. See
[docs/deployment.md](../docs/deployment.md#where-the-wrapper-may-live).)

For everything else — features, learn mode, safety guidance — see the
[root README](../README.md).
