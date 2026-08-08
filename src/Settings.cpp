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
    pisteNr = prefs.getUInt("pisteNr", pisteNr);
    String name = prefs.getString("pisteName", pisteName);
    String broker = prefs.getString("mqttBroker", mqttBroker);
    mqttPort = prefs.getUShort("mqttPort", mqttPort);
    String apPass = prefs.getString("apPassword", apPassword);
    remoteLayout = prefs.getUChar("remoteLayout", remoteLayout);
    prefs.end();

    strncpy(pisteName, name.c_str(), sizeof(pisteName) - 1);
    pisteName[sizeof(pisteName) - 1] = '\0';
    strncpy(mqttBroker, broker.c_str(), sizeof(mqttBroker) - 1);
    mqttBroker[sizeof(mqttBroker) - 1] = '\0';
    strncpy(apPassword, apPass.c_str(), sizeof(apPassword) - 1);
    apPassword[sizeof(apPassword) - 1] = '\0';
}

void Settings::save() const {
    Preferences prefs;
    prefs.begin(kNamespace, /*readOnly=*/false);
    prefs.putUInt("pisteNr", pisteNr);
    prefs.putString("pisteName", pisteName);
    prefs.putString("mqttBroker", mqttBroker);
    prefs.putUShort("mqttPort", mqttPort);
    prefs.putString("apPassword", apPassword);
    prefs.putUChar("remoteLayout", remoteLayout);
    prefs.end();
}
