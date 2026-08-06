#include "WiFiSetup.h"

#include <Arduino.h>
#include <WiFiManager.h>
#include <cstring>

namespace {

// WiFiManager's save-params callback has no user-context parameter, so
// (matching esp32scoringdeviceMqtt's src/network.cpp) the parameters and
// the Settings they write into are file-scope statics.
Settings* g_settings = nullptr;

WiFiManagerParameter g_pisteIdParam("pisteId", "Piste ID", "", 16);
WiFiManagerParameter g_mqttBrokerParam("mqttBroker", "MQTT Broker host", "", 64);

void saveParamsCallback() {
    if (!g_settings) return;
    strncpy(g_settings->pisteId, g_pisteIdParam.getValue(),
            sizeof(g_settings->pisteId) - 1);
    strncpy(g_settings->mqttBroker, g_mqttBrokerParam.getValue(),
            sizeof(g_settings->mqttBroker) - 1);
    g_settings->save();
}

}  // namespace

void WiFiSetup::begin(Settings& settings) {
    g_settings = &settings;
    g_pisteIdParam.setValue(settings.pisteId, 16);
    g_mqttBrokerParam.setValue(settings.mqttBroker, 64);

    WiFiManager wm;
    wm.addParameter(&g_pisteIdParam);
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
