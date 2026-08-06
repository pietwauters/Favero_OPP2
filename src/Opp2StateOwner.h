// Single source of truth for this bridge's OPP2 state -- mirrors the
// Opp2Handler SSOT pattern from esp32scoringdeviceMqtt (one owner, publish
// on every accepted change), simplified: everything here runs from the
// Arduino loop() on one core, so no mutex is needed.
//
// Three distinct kinds of update come in:
//   - updateFromFavero(): score/clock/lights/priority/cards, decoded from
//     the real apparatus's telemetry. Favero is authoritative for these;
//     this bridge never predicts or overrides them.
//   - setApparatusState()/setFencer(): match lifecycle and identity that
//     Favero has no concept of at all. These come from this device's own
//     web UI.
//   - nextMatch()/prevMatch(): standard OPP2 apparatus behavior, per spec
//     (Section 19) -- publish a `control` command (NEXT/PREV) and let the
//     competition-management software decide what "next" means and push
//     back the real match/fencers data. This bridge never guesses at pool
//     progression itself; handleSoftwareMatch()/handleSoftwareFencers()
//     mirror whatever the CMS sends in response (see main.cpp's MQTT
//     subscription to software/match + software/fencers).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <opp2.h>

#include "Esp32MqttClient.h"
#include "FaveroSerialDecoder.h"

class Opp2StateOwner {
public:
    void begin(const char* pisteId, Esp32MqttClient* mqtt);

    void updateFromFavero(const FaveroFrame& frame);

    void setApparatusState(OPP2::ApparatusState state);
    void setWeapon(OPP2::Weapon weapon);
    void setFencer(OPP2::Side side, const char* name, const char* noc);

    /// Requests the next/previous match from the CMS by publishing a
    /// control command (NEXT/PREV) -- does not touch local match state at
    /// all. Whatever the CMS pushes back arrives via handleSoftwareMatch()/
    /// handleSoftwareFencers().
    void nextMatch();
    void prevMatch();

    /// Called from main.cpp's MQTT callback when a software/match or
    /// software/fencers message arrives -- mirrors it into local state
    /// AND relays it under apparatus/match + apparatus/fencers, so other
    /// devices on this piste (displays, repeaters) get correct data by
    /// subscribing only to apparatus/* -- the same topic tree they already
    /// need for lights/score/clock, and the only one every device is
    /// guaranteed able to read (software/* may be ACL-restricted).
    void handleSoftwareMatch(const OPP2::Match& match);
    void handleSoftwareFencers(const OPP2::Fencers& fencers);

    /// Publishes connection (LWT counterpart) -- call with true once MQTT
    /// connects, the broker publishes {"online":false} itself via LWT on
    /// unexpected disconnect.
    void publishConnection(bool online);

    /// Republishes every retained topic -- call once after (re)connecting
    /// to MQTT so a fresh/reconnected broker has the full current state.
    void publishAll();

    const OPP2::SystemState& state() const { return m_state; }

    /// Fills buf with a JSON snapshot of current state for the web UI's
    /// GET /api/state polling endpoint. Not an OPP2 wire message -- just
    /// whatever the UI needs to render.
    void writeStateJson(char* buf, size_t bufSize) const;

private:
    void publish(OPP2::Publisher pub, OPP2::MessageType mt, const char* json, int qos,
                 bool retained);

    void publishScore();
    void publishClock();
    void publishLights();
    void publishApparatusState();
    void publishMatch();
    void publishFencers();
    void publishUW2F();
    void publishControl(OPP2::Command command);

    /// Derives uw2f.time_ms: elapsed wall time since the last valid hit,
    /// accruing only while `running` (the Favero-reported clock state) is
    /// true. Favero has no concept of this at all -- see updateFromFavero().
    void tickUW2F(bool running, bool hitOccurred);

    static OPP2::Priority derivePriority(const FaveroFrame& f);
    static bool checkSerialize(OPP2::SerializeError err, const char* what);

    Esp32MqttClient* m_mqtt = nullptr;
    uint32_t         m_seq  = 0;

    OPP2::SystemState m_state;
    char              m_payload[OPP2::JSON_SIZE_MAX] = {};
    char              m_topic[64]                    = {};

    // UW2F passivity timer bookkeeping (millis()-based, independent of
    // Favero's own clock resolution).
    uint32_t m_uw2fBaseMs     = 0;      // banked elapsed ms from prior running segments
    uint32_t m_uw2fRunSinceMs = 0;      // millis() when the current running segment started
    bool     m_uw2fRunning    = false;  // running state as of the last tick
};
