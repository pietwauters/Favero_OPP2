// WiFi bring-up: station mode is primary (this device's whole purpose is
// reaching an MQTT broker), AP + captive config portal is a fallback used
// only when no known network can be joined. Built on tzapu/WiFiManager,
// same library esp32scoringdeviceMqtt uses for the equivalent problem --
// see its src/network.cpp for the fuller pattern this trims down.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "Settings.h"

namespace WiFiSetup {

/// Blocks until WiFi is up: tries saved STA credentials first; on failure
/// (or first boot), opens a "Favero_OPP2-setup" AP with a captive config
/// portal (WiFi credentials + piste ID + MQTT broker host) until the user
/// configures it. Updates `settings` and persists it if the portal was
/// used to change piste ID / MQTT broker.
void begin(Settings& settings);

/// Clears saved WiFi credentials and reboots -- next boot's begin() will
/// have nothing to auto-connect with and fall straight into the config
/// portal. Used by the Settings page's "Reconfigure WiFi" button.
void resetAndReboot();

}  // namespace WiFiSetup
