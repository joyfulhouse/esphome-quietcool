# Contributing

- Run the checks before opening a PR:
  ```bash
  make -C tests/cpp test            # C++ core + adapter host suites
  make -C tests/cpp test-sanitized  # the core suite only, under ASan/UBSan
  cp -n secrets.yaml.example secrets.yaml         # -n: never clobber real credentials
  cp -n secrets.yaml.example legacy/secrets.yaml  # ESPHome reads !secret from the config's own dir
  .venv/bin/python -m unittest tests.test_quietcool_esphome_config -v
  .venv/bin/esphome config quietcool-cpp-example.yaml
  .venv/bin/esphome compile quietcool-cpp-example.yaml
  .venv/bin/esphome config quietcool-cpp-lora32.yaml
  .venv/bin/esphome compile quietcool-cpp-lora32.yaml
  .venv/bin/esphome config quietcool-cpp-diag.yaml
  .venv/bin/esphome compile quietcool-cpp-diag.yaml
  .venv/bin/esphome config legacy/quietcool-lora32.yaml
  .venv/bin/esphome compile legacy/quietcool-lora32.yaml
  .venv/bin/esphome config legacy/quietcool-lora-v3.yaml
  .venv/bin/esphome compile legacy/quietcool-lora-v3.yaml
  ```
- `.github/workflows/ci.yml` runs `esphome config` on
  `quietcool-cpp-lora32.yaml` (the config people actually flash) and
  `quietcool-cpp-diag.yaml`, but never `esphome compile` on either — so the
  two `compile` lines above are the only thing that ever builds their display
  and entity lambdas. Run them. If you change the workflow, update the
  "What `.github/workflows/ci.yml` runs" table in the README to match; it is
  maintained by hand, because what the workflow file lists and what GitHub
  requires before a merge are different things and only the first is in this
  repository.
- The V3 remains an unverified hardware port, so successful config validation
  and compilation are not evidence of on-air RF parity.
- The on-air state commands (Off `90/A0/B0`, plus `9F/AF/BF`), the 3×/45 ms
  burst timing, and the "never transmit at boot/restore/from RX/from
  temperatures" invariants are covered by tests and must not regress.
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
  query and command attempts. The custom fan `control()` must not publish a
  requested state; any equivalent active request must not transmit or reset
  its budget. Preserve stale-command rejection at actual execution, the OEM
  exchange holdoff, previous-tail query quarantine, and mailbox poisoning.
- Preserve the explicit state-knowledge contract. ESPHome's native Fan API
  cannot represent unknown and exposes raw Off/Low defaults at initial
  subscription. Preserve the atomic `Fan Confirmed Off` diagnostic for HA
  interlocks; do not replace it with a cross-entity Fan/Known join whose API
  update order is not atomic. `Fan State Known` remains the entity-authority
  diagnostic, and `Timer State Known` gates the select/countdown. A new
  command and every actually executed non-query burst must invalidate both;
  correlated local-query consensus and the conservative passive fan-reply
  consensus path may set them. A passive command burst alone remains
  diagnostics-only; response-only consensus or a second independent ambiguous
  burst in the named reply window publishes through AuthorityStore. Outgoing
  commands must not optimistically arm/clear timer metadata. A countdown may
  be armed only from a confirmed locally initiated timer command; manual-query
  active-timer reports have unknown age, and estimated expiry must invalidate
  authority rather than publish guessed Off.
- `tx_burst` is intentionally bounded at `max_runs: 5`; ESPHome can reject an
  execution beyond capacity. Do not promise that every rapid press is queued or
  that none can be dropped. Preserve arm-before-enqueue and the idle-gated
  spaced re-fire so the latest desired transaction can converge after queue
  saturation. A pending quarantined query must not block a due spaced command
  re-fire; an active query window or unconsumed report still must.
- Keep regression tests green for all of these invariants before documenting a
  feature as shipped. The 2026-07-19 correction passes tests and config/compile
  validation for both public targets and was live-validated through a downstream
  SX1278 build, including three equivalent Off requests joining one bounded
  transaction. Do not generalize that result to SX1262 hardware: the V3 target
  compiles but has not been physically tested.
- The live and public SX1278 configurations use an explicit 50 kHz RX
  bandwidth. The SX1262 template uses 58.6 kHz, the nearest supported FSK
  bandwidth, but that radio target still awaits physical verification.
- If you change the OLED, keep `tools/render_display.py` in sync (the
  `KEEP IN SYNC` tags) and regenerate `docs/display-previews/`.
- Do **not** commit `secrets.yaml` or any raw OEM firmware dump (`*.bin`).
