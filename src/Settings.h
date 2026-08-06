// Persisted device settings (NVS-backed via Preferences).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

struct Settings {
    char     pisteId[16]     = "1";
    char     mqttBroker[64]  = "openpiste.local";  // mDNS default, per OPP2 convention
    uint16_t mqttPort        = 1883;

    void load();
    void save() const;
};
