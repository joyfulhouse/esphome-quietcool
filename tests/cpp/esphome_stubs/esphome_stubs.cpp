// Definitions for the host stubs. Real ESPHome defines global_preferences in
// esphome/core/preferences.cpp and points it at the platform NVS backend during
// startup; here it starts null so that "no preferences backend" — which the
// adapter must survive without failing the component — is the default state a
// test sees unless it installs one.

#include "esphome/core/preferences.h"

namespace esphome {

ESPPreferences* global_preferences = nullptr;

}  // namespace esphome
