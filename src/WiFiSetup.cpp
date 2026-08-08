#include "WiFiSetup.h"

#include <Arduino.h>
#include <WiFiManager.h>
#include <cstring>

namespace {

// WiFiManager's save-params callback has no user-context parameter, so
// (matching esp32scoringdeviceMqtt's src/network.cpp) the parameters and
// the Settings they write into are file-scope statics.
Settings* g_settings = nullptr;

WiFiManagerParameter g_pisteNrParam("pisteNr", "Piste number", "1", 8);
WiFiManagerParameter g_pisteNameParam("pisteName", "Piste name (optional, e.g. Red)", "", 16);
WiFiManagerParameter g_mqttBrokerParam("mqttBroker", "MQTT Broker host", "", 64);

void saveParamsCallback() {
    if (!g_settings) return;
    g_settings->pisteNr = strtoul(g_pisteNrParam.getValue(), nullptr, 10);
    strncpy(g_settings->pisteName, g_pisteNameParam.getValue(),
            sizeof(g_settings->pisteName) - 1);
    strncpy(g_settings->mqttBroker, g_mqttBrokerParam.getValue(),
            sizeof(g_settings->mqttBroker) - 1);
    g_settings->save();
}

}  // namespace

bool WiFiSetup::begin(Settings& settings) {
    g_settings = &settings;
    char pisteNrStr[8];
    snprintf(pisteNrStr, sizeof(pisteNrStr), "%u", settings.pisteNr);
    g_pisteNrParam.setValue(pisteNrStr, 8);
    g_pisteNameParam.setValue(settings.pisteName, 16);
    g_mqttBrokerParam.setValue(settings.mqttBroker, 64);

    WiFiManager wm;
    wm.addParameter(&g_pisteNrParam);
    wm.addParameter(&g_pisteNameParam);
    wm.addParameter(&g_mqttBrokerParam);
    wm.setSaveParamsCallback(saveParamsCallback);
    wm.setParamsPage(true);
    wm.setConfigPortalTimeout(180);

    // Credentials already saved from a previous successful setup -- a
    // failed connect here almost always means the venue WiFi/router is
    // just temporarily unreachable, not that this device needs
    // reconfiguring. Bound the attempt and skip the blocking captive
    // portal on failure: falling into that would make the device
    // unreachable for up to 180s and then reboot-loop, defeating the
    // point of main.cpp's always-on remote-control AP, which exists
    // exactly to keep this device usable through this case.
    const bool hasSavedCreds = wm.getWiFiIsSaved();
    if (hasSavedCreds) {
        wm.setConnectTimeout(15);
        wm.setEnableConfigPortal(false);
    }

    const bool connected = wm.autoConnect("Favero_OPP2-setup");
    if (!connected && !hasSavedCreds) {
        // No credentials at all (first boot, or after resetAndReboot())
        // -- the portal is the only way in, so it already ran (blocking,
        // per setEnableConfigPortal's default) and still timed out
        // unconfigured. Reboot and retry the whole sequence rather than
        // running headless with no STA network and no way to configure
        // one either.
        ESP.restart();
    }
    return connected;
}

void WiFiSetup::resetAndReboot() {
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}
