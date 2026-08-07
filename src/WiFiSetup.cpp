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

void WiFiSetup::begin(Settings& settings) {
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

    // Tries saved STA credentials first; only falls back to the blocking
    // AP + captive portal (SSID below) if that fails or nothing is saved
    // yet -- station mode is the primary path, since this device's whole
    // job is reaching an MQTT broker.
    const bool connected = wm.autoConnect("Favero_OPP2-setup");
    if (!connected) {
        // Portal ran and still never got connected (timed out) -- reboot
        // and try the whole sequence again rather than running headless
        // with no network at all.
        ESP.restart();
    }
}

void WiFiSetup::resetAndReboot() {
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
}
