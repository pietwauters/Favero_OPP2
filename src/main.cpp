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

// Resolved OPP2 piste_id (Settings.pisteName if set, else Settings.pisteNr)
// and mDNS hostname, both set once in setup() and exposed via
// /api/settings-info so the settings page can show them -- otherwise
// there'd be no way to find a given board's address on a multi-piste LAN
// without a serial monitor.
char g_pisteId[OPP2::PISTE_ID_MAX] = "1";
char g_mdnsHostname[16]            = "piste_001";
char g_apSsid[32]                  = "Piste_001-remote";

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
    } else if (parsed.message_type == OPP2::MessageType::CONTROL) {
        OPP2::Control control;
        if (OPP2::Deserializer::deserialize(payload, length, control) ==
            OPP2::DeserializeError::OK) {
            g_opp2.handleSoftwareControl(control);
        }
    }
}

void onMqttConnect(bool /*sessionPresent*/) {
    g_opp2.publishAll();

    char topic[64];
    OPP2::TopicParser::buildFrom(g_pisteId, OPP2::Publisher::SOFTWARE,
                                 OPP2::MessageType::MATCH, topic, sizeof(topic));
    g_mqtt.subscribe(topic, 1);
    OPP2::TopicParser::buildFrom(g_pisteId, OPP2::Publisher::SOFTWARE,
                                 OPP2::MessageType::FENCERS, topic, sizeof(topic));
    g_mqtt.subscribe(topic, 1);
    OPP2::TopicParser::buildFrom(g_pisteId, OPP2::Publisher::SOFTWARE,
                                 OPP2::MessageType::CONTROL, topic, sizeof(topic));
    g_mqtt.subscribe(topic, 1);
}

// OPP2 piste_id: the friendly name if one is set, else the piste number --
// matches esp32scoringdeviceMqtt's Opp2Handler.cpp convention exactly, so
// this bridge looks like any other piste to the CMS, not something
// Favero-specific.
void buildPisteId(const Settings& settings, char* out, size_t outSize) {
    if (settings.pisteName[0] != '\0') {
        strncpy(out, settings.pisteName, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        snprintf(out, outSize, "%u", settings.pisteNr);
    }
}

// mDNS hostname and MQTT client ID are always purely numeric
// ("piste_007.local" / "Piste_007"), regardless of any friendly name --
// also matching esp32scoringdeviceMqtt's convention (its soft_ap_ssid and
// MQTT client ID, network.cpp/CyranoHandler.cpp, are both "Piste_%03d"
// from the number alone). Unlike the old free-text-based scheme, this
// needs no sanitizing -- a zero-padded number is always a legal DNS
// label and MQTT client ID on its own.
void buildMdnsHostname(uint32_t pisteNr, char* out, size_t outSize) {
    snprintf(out, outSize, "piste_%03u", pisteNr);
}

// SSID for the always-on remote-control AP -- same piste-number convention
// as buildMdnsHostname()/the MQTT client ID, with a "-remote" suffix so it
// reads clearly in a phone's WiFi picker as a distinct network from this
// piste's CMS-facing identity strings (which aren't SSIDs at all), and from
// the one-time-setup "Favero_OPP2-setup" SSID WiFiSetup.cpp uses.
void buildApSsid(uint32_t pisteNr, char* out, size_t outSize) {
    snprintf(out, outSize, "Piste_%03u-remote", pisteNr);
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
    OPP2::TopicParser::buildLwtTopic(g_pisteId, lwtTopic, sizeof(lwtTopic));

    g_mqtt.setServer(broker.toString().c_str(), g_settings.mqttPort);
    char clientId[16];
    snprintf(clientId, sizeof(clientId), "Piste_%03u", g_settings.pisteNr);
    g_mqtt.setClientId(clientId);
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
        request->send(200, "text/plain", "OK");
    });
}

void addOpp2Route(const char* path,
                   const std::function<void(AsyncWebServerRequest*)>& action) {
    g_server.on(path, HTTP_GET, [action](AsyncWebServerRequest* request) {
        action(request);
        request->send(200, "text/plain", "OK");
    });
}

// For lifecycle transitions only (Prev/Begin/Next/End): while the Favero's
// clock is actually running, a bout is live and no lifecycle transition
// makes sense -- rejected with 409 rather than silently applied, since
// Halt/Pause used to be exposed as separate manual buttons for exactly
// this kind of "the real state disagrees with what's requested" case, and
// removing them (see main.cpp history) means this guard is now the only
// thing standing between a stray click and a nonsense mid-bout state
// change. 409 body is shown to the user by opp2.html's action().
void addGuardedOpp2Route(const char* path,
                          const std::function<void(AsyncWebServerRequest*)>& action) {
    g_server.on(path, HTTP_GET, [action](AsyncWebServerRequest* request) {
        if (g_opp2.state().clock.running) {
            request->send(409, "text/plain", "Timer is running -- stop it first");
            return;
        }
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
    // Remote-control page -- same button set as esp32_FaveroRemoteControl.
    // Buttons call these via fetch() (index.html's faveroAction()), not
    // real navigation, so the four views can live in one document and the
    // Fullscreen API below doesn't get dropped by a page load on every tap.
    addFaveroIrRoute("/FaveroStartStop", []() { g_faveroIr.startStop(); });
    addFaveroIrRoute("/FaveroRearm", []() { g_faveroIr.rearm(); });
    addFaveroIrRoute("/FaveroPause", []() { g_faveroIr.pause(); });
    addFaveroIrRoute("/FaveroPlusLeft", []() { g_faveroIr.scorePlusLeft(); });
    addFaveroIrRoute("/FaveroReset", []() {
        g_faveroIr.reset();
        g_opp2.resetRedCards();       // "Mise a zero" -- a genuinely new bout
        g_opp2.armPostResetCleanup(); // Mise a zero doesn't clear yellow/priority itself
    });
    // Long-press Reset on the compact remote layout only (classic grid's
    // Mise a zero button above stays a single one-shot press) -- the full
    // Mise a zero -> confirm score 0 -> clear cards/priority -> Set
    // (confirm clock 3:00) -> MatchCount (confirm round 1) sequence, each
    // step confirmed via telemetry before advancing. See
    // Opp2StateOwner::armFullReset().
    addFaveroIrRoute("/FaveroFullReset", []() {
        g_faveroIr.reset();
        g_opp2.armFullReset();
    });
    addFaveroIrRoute("/FaveroPlusRight", []() { g_faveroIr.scorePlusRight(); });
    addFaveroIrRoute("/FaveroRedLeft", []() { g_faveroIr.redCardLeft(); });
    addFaveroIrRoute("/FaveroSet", []() { g_faveroIr.set(); });
    addFaveroIrRoute("/FaveroRedRight", []() { g_faveroIr.redCardRight(); });
    // Undo a mistakenly-given red card -- no equivalent button exists on
    // the real Favero remote at all (it can't un-give a card), so this is
    // purely a bridge-side correction: decrement the local count for the
    // carded side, and if there was actually one to undo, decrement the
    // *opposite* side's score via the same IR command the Minus buttons
    // already use, since a red card awarded that side a point.
    addFaveroIrRoute("/FaveroUndoRedLeft", []() {
        if (g_opp2.undoRedCard(OPP2::Side::LEFT)) g_faveroIr.scoreMinusRight();
    });
    addFaveroIrRoute("/FaveroUndoRedRight", []() {
        if (g_opp2.undoRedCard(OPP2::Side::RIGHT)) g_faveroIr.scoreMinusLeft();
    });
    // Once a side has an active red card, the Favero's yellow LED bit is
    // physically ambiguous (lit as the red card's own side effect, not a
    // real yellow -- see Opp2StateOwner::updateFromFavero()) and this
    // bridge already stops trusting/reporting it. Sending another yellow
    // toggle in that state would just flip that side-effect LED for no
    // meaningful reason, so it's rejected outright rather than silently
    // corrupting a state the referee can no longer visually distinguish
    // from a real yellow card. Bypasses addFaveroIrRoute (which always
    // returns 200) since this needs a conditional response, same reason
    // addGuardedOpp2Route exists as its own thing rather than a flag.
    g_server.on("/FaveroYellowLeft", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (g_opp2.state().score.left.red_cards > 0) {
            request->send(409, "text/plain", "Red card already given on this side");
            return;
        }
        g_faveroIr.yellowCardLeft();
        request->send(200, "text/plain", "OK");
    });
    addFaveroIrRoute("/FaveroMatchCount", []() { g_faveroIr.matchCount(); });
    g_server.on("/FaveroYellowRight", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (g_opp2.state().score.right.red_cards > 0) {
            request->send(409, "text/plain", "Red card already given on this side");
            return;
        }
        g_faveroIr.yellowCardRight();
        request->send(200, "text/plain", "OK");
    });
    addFaveroIrRoute("/FaveroMinusLeft", []() { g_faveroIr.scoreMinusLeft(); });
    addFaveroIrRoute("/FaveroPrioMan", []() { g_faveroIr.prioMan(); });
    addFaveroIrRoute("/FaveroMinusRight", []() { g_faveroIr.scoreMinusRight(); });
    addFaveroIrRoute("/FaveroBlock", []() { g_faveroIr.block(); });
    addFaveroIrRoute("/FaveroPrioCas", []() { g_faveroIr.prioCas(); });
    addFaveroIrRoute("/FaveroTeleAq", []() { g_faveroIr.teleAq(); });

    // OPP2 lifecycle -- local only, never touches IR (Favero has no
    // concept of match identity or lifecycle at all). Halt/Pause used to
    // be separate manual buttons here, but both are facts the Favero's
    // own telemetry reports (chrono running/not running), not choices a
    // human should inject from this UI -- see updateFromFavero(), which
    // already derives Fencing/Halt from the real clock and would just
    // fight a manual Halt/Pause press on the next telemetry frame anyway.
    // Removed rather than kept as dead buttons.
    addGuardedOpp2Route("/Opp2Next", [](AsyncWebServerRequest*) { g_opp2.nextMatch(); });
    addGuardedOpp2Route("/Opp2Prev", [](AsyncWebServerRequest*) { g_opp2.prevMatch(); });
    addGuardedOpp2Route("/Opp2Begin", [](AsyncWebServerRequest*) {
        g_opp2.setApparatusState(OPP2::ApparatusState::HALT);
    });
    addGuardedOpp2Route("/Opp2End", [](AsyncWebServerRequest*) { g_opp2.endMatch(); });
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

    // UW2F P-cards + passivity timer -- local/OPP2 state only, never IR:
    // the Favero has no way to display this at all (confirmed reason for
    // building it this way), it's purely for the app/repeaters/CMS. Not
    // guarded by addGuardedOpp2Route: giving a UW2F card happens mid-bout
    // while the clock is running, same as the existing red/yellow card
    // routes above. Single combined route (not per-side) -- the compact
    // layout's Penalties page gives mutual passivity to both fencers at
    // once with one button, per explicit design decision.
    addOpp2Route("/Opp2UW2FCardBoth", [](AsyncWebServerRequest*) {
        g_opp2.incrementPCard(OPP2::Side::LEFT);
        g_opp2.incrementPCard(OPP2::Side::RIGHT);
    });
    addOpp2Route("/Opp2UndoUW2FCardBoth", [](AsyncWebServerRequest*) {
        g_opp2.undoPCard(OPP2::Side::LEFT);
        g_opp2.undoPCard(OPP2::Side::RIGHT);
    });
    addOpp2Route("/Opp2RestoreUW2FTimer",
                  [](AsyncWebServerRequest*) { g_opp2.restoreUW2FTimer(); });

    // Black card -- local/OPP2 state only, same reasoning as the UW2F
    // P-cards above: no Favero IR command exists for this at all (no such
    // button on the physical remote), so unlike yellow/red there's
    // nothing to also send over IR here.
    addOpp2Route("/Opp2BlackCardLeft",
                  [](AsyncWebServerRequest*) { g_opp2.giveBlackCard(OPP2::Side::LEFT); });
    addOpp2Route("/Opp2BlackCardRight",
                  [](AsyncWebServerRequest*) { g_opp2.giveBlackCard(OPP2::Side::RIGHT); });
    addOpp2Route("/Opp2UndoBlackCardLeft",
                  [](AsyncWebServerRequest*) { g_opp2.undoBlackCard(OPP2::Side::LEFT); });
    addOpp2Route("/Opp2UndoBlackCardRight",
                  [](AsyncWebServerRequest*) { g_opp2.undoBlackCard(OPP2::Side::RIGHT); });

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
        char buf[672];
        snprintf(buf, sizeof(buf),
                 "{\"pisteNr\":%u,\"pisteName\":\"%s\",\"mqttBroker\":\"%s\",\"mqttPort\":%u,"
                 "\"wifiSsid\":\"%s\",\"wifiIp\":\"%s\",\"mdnsHostname\":\"%s\","
                 "\"apSsid\":\"%s\",\"apIp\":\"%s\",\"apPassword\":\"%s\","
                 "\"remoteLayout\":%u}",
                 g_settings.pisteNr, g_settings.pisteName, g_settings.mqttBroker,
                 g_settings.mqttPort, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                 g_mdnsHostname, g_apSsid, WiFi.softAPIP().toString().c_str(),
                 g_settings.apPassword, g_settings.remoteLayout);
        request->send(200, "application/json", buf);
    });
    g_server.on("/api/settings-save", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (request->hasParam("apPassword")) {
            const String pw = request->getParam("apPassword")->value();
            if (pw.length() < 8 || pw.length() > 63) {
                request->send(400, "text/plain", "AP password must be 8-63 characters");
                return;
            }
            strncpy(g_settings.apPassword, pw.c_str(), sizeof(g_settings.apPassword) - 1);
        }
        if (request->hasParam("pisteNr")) {
            g_settings.pisteNr = request->getParam("pisteNr")->value().toInt();
        }
        if (request->hasParam("pisteName")) {
            strncpy(g_settings.pisteName, request->getParam("pisteName")->value().c_str(),
                    sizeof(g_settings.pisteName) - 1);
        }
        if (request->hasParam("mqttBroker")) {
            strncpy(g_settings.mqttBroker, request->getParam("mqttBroker")->value().c_str(),
                    sizeof(g_settings.mqttBroker) - 1);
        }
        if (request->hasParam("mqttPort")) {
            g_settings.mqttPort = request->getParam("mqttPort")->value().toInt();
        }
        if (request->hasParam("remoteLayout")) {
            const int layout = request->getParam("remoteLayout")->value().toInt();
            g_settings.remoteLayout = (layout == 1) ? 1 : 0;
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
    // No Cache-Control by default meant browsers could skip revalidation
    // entirely on a normal reload and keep showing a stale page/style
    // after a data/ update was reflashed -- confirmed live (device served
    // the new repeater.html/style.css correctly; the browser just never
    // asked for it again). Static files are a few KB on a LAN, so
    // no-store's cost is negligible next to that footgun.
    g_server.serveStatic("/", SPIFFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("no-store");
}

}  // namespace

void setup() {
    Serial.begin(115200);

    g_settings.load();
    // Returns false (without blocking in the captive portal) if STA
    // credentials are already saved but currently unreachable -- see
    // WiFiSetup.h. Only genuinely blocks/reboot-loops when no credentials
    // exist at all. Either way, everything below (including the always-on
    // remote AP) still comes up regardless of the result.
    const bool staConnected = WiFiSetup::begin(g_settings);

    // Default modem-sleep power save adds a wake-up delay to every request
    // that lands after any idle gap -- this bridge needs to be responsive
    // for both the web UI and MQTT, not power-optimized.
    WiFi.setSleep(false);

    // WiFiManager's captive portal (when it runs) also listens on port 80
    // via the blocking WebServer class; its socket isn't guaranteed closed
    // the instant autoConnect() returns. Without this, ESPAsyncWebServer's
    // own bind() below can lose that race (AsyncTCP.cpp bind error: -8).
    delay(500);

    // Always-on remote-control AP, concurrent with STA. WiFiManager leaves
    // the device in plain WIFI_STA (AP off) after a successful
    // autoConnect() -- must switch mode here, never earlier: WiFiManager
    // manages WiFi mode internally during its own portal and would just
    // override an earlier call. softAP() never touches port 80, so this is
    // unrelated to the delay(500) race above (kept separate anyway to leave
    // that fix's job unambiguous).
    // NOT YET VERIFIED ON REAL HARDWARE: whether switching WIFI_STA ->
    // WIFI_AP_STA after STA is already associated preserves that
    // connection or forces a brief reconnect blip. Watch Serial/MQTT
    // connect behavior around this call on first real-device test.
    WiFi.mode(WIFI_AP_STA);
    if (strlen(g_settings.apPassword) < 8) {
        Serial.println("[ap] stored AP password invalid, using default");
        strncpy(g_settings.apPassword, "FaveroRemote1", sizeof(g_settings.apPassword) - 1);
    }
    buildApSsid(g_settings.pisteNr, g_apSsid, sizeof(g_apSsid));
    if (!WiFi.softAP(g_apSsid, g_settings.apPassword)) {
        Serial.println("[ap] softAP() failed to start");
    }
    Serial.printf("AP broadcasting: %s  IP: %s\n", g_apSsid,
                  WiFi.softAPIP().toString().c_str());

    buildMdnsHostname(g_settings.pisteNr, g_mdnsHostname, sizeof(g_mdnsHostname));
    MDNS.begin(g_mdnsHostname);
    if (staConnected) {
        Serial.printf("WiFi joined: %s  IP: %s  (also reachable at http://%s.local/)\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), g_mdnsHostname);
    } else {
        Serial.println("[wifi] STA not connected (venue WiFi unreachable?) -- "
                        "running on the remote-control AP only, retrying STA in background");
    }

    buildPisteId(g_settings, g_pisteId, sizeof(g_pisteId));
    g_opp2.begin(g_pisteId, &g_mqtt);
    g_faveroIr.begin();

    // Corrective IR sends Opp2StateOwner can't issue itself (no FaveroIR
    // dependency of its own -- see Opp2StateOwner.h) -- clearing a
    // yellow-card LED left over from a red card's side effect or from
    // FaveroReset not clearing it, and clearing an active priority that
    // FaveroReset also doesn't clear.
    g_opp2.setYellowClearCallback([](OPP2::Side side) {
        if (side == OPP2::Side::LEFT) {
            g_faveroIr.yellowCardLeft();
        } else {
            g_faveroIr.yellowCardRight();
        }
    });
    g_opp2.setPriorityClearCallback([]() { g_faveroIr.prioMan(); });
    // The two extra IR sends the long-press "full reset" sequence needs
    // (Opp2StateOwner::armFullReset()) -- same no-FaveroIR-dependency
    // reasoning as the two callbacks above.
    g_opp2.setSetCommandCallback([]() { g_faveroIr.set(); });
    g_opp2.setMatchCountCallback([]() { g_faveroIr.matchCount(); });

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

    // WiFiSetup::begin() may have returned with STA unconnected (saved
    // credentials but venue WiFi unreachable at boot -- see WiFiSetup.h)
    // rather than blocking forever. Nudge a reconnect periodically rather
    // than assuming the core's own auto-reconnect reliably covers a
    // connect that never succeeded in the first place (as opposed to a
    // live drop, which it does handle). Cheap no-op once actually
    // connected.
    static uint32_t s_lastWifiRetry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - s_lastWifiRetry > 30000) {
        s_lastWifiRetry = millis();
        WiFi.reconnect();
    }

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
