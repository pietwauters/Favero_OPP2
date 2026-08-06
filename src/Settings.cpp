#include "Settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

namespace {
const char* kNamespace = "favero_opp2";
}

void Settings::load() {
    Preferences prefs;
    prefs.begin(kNamespace, /*readOnly=*/true);
    String piste = prefs.getString("pisteId", pisteId);
    String broker = prefs.getString("mqttBroker", mqttBroker);
    mqttPort = prefs.getUShort("mqttPort", mqttPort);
    prefs.end();

    strncpy(pisteId, piste.c_str(), sizeof(pisteId) - 1);
    pisteId[sizeof(pisteId) - 1] = '\0';
    strncpy(mqttBroker, broker.c_str(), sizeof(mqttBroker) - 1);
    mqttBroker[sizeof(mqttBroker) - 1] = '\0';
}

void Settings::save() const {
    Preferences prefs;
    prefs.begin(kNamespace, /*readOnly=*/false);
    prefs.putString("pisteId", pisteId);
    prefs.putString("mqttBroker", mqttBroker);
    prefs.putUShort("mqttPort", mqttPort);
    prefs.end();
}
