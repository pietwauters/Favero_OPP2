// Persisted device settings (NVS-backed via Preferences).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Piste identity matches esp32scoringdeviceMqtt's convention exactly
// (Opp2Handler.cpp): a numeric piste number always exists; an optional
// friendly name (e.g. "Red", "Podium"), when set, is used instead of the
// number wherever this device identifies itself to the CMS/broker (OPP2
// piste_id) -- but never for the MQTT client ID or mDNS hostname, which
// stay purely numeric ("Piste_007" / "piste_007.local") regardless of
// whether a friendly name is set, also matching that project.
struct Settings {
    uint32_t pisteNr          = 1;
    char     pisteName[16]    = "";                 // empty = use pisteNr
    char     mqttBroker[64]  = "openpiste.local";  // mDNS default, per OPP2 convention
    uint16_t mqttPort        = 1883;

    // WPA2 password for the always-on remote-control AP (Piste_%03u-remote,
    // see main.cpp). WPA2 requires 8-63 chars; this default is always valid
    // out of the box -- change it via the settings page.
    char apPassword[64] = "FaveroRemote1";

    void load();
    void save() const;
};
