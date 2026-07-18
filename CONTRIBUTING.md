# Contributing

- Run the checks before opening a PR:
  ```bash
  cp secrets.yaml.example secrets.yaml
  .venv/bin/python -m unittest tests.test_quietcool_esphome_config -v
  .venv/bin/esphome config quietcool-lora32.yaml
  ```
- The four on-air command payloads (`90/9F/AF/BF`), the 3×/45 ms burst timing,
  and the "never transmit at boot/restore/from RX/from temperatures" invariants
  are covered by tests and must not regress.
- If you change the OLED, keep `tools/render_display.py` in sync (the
  `KEEP IN SYNC` tags) and regenerate `docs/display-previews/`.
- Do **not** commit `secrets.yaml` or any raw OEM firmware dump (`*.bin`).
