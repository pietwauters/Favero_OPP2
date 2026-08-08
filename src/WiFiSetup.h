// WiFi bring-up: station mode is primary (this device's whole purpose is
// reaching an MQTT broker), AP + captive config portal is a fallback used
// only when no known network can be joined at all. Built on
// tzapu/WiFiManager, same library esp32scoringdeviceMqtt uses for the
// equivalent problem -- see its src/network.cpp for the fuller pattern
// this trims down.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "Settings.h"

namespace WiFiSetup {

/// Brings up STA WiFi and returns once it's resolved one way or another --
/// does NOT always mean connected, see below. Behavior depends on whether
/// STA credentials are already saved:
///  - No saved credentials (first boot, or after resetAndReboot()): this
///    is the only way in, so it blocks in a "Favero_OPP2-setup" AP +
///    captive config portal (WiFi credentials + piste ID + MQTT broker
///    host) until configured, rebooting to retry the whole sequence if the
///    portal times out unconfigured.
///  - Saved credentials exist: bounds the connect attempt (~15s) and
///    returns false without opening the portal if it fails, rather than
///    blocking/reboot-looping -- a failed connect here almost always means
///    the venue WiFi/router is just temporarily unreachable, not that this
///    device needs reconfiguring, and main.cpp's always-on remote-control
///    AP exists specifically to keep the device usable through exactly
///    this case. Returns true if it did connect within the bound.
bool begin(Settings& settings);

/// Clears saved WiFi credentials and reboots -- next boot's begin() will
/// have nothing to auto-connect with and fall straight into the config
/// portal. Used by the Settings page's "Reconfigure WiFi" button.
void resetAndReboot();

}  // namespace WiFiSetup
