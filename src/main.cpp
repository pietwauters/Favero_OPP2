// Favero_OPP2 — Favero data bridge.
//
// Reads a real Favero apparatus's 10-byte telemetry port, republishes it as
// a first-class OPP2 apparatus over MQTT, and commands the apparatus back
// via the same IR remote-control protocol as esp32_FaveroRemoteControl
// (github.com/pietwauters/esp32_FaveroRemoteControl). Match lifecycle has
// no Favero equivalent at all: apparatus_state/fencer-entry come from this
// device's own web UI, while next/prev match progression is delegated to
// whatever competition-management software is on the broker (standard
// OPP2 control-command handshake -- see Opp2StateOwner).
//
// SPDX-License-Identifier: MIT
#include <Arduino.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

#include "Esp32MqttClient.h"
#include "FaveroIR.h"
#include "FaveroSerialDecoder.h"
#include "Opp2StateOwner.h"
#include "Settings.h"
#include "WiFiSetup.h"

namespace {

constexpr uint8_t IR_PIN        = 4;   // unchanged from esp32_FaveroRemoteControl
constexpr int8_t  FAVERO_RX_PIN = 13;  // per hardware test device wiring
constexpr int8_t  FAVERO_TX_PIN = -1;  // unused -- Favero's port is output-only

Settings              g_settings;
FaveroIR              g_faveroIr(IR_PIN);
FaveroSerialDecoder   g_decoder;
Opp2StateOwner        g_opp2;
Esp32MqttClient       g_mqtt;

AsyncWebServer g_server(80);

// Deferred restart: HTTP handlers set this instead of calling ESP.restart()
// directly, so the response has a chance to flush to the client first.
enum class PendingAction { NONE, RESTART, WIFI_RESET };
PendingAction g_pendingAction  = PendingAction::NONE;
uint32_t      g_pendingActionAt = 0;

void onFaveroFrame(const FaveroFrame& frame, void* /*ctx*/) {
    g_opp2.updateFromFavero(frame);
}

// Mirrors software/match + software/fencers into local state -- this is
// how NEXT/PREV (which only ever publish a control command, never touch
// match state themselves) actually get real fencer/match data: whatever
// CMS is on the broker owns pool/tableau progression and pushes the
// answer back on these two topics.
void onMqttMessage(const char* topic, const char* payload, size_t length) {
    OPP2::Topic parsed;
    if (!OPP2::TopicParser::parse(topic, parsed)) return;
    if (parsed.publisher != OPP2::Publisher::SOFTWARE) return;

    if (parsed.message_type == OPP2::MessageType::MATCH) {
        OPP2::Match match;
        if (OPP2::Deserializer::deserialize(payload, length, match) ==
            OPP2::DeserializeError::OK) {
            g_opp2.handleSoftwareMatch(match);
        }
    } else if (parsed.message_type == OPP2::MessageType::FENCERS) {
        OPP2::Fencers fencers;
        if (OPP2::Deserializer::deserialize(payload, length, fencers) ==
            OPP2::DeserializeError::OK) {
            g_opp2.handleSoftwareFencers(fencers);
        }
    }
}

void onMqttConnect(bool /*sessionPresent*/) {
    g_opp2.publishAll();

    char topic[64];
    OPP2::TopicParser::buildFrom(g_settings.pisteId, OPP2::Publisher::SOFTWARE,
                                 OPP2::MessageType::MATCH, topic, sizeof(topic));
    g_mqtt.subscribe(topic, 1);
    OPP2::TopicParser::buildFrom(g_settings.pisteId, OPP2::Publisher::SOFTWARE,
                                 OPP2::MessageType::FENCERS, topic, sizeof(topic));
    g_mqtt.subscribe(topic, 1);
}

// Accepts either a literal IP or an mDNS "<name>.local" host, matching the
// OPP2 convention of defaulting to openpiste.local. esp_mqtt_client's own
// DNS resolution doesn't understand mDNS, so ".local" names are always
// pre-resolved to a literal IP before being handed to it.
IPAddress resolveBroker(const char* host) {
    IPAddress ip;
    if (ip.fromString(host)) return ip;

    String hostname(host);
    if (hostname.endsWith(".local")) {
        hostname.remove(hostname.length() - 6);
    }
    return MDNS.queryHost(hostname);
}

void setupMqtt() {
    IPAddress broker;
    for (int attempt = 0; attempt < 10; ++attempt) {
        broker = resolveBroker(g_settings.mqttBroker);
        if (broker != IPAddress(0, 0, 0, 0)) break;
        delay(500);
    }
    if (broker == IPAddress(0, 0, 0, 0)) {
        Serial.printf("[mqtt] could not resolve broker '%s' -- giving up for this boot\n",
                      g_settings.mqttBroker);
        return;
    }

    char lwtTopic[64];
    OPP2::TopicParser::buildLwtTopic(g_settings.pisteId, lwtTopic, sizeof(lwtTopic));

    g_mqtt.setServer(broker.toString().c_str(), g_settings.mqttPort);
    const String clientId = "Favero_OPP2-" + WiFi.macAddress();
    g_mqtt.setClientId(clientId.c_str());
    g_mqtt.setWill(lwtTopic, "{\"online\":false}", 1, true);
    g_mqtt.onConnect(onMqttConnect);
    g_mqtt.onMessage(onMqttMessage);
    g_mqtt.begin();  // esp_mqtt_client manages its own reconnects from here
}

void schedulePendingAction(PendingAction action) {
    g_pendingAction   = action;
    g_pendingActionAt = millis() + 300;  // let the HTTP response flush first
}

// ── Web routes ───────────────────────────────────────────────────────────

void addFaveroIrRoute(const char* path, void (*action)()) {
    g_server.on(path, HTTP_GET, [action](AsyncWebServerRequest* request) {
        action();
        request->redirect("/");
    });
}

void addOpp2Route(const char* path,
                   const std::function<void(AsyncWebServerRequest*)>& action) {
    g_server.on(path, HTTP_GET, [action](AsyncWebServerRequest* request) {
        action(request);
        request->send(200, "text/plain", "OK");
    });
}

OPP2::Weapon parseWeapon(const String& s) {
    if (s == "F") return OPP2::Weapon::FOIL;
    if (s == "E") return OPP2::Weapon::EPEE;
    if (s == "S") return OPP2::Weapon::SABRE;
    return OPP2::Weapon::UNKNOWN;
}

void setupWebServer() {
    // Remote-control page -- same button set as esp32_FaveroRemoteControl,
    // each a full-page GET (link, not fetch), so each handler redirects
    // back to "/" once the IR command has been sent.
    addFaveroIrRoute("/FaveroStartStop", []() { g_faveroIr.startStop(); });
    addFaveroIrRoute("/FaveroRearm", []() { g_faveroIr.rearm(); });
    addFaveroIrRoute("/FaveroPause", []() { g_faveroIr.pause(); });
    addFaveroIrRoute("/FaveroPlusLeft", []() { g_faveroIr.scorePlusLeft(); });
    addFaveroIrRoute("/FaveroReset", []() { g_faveroIr.reset(); });
    addFaveroIrRoute("/FaveroPlusRight", []() { g_faveroIr.scorePlusRight(); });
    addFaveroIrRoute("/FaveroRedLeft", []() { g_faveroIr.redCardLeft(); });
    addFaveroIrRoute("/FaveroSet", []() { g_faveroIr.set(); });
    addFaveroIrRoute("/FaveroRedRight", []() { g_faveroIr.redCardRight(); });
    addFaveroIrRoute("/FaveroYellowLeft", []() { g_faveroIr.yellowCardLeft(); });
    addFaveroIrRoute("/FaveroMatchCount", []() { g_faveroIr.matchCount(); });
    addFaveroIrRoute("/FaveroYellowRight", []() { g_faveroIr.yellowCardRight(); });
    addFaveroIrRoute("/FaveroMinusLeft", []() { g_faveroIr.scoreMinusLeft(); });
    addFaveroIrRoute("/FaveroPrioMan", []() { g_faveroIr.prioMan(); });
    addFaveroIrRoute("/FaveroMinusRight", []() { g_faveroIr.scoreMinusRight(); });
    addFaveroIrRoute("/FaveroBlock", []() { g_faveroIr.block(); });
    addFaveroIrRoute("/FaveroPrioCas", []() { g_faveroIr.prioCas(); });
    addFaveroIrRoute("/FaveroTeleAq", []() { g_faveroIr.teleAq(); });

    // OPP2 lifecycle -- local only, never touches IR (Favero has no
    // concept of match identity or lifecycle at all).
    addOpp2Route("/Opp2Next", [](AsyncWebServerRequest*) { g_opp2.nextMatch(); });
    addOpp2Route("/Opp2Prev", [](AsyncWebServerRequest*) { g_opp2.prevMatch(); });
    addOpp2Route("/Opp2Begin", [](AsyncWebServerRequest*) {
        g_opp2.setApparatusState(OPP2::ApparatusState::HALT);
    });
    addOpp2Route("/Opp2Halt", [](AsyncWebServerRequest*) {
        g_opp2.setApparatusState(OPP2::ApparatusState::HALT);
    });
    addOpp2Route("/Opp2Pause", [](AsyncWebServerRequest*) {
        g_opp2.setApparatusState(OPP2::ApparatusState::PAUSE);
    });
    addOpp2Route("/Opp2End", [](AsyncWebServerRequest*) {
        g_opp2.setApparatusState(OPP2::ApparatusState::ENDING);
    });
    addOpp2Route("/Opp2SetWeapon", [](AsyncWebServerRequest* request) {
        if (request->hasParam("weapon")) {
            g_opp2.setWeapon(parseWeapon(request->getParam("weapon")->value()));
        }
    });
    addOpp2Route("/Opp2SetFencer", [](AsyncWebServerRequest* request) {
        if (!request->hasParam("side")) return;
        const OPP2::Side side =
            request->getParam("side")->value() == "L" ? OPP2::Side::LEFT : OPP2::Side::RIGHT;
        const String name = request->hasParam("name") ? request->getParam("name")->value() : "";
        const String noc  = request->hasParam("noc") ? request->getParam("noc")->value() : "";
        g_opp2.setFencer(side, name.c_str(), noc.c_str());
    });

    // Live state for the OPP2 page's polling loop.
    g_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* request) {
        char buf[768];
        g_opp2.writeStateJson(buf, sizeof(buf));
        AsyncWebServerResponse* response =
            request->beginResponse(200, "application/json", buf);
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Settings page.
    // NOTE: these two paths must never be a literal string-prefix of one
    // another (e.g. "/api/settings" vs "/api/settings/save") -- this
    // ESPAsyncWebServer fork matches routes with a prefix check, not exact
    // equality, and whichever is registered first silently swallows every
    // request to the other. Confirmed by observation: pisteId writes were
    // being served by the read handler and never actually saved.
    g_server.on("/api/settings-info", HTTP_GET, [](AsyncWebServerRequest* request) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"pisteId\":\"%s\",\"mqttBroker\":\"%s\",\"mqttPort\":%u,"
                 "\"wifiSsid\":\"%s\",\"wifiIp\":\"%s\"}",
                 g_settings.pisteId, g_settings.mqttBroker, g_settings.mqttPort,
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        request->send(200, "application/json", buf);
    });
    g_server.on("/api/settings-save", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (request->hasParam("pisteId")) {
            strncpy(g_settings.pisteId, request->getParam("pisteId")->value().c_str(),
                    sizeof(g_settings.pisteId) - 1);
        }
        if (request->hasParam("mqttBroker")) {
            strncpy(g_settings.mqttBroker, request->getParam("mqttBroker")->value().c_str(),
                    sizeof(g_settings.mqttBroker) - 1);
        }
        if (request->hasParam("mqttPort")) {
            g_settings.mqttPort = request->getParam("mqttPort")->value().toInt();
        }
        g_settings.save();
        request->send(200, "text/plain", "OK -- rebooting");
        schedulePendingAction(PendingAction::RESTART);
    });
    g_server.on("/api/wifi/reset", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "text/plain", "OK -- rebooting into setup mode");
        schedulePendingAction(PendingAction::WIFI_RESET);
    });

    // Registered last deliberately: ESPAsyncWebServer checks handlers in
    // registration order, so every dynamic route above would otherwise
    // fail a SPIFFS lookup first before falling through to its real
    // handler -- measured as a consistent ~330ms gap between static file
    // serving and API routes.
    g_server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
}

}  // namespace

void setup() {
    Serial.begin(115200);

    g_settings.load();
    WiFiSetup::begin(g_settings);  // blocks until STA connected, or via AP portal

    // Default modem-sleep power save adds a wake-up delay to every request
    // that lands after any idle gap -- this bridge needs to be responsive
    // for both the web UI and MQTT, not power-optimized.
    WiFi.setSleep(false);

    // WiFiManager's captive portal (when it runs) also listens on port 80
    // via the blocking WebServer class; its socket isn't guaranteed closed
    // the instant autoConnect() returns. Without this, ESPAsyncWebServer's
    // own bind() below can lose that race (AsyncTCP.cpp bind error: -8).
    delay(500);

    MDNS.begin("favero-opp2");
    Serial.printf("WiFi joined: %s  IP: %s  (also reachable at http://favero-opp2.local/)\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    g_opp2.begin(g_settings.pisteId, &g_mqtt);
    g_faveroIr.begin();

    Serial2.begin(2400, SERIAL_8N1, FAVERO_RX_PIN, FAVERO_TX_PIN);
    g_decoder.setCallback(onFaveroFrame);

    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
    }

    setupWebServer();
    g_server.begin();
    setupMqtt();  // esp_mqtt_client runs its own task -- no polling needed from loop()

    Serial.println("Favero_OPP2 ready.");
}

void loop() {
    static uint32_t s_byteCount = 0;
    static uint32_t s_lastReport = 0;
    while (Serial2.available()) {
        g_decoder.feed(static_cast<uint8_t>(Serial2.read()));
        ++s_byteCount;
    }
    if (millis() - s_lastReport > 2000) {
        s_lastReport = millis();
        Serial.printf("[favero] %u bytes/2s  decoded=%u rejected=%u locked=%d\n",
                      s_byteCount, g_decoder.framesDecoded(), g_decoder.framesRejected(),
                      g_decoder.isLocked());
        s_byteCount = 0;
    }

    g_mqtt.loop();  // drains queued MQTT events -- see Esp32MqttClient::loop()

    if (g_pendingAction != PendingAction::NONE && millis() >= g_pendingActionAt) {
        const PendingAction action = g_pendingAction;
        g_pendingAction = PendingAction::NONE;
        if (action == PendingAction::RESTART) {
            ESP.restart();
        } else if (action == PendingAction::WIFI_RESET) {
            WiFiSetup::resetAndReboot();
        }
    }

    // Our app_main() runs this loop directly with no built-in yield (unlike
    // the standard Arduino core's own loopTask). Without this, "main" can
    // spin continuously and starve the lwIP/AsyncTCP task of CPU time to
    // service HTTP requests -- observed as ~0.5-1.2s time-to-first-byte on
    // every request despite sub-10ms ping RTT on the same link.
    delay(1);
}

// Required entry point when building with `framework = arduino, espidf`
// (Arduino as an ESP-IDF component) -- unlike plain `framework = arduino`,
// PlatformIO does not auto-generate this wrapper in that mode.
//
// setup()/loop() run on a dedicated task, NOT directly on "main" -- "main"
// only gets CONFIG_ESP_MAIN_TASK_STACK_SIZE (3584 bytes on this project),
// nowhere near enough for what loop() does via g_mqtt.loop() (JSON
// building/parsing, publishAll()'s seven synchronous publishes). Confirmed
// on real hardware: running loop() directly on "main" produced "stack
// overflow in task main", crash-reboot looping. Matches
// esp32scoringdeviceMqtt/src/main.cpp's exact pattern (RTOSSettings.h:
// STACK_ARDUINO_TASK=16384, PRIORITY_ARDUINO_TASK=3, CORE_ARDUINO_TASK=0),
// minus its dual-core sensor-isolation pinning (nothing here needs Core 1).
namespace {
constexpr uint32_t kArduinoTaskStack    = 16384;
constexpr UBaseType_t kArduinoTaskPrio  = 3;
constexpr BaseType_t  kArduinoTaskCore  = 0;

void arduinoTask(void*) {
    setup();
    while (true) {
        loop();
    }
}
}  // namespace

extern "C" void app_main() {
    initArduino();
    xTaskCreatePinnedToCore(arduinoTask, "arduino_task", kArduinoTaskStack, nullptr,
                            kArduinoTaskPrio, nullptr, kArduinoTaskCore);
}
