# Stage-2 toolchain verification — 2026-07-24

Independent verification of the Stage-2 deliverable (thin ESPHome adapter + both
radio adapters) against the real ESPHome 2025.11.5 toolchain. Codex was
prohibited from running `esphome`; this verification was performed by the
reviewer. All five checks from the Stage-2 report now pass.

## Results

| Check | Result |
|---|---|
| `esphome config quietcool-cpp-example.yaml` | valid |
| SX127x compile (`quietcool-cpp-example.yaml`, ttgo-lora32-v21) | success, `config_hash=0x9543762b` |
| SX126x compile (`quietcool-cpp-example-sx126x.yaml`, heltec_wifi_lora_32_V3) | success, `config_hash=0xeebca593` |
| Variant define exclusivity | SX127x build defines only `QUIETCOOL_USE_SX127X`; SX126x build only `QUIETCOOL_USE_SX126X` |
| Generated-code ownership | exactly one `quietcool.on_packet` consumer, one `QuietCoolComponent`, zero legacy `cl_*` symbols |

Host suite after all fixes: **146/146**. Design contract md5 unchanged
(`fa82c72a10f66af09900d2f365edbd9a`).

The SX126x example was constructed strictly by applying the SX127x example's own
documented selection steps — the guidance was sufficient as written.

## Packaging defects found by the real toolchain (fixed by the reviewer)

Codex's syntax-checks against installed ESPHome headers could not see any of
these; all four only surface in a real `esphome compile`. This is why the
toolchain gate is reserved for the reviewer side.

1. **Relative `-I` breaks under ESP-IDF.** The codegen's
   `cg.add_build_flag("-Isrc/quietcool")` resolves under PlatformIO (compiler
   cwd = project root) but not under ESP-IDF (different build cwd), so every
   cross-directory include failed. Fix: all cross-directory includes are now
   **src-rooted** (`#include "quietcool/core/..."`), the convention ESPHome's
   own components use — resolves from the build's `src/` include root under
   both toolchains with no flags. The flag was removed; the host test Makefile
   include root moved from `components/quietcool` to `components`.

2. **Subdirectory headers never reach `main.cpp`.** ESPHome auto-includes only
   top-level external-component headers; this component's C++ lives entirely in
   subdirectories, so generated code had no declaration of any quietcool type.
   Fix: the class-declaring codegen modules add their headers explicitly via
   `cg.add_global(cg.RawStatement(...))` — component/automation/both radio
   adapters in `__init__.py` (the adapter headers self-guard on the variant
   defines, so both are safe), fan in `fan.py`, button in `button.py`.

3. **Namespace collision.** In ESPHome 2025.11, `cg.esphome_ns` is literally
   `global_ns`, so codegen rendered bare `quietcool::` references. Generated
   `main.cpp` does `using namespace esphome;`, making bare `quietcool` ambiguous
   between the platform-free core (`::quietcool`) and the adapters
   (`esphome::quietcool`). Fix: `quietcool_ns` is declared fully qualified
   (`cg.global_ns.namespace("esphome").namespace("quietcool")`) so generated
   references are unambiguous.

4. **Scoped-enum rendering.** `QuietCoolButtonKind` is an `enum class`, but
   `button.py` declared it without `is_class=True`, so codegen emitted
   `esphome::quietcool::Refresh` instead of
   `esphome::quietcool::QuietCoolButtonKind::Refresh`. One-argument fix.

## Design-doc follow-ups (not yet applied — the contract was not edited)

- Codex's Stage-2 report recommends either retaining the `esphome.includes`
  directory bridge or revising §5 packaging; the bridge is retained and now
  documented in `__init__.py`. A future design revision should fold the
  src-rooted include convention and the explicit-header-injection requirement
  into §5/§16 so the packaging rules are contractual.
- Codex's recommendation for an explicit reset-rejected core event (§13) is
  still open.

## Not validated here (hardware-only, per §19/§20)

FSK interoperability with the fan, physical FIFO/burst-completion semantics,
measured 45 ms gaps, SX126x TCXO/RF-switch behavior, real RX callback behavior,
driver-fault recovery, NVS persistence across reboot. Next boundary is Stage 3
(receive-only shadow on a canary device), which requires explicit user
authorization before anything is flashed.
