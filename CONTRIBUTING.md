# Contributing

- Run the checks before opening a PR:
  ```bash
  cp secrets.yaml.example secrets.yaml
  .venv/bin/python -m unittest tests.test_quietcool_esphome_config -v
  .venv/bin/esphome config quietcool-lora32.yaml
  .venv/bin/esphome compile quietcool-lora32.yaml
  ```
- If a change touches shared radio, RX, safety, or display behavior, also run
  `config` and `compile` for `quietcool-lora-v3.yaml`. The V3 remains an
  unverified hardware port, so a successful compile is not evidence of RF
  parity.
- The four on-air command payloads (`90/9F/AF/BF`), the 3×/45 ms burst timing,
  and the "never transmit at boot/restore/from RX/from temperatures" invariants
  are covered by tests and must not regress.
- Keep protocol facts separate from shipped implementation status. Firmware
  analysis and live captures establish a fixed six-byte fan reply
  (`CB|CE`, exact three-byte ID suffix, duplicated state byte), lower-six-bit
  state comparison, capability metadata in bits 7:6, and the OEM `66 66`
  query flow. Both checked-in public YAMLs implement this closed-loop control.
- Changes to the closed loop must keep the validated spaced safety re-fire
  underneath query confirmation—never substitute a rapid retry loop. Keep
  `tx_burst` as the sole send point, correlate replies to a local post-command
  query, treat `CE` as non-confirming, enforce exact ID and duplicate-byte
  checks, compare reported state with the desired command, and bound both
  query and command attempts. Confirmed state, confirmation status, and speed
  capability must publish without allowing any RX path to trigger an unrelated
  transmission. Keep regression tests green for all of those
  invariants before documenting the feature as shipped.
- The live SX1278 research used an explicit 50 kHz RX bandwidth. The public
  SX1278 template currently omits `bandwidth` and therefore uses ESPHome's
  wider default; the V3 template explicitly uses an unverified 117.3 kHz
  setting. Do not describe either public file as using 50 kHz until its source,
  config validation, compile, and hardware verification agree.
- If you change the OLED, keep `tools/render_display.py` in sync (the
  `KEEP IN SYNC` tags) and regenerate `docs/display-previews/`.
- Do **not** commit `secrets.yaml` or any raw OEM firmware dump (`*.bin`).
